#include "auth.h"
#include "log.hpp"
#include "qrcodegen.hpp"
#include "status.h"

#include <config.hpp>
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#define CURL_STATICLIB
#include <curl/curl.h>
#include <jpeglib.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>
#include <ctime>
#include <cstring>
#include <mutex>
#include <setjmp.h>
#include <string>
#include <thread>
#include <vector>

struct Label {
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
};

static SDL_Texture* makeQrTexture(SDL_Renderer* renderer, const std::string& text) {
    if (text.empty()) return nullptr;
    try {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize();
        const int quiet = 4;
        const int span = n + quiet * 2;
        const int scale = std::max(1, 360 / span);
        const int dim = span * scale;
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, dim, dim, 32, SDL_PIXELFORMAT_RGBA32);
        if (!surface) return nullptr;
        SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 255, 255, 255, 255));
        const Uint32 dark = SDL_MapRGBA(surface->format, 16, 16, 18, 255);
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                if (!qr.getModule(x, y)) continue;
                SDL_Rect px{(x + quiet) * scale, (y + quiet) * scale, scale, scale};
                SDL_FillRect(surface, &px, dark);
            }
        }
        SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_FreeSurface(surface);
        return tex;
    } catch (...) {
        return nullptr;
    }
}

static bool ttfMagic(const u8* p, size_t n) {
    return n >= 4 && p[0] == 0x00 && p[1] == 0x01 && p[2] == 0x00 && p[3] == 0x00;
}

static TTF_Font* openSystemFont(int px) {
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)))
        return nullptr;

    const u8* data = static_cast<const u8*>(font.address);
    size_t size = font.size;
    // ponytail: shared font is sometimes BFTTF (8-byte header + TTF). Drop the header if present.
    if (!ttfMagic(data, size) && size > 8 && ttfMagic(data + 8, size - 8)) {
        data += 8;
        size -= 8;
    }
    return TTF_OpenFontRW(SDL_RWFromConstMem(data, static_cast<int>(size)), 1, px);
}

static Label makeLabel(SDL_Renderer* renderer, TTF_Font* font, const char* text, SDL_Color color) {
    Label label;
    if (!font || !text || !text[0]) return label;
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return label;
    label.tex = SDL_CreateTextureFromSurface(renderer, surface);
    label.w = surface->w;
    label.h = surface->h;
    SDL_FreeSurface(surface);
    return label;
}

static void drawLabel(SDL_Renderer* renderer, const Label& label, int x, int y) {
    if (!label.tex) return;
    SDL_Rect dst{x, y, label.w, label.h};
    SDL_RenderCopy(renderer, label.tex, nullptr, &dst);
}

static void destroyLabel(Label& label) {
    if (label.tex) SDL_DestroyTexture(label.tex);
    label.tex = nullptr;
}

struct ServerRow {
    std::string titleId;
    std::string fullName;
    Label name;
    Label status;
    Label number;
    Label word;
    int players = -1;
    int kind = -1;
    SDL_Texture* icon = nullptr;
    bool iconDone = false;
};

static void setText(Label& label, SDL_Renderer* renderer, TTF_Font* font, const char* text, SDL_Color color) {
    destroyLabel(label);
    label = makeLabel(renderer, font, text, color);
}

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpegExit(j_common_ptr cinfo) {
    longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1);
}

static SDL_Texture* textureFromRgb(SDL_Renderer* renderer, int w, int h, const unsigned char* rgb) {
    if (!renderer || w <= 0 || h <= 0 || w > 2048 || h > 2048 || !rgb) return nullptr;
    std::vector<unsigned char> rgba(static_cast<size_t>(w) * h * 4);
    const int rad = std::max(1, std::min(w, h) / 7);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int dx = 0, dy = 0;
            if (x < rad) dx = rad - x;
            else if (x >= w - rad) dx = x - (w - rad - 1);
            if (y < rad) dy = rad - y;
            else if (y >= h - rad) dy = y - (h - rad - 1);
            unsigned char a = 255;
            if (dx || dy) {
                const int d2 = dx * dx + dy * dy;
                const int r2 = rad * rad;
                if (d2 >= r2) a = 0;
                else if (rad > 1 && d2 > (rad - 1) * (rad - 1)) a = 110;
            }
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const size_t s = (static_cast<size_t>(y) * w + x) * 3;
            rgba[i] = rgb[s];
            rgba[i + 1] = rgb[s + 1];
            rgba[i + 2] = rgb[s + 2];
            rgba[i + 3] = a;
        }
    }
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    if (SDL_UpdateTexture(tex, nullptr, rgba.data(), w * 4) != 0) {
        SDL_DestroyTexture(tex);
        return nullptr;
    }
    return tex;
}

static SDL_Texture* textureFromJpeg(SDL_Renderer* renderer, const std::string& jpeg) {
    if (jpeg.size() < 4) return nullptr;
    jpeg_decompress_struct cinfo{};
    JpegError err{};
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpegExit;
    // ponytail: longjmp skips C++ destructors if a later decode error fires after locals exist.
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, reinterpret_cast<const unsigned char*>(jpeg.data()), static_cast<unsigned long>(jpeg.size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    const int w = static_cast<int>(cinfo.output_width);
    const int h = static_cast<int>(cinfo.output_height);
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char* row = rgb.data() + static_cast<size_t>(cinfo.output_scanline) * w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return textureFromRgb(renderer, w, h, rgb.data());
}

static void fillRound(SDL_Renderer* renderer, SDL_Rect box, int rad) {
    if (box.w <= 0 || box.h <= 0) return;
    if (rad > box.w / 2) rad = box.w / 2;
    if (rad > box.h / 2) rad = box.h / 2;
    if (rad <= 0) {
        SDL_RenderFillRect(renderer, &box);
        return;
    }
    SDL_Rect mid{box.x, box.y + rad, box.w, box.h - rad * 2};
    if (mid.h > 0) SDL_RenderFillRect(renderer, &mid);
    for (int i = 0; i < rad; ++i) {
        const int dy = rad - i;
        const int dx = static_cast<int>(std::sqrt(std::max(0, rad * rad - dy * dy)));
        const int rowW = box.w - 2 * (rad - dx);
        const int rowX = box.x + (rad - dx);
        SDL_Rect top{rowX, box.y + i, rowW, 1};
        SDL_Rect bottom{rowX, box.y + box.h - 1 - i, rowW, 1};
        SDL_RenderFillRect(renderer, &top);
        SDL_RenderFillRect(renderer, &bottom);
    }
}

static bool inRound(int x, int y, SDL_Rect box, int rad) {
    if (x < box.x || y < box.y || x >= box.x + box.w || y >= box.y + box.h) return false;
    const int left = box.x + rad;
    const int right = box.x + box.w - 1 - rad;
    const int top = box.y + rad;
    const int bottom = box.y + box.h - 1 - rad;
    int cx = x;
    int cy = y;
    if (x < left && y < top) { cx = left; cy = top; }
    else if (x > right && y < top) { cx = right; cy = top; }
    else if (x < left && y > bottom) { cx = left; cy = bottom; }
    else if (x > right && y > bottom) { cx = right; cy = bottom; }
    else return true;
    const int dx = x - cx;
    const int dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

static void conicColor(float cssDeg, int& r, int& g, int& b) {
    float t = std::fmod(cssDeg - 135.f, 360.f);
    if (t < 0) t += 360.f;
    struct Stop { float at; int r, g, b; };
    static const Stop stops[] = {
        {0.f, 255, 255, 255},
        {70.f, 255, 90, 101},
        {180.f, 168, 123, 255},
        {270.f, 69, 196, 255},
        {360.f, 255, 255, 255},
    };
    const Stop* a = &stops[0];
    const Stop* c = &stops[1];
    for (int i = 0; i < 4; ++i) {
        if (t >= stops[i].at && t <= stops[i + 1].at) {
            a = &stops[i];
            c = &stops[i + 1];
            break;
        }
    }
    const float span = c->at - a->at;
    const float u = span <= 0.f ? 0.f : (t - a->at) / span;
    r = static_cast<int>(a->r + (c->r - a->r) * u);
    g = static_cast<int>(a->g + (c->g - a->g) * u);
    b = static_cast<int>(a->b + (c->b - a->b) * u);
}

static void glowRing(SDL_Renderer* renderer, SDL_Rect box, int rad, float time) {
    const int pad = 3;
    const int thick = 4;
    SDL_Rect outer{box.x - pad, box.y - pad, box.w + pad * 2, box.h + pad * 2};
    SDL_Rect inner{outer.x + thick, outer.y + thick, outer.w - thick * 2, outer.h - thick * 2};
    int outerRad = rad + pad;
    int innerRad = std::max(0, outerRad - thick);
    if (outerRad > outer.w / 2) outerRad = outer.w / 2;
    if (outerRad > outer.h / 2) outerRad = outer.h / 2;
    if (innerRad > inner.w / 2) innerRad = inner.w / 2;
    if (innerRad > inner.h / 2) innerRad = inner.h / 2;
    const float pulse = 0.72f + 0.28f * (0.5f + 0.5f * std::sin(time * 2.2f));
    const Uint8 alpha = static_cast<Uint8>(255.f * pulse);
    const float cx = box.x + box.w * 0.5f;
    const float cy = box.y + box.h * 0.5f;
    for (int y = outer.y; y < outer.y + outer.h; ++y) {
        for (int x = outer.x; x < outer.x + outer.w; ++x) {
            if (!inRound(x, y, outer, outerRad) || inRound(x, y, inner, innerRad)) continue;
            const float deg = std::atan2((x + 0.5f) - cx, cy - (y + 0.5f)) * 57.2957795f;
            int r, g, b;
            conicColor(deg - time * 40.f, r, g, b);
            SDL_SetRenderDrawColor(renderer, r, g, b, alpha);
            SDL_RenderDrawPoint(renderer, x, y);
        }
    }
}

static void fillDisk(SDL_Renderer* renderer, int cx, int cy, int rad) {
    for (int y = -rad; y <= rad; ++y)
        for (int x = -rad; x <= rad; ++x)
            if (x * x + y * y <= rad * rad)
                SDL_RenderDrawPoint(renderer, cx + x, cy + y);
}

static const char* kUpdateRepo = "jean-louis1776/nextendo-status";

struct CurlStop {
    std::atomic<bool>* stop = nullptr;
    std::atomic<curl_off_t>* got = nullptr;
    std::atomic<curl_off_t>* total = nullptr;
    std::atomic<int>* speed = nullptr;
    curl_off_t markBytes = 0;
    u64 markTick = 0;
};

static int curlAbort(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* gate = static_cast<CurlStop*>(userdata);
    if (gate->got) gate->got->store(dlnow);
    if (gate->total) gate->total->store(dltotal);
    const u64 now = armGetSystemTick();
    if (gate->markTick == 0) {
        gate->markTick = now;
        gate->markBytes = dlnow;
    } else {
        const u64 dt = now - gate->markTick;
        const u64 freq = armGetSystemTickFreq();
        if (freq > 0 && dt > freq / 4) {
            const curl_off_t delta = dlnow - gate->markBytes;
            int perSec = 0;
            if (delta > 0) perSec = static_cast<int>((delta * static_cast<curl_off_t>(freq)) / static_cast<curl_off_t>(dt));
            if (gate->speed) gate->speed->store(perSec);
            gate->markTick = now;
            gate->markBytes = dlnow;
        }
    }
    return gate->stop && gate->stop->load() ? 1 : 0;
}

static CURL* curlOpen(const std::string& url, CurlStop* gate, long timeout) {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlAbort);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, gate);
    return curl;
}

static bool curlToString(const std::string& url, std::string& body, CurlStop* gate) {
    body.clear();
    CURL* curl = curlOpen(url, gate, 20);
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* out = static_cast<std::string*>(userdata);
        const size_t n = size * nmemb;
        if (out->size() + n > 262144) return 0;
        out->append(ptr, n);
        return n;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && http == 200;
}

static bool curlToFile(const std::string& url, const std::string& path, CurlStop* gate) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    CURL* curl = curlOpen(url, gate, 60);
    if (!curl) {
        std::fclose(file);
        std::remove(path.c_str());
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    std::fclose(file);
    if (rc != CURLE_OK || http != 200) {
        std::remove(path.c_str());
        return false;
    }
    return true;
}

static std::vector<int> versionParts(std::string text) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.erase(text.begin());
    std::vector<int> parts;
    size_t i = 0;
    while (i < text.size() && parts.size() < 3) {
        int n = 0;
        bool any = false;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            n = n * 10 + (text[i] - '0');
            ++i;
            any = true;
        }
        if (!any) break;
        parts.push_back(n);
        if (i < text.size() && text[i] == '.') ++i;
        else break;
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

static std::string jsonString(const std::string& body, const char* key) {
    const std::string token = std::string("\"") + key + "\":\"";
    auto pos = body.find(token);
    size_t skip = token.size();
    if (pos == std::string::npos) {
        const std::string spaced = std::string("\"") + key + "\": \"";
        pos = body.find(spaced);
        skip = spaced.size();
    }
    if (pos == std::string::npos) return {};
    pos += skip;
    const auto end = body.find('"', pos);
    if (end == std::string::npos) return {};
    return body.substr(pos, end - pos);
}

static bool replaceNro(const std::string& tmp, const std::string& dest);

static bool endsWith(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

static bool copyExact(FILE* in, const std::string& path, uint32_t size) {
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    char buf[8192];
    bool ok = true;
    while (size > 0 && ok) {
        const size_t chunk = size > sizeof buf ? sizeof buf : size;
        ok = std::fread(buf, 1, chunk, in) == chunk && std::fwrite(buf, 1, chunk, out) == chunk;
        size -= static_cast<uint32_t>(chunk);
    }
    std::fclose(out);
    if (!ok) std::remove(path.c_str());
    return ok;
}

static uint32_t readU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t readU16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static bool installZip(const std::string& zipPath, const std::string& nroPath) {
    FILE* in = std::fopen(zipPath.c_str(), "rb");
    if (!in) return false;
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/.overlays", 0777);
    bool nro = false;
    while (true) {
        unsigned char header[30];
        if (std::fread(header, 1, 30, in) != 30) break;
        if (readU32(header) != 0x04034b50) break;
        if (readU16(header + 8) != 0) {
            std::fclose(in);
            return false;
        }
        const uint32_t size = readU32(header + 18);
        const uint16_t nameLen = readU16(header + 26);
        const uint16_t extraLen = readU16(header + 28);
        std::string name(nameLen, '\0');
        if (nameLen != 0 && std::fread(name.data(), 1, nameLen, in) != nameLen) {
            std::fclose(in);
            return false;
        }
        if (extraLen != 0 && std::fseek(in, extraLen, SEEK_CUR) != 0) {
            std::fclose(in);
            return false;
        }
        std::string dest;
        bool isNro = false;
        if (endsWith(name, "nexstatus.nro")) {
            dest = nroPath + ".new";
            isNro = true;
        } else if (endsWith(name, "nexstatus.ovl")) {
            dest = "sdmc:/switch/.overlays/nexstatus.ovl.new";
        }
        if (dest.empty()) {
            if (std::fseek(in, size, SEEK_CUR) != 0) {
                std::fclose(in);
                return false;
            }
            continue;
        }
        if (!copyExact(in, dest, size)) {
            std::fclose(in);
            return false;
        }
        if (isNro) {
            if (!replaceNro(dest, nroPath)) {
                std::fclose(in);
                return false;
            }
            nro = true;
        } else if (std::rename(dest.c_str(), "sdmc:/switch/.overlays/nexstatus.ovl") != 0) {
            std::remove("sdmc:/switch/.overlays/nexstatus.ovl");
            if (std::rename(dest.c_str(), "sdmc:/switch/.overlays/nexstatus.ovl") != 0) {
                std::fclose(in);
                return false;
            }
        }
    }
    std::fclose(in);
    std::remove(zipPath.c_str());
    return nro;
}

static bool replaceNro(const std::string& tmp, const std::string& dest) {
    FILE* in = std::fopen(tmp.c_str(), "rb");
    FILE* out = std::fopen(dest.c_str(), "r+b");
    if (!out) out = std::fopen(dest.c_str(), "wb");
    if (!in || !out) {
        if (in) std::fclose(in);
        if (out) std::fclose(out);
        return false;
    }
    char buf[8192];
    bool ok = true;
    while (ok) {
        const size_t n = std::fread(buf, 1, sizeof buf, in);
        if (n == 0) break;
        ok = std::fwrite(buf, 1, n, out) == n;
    }
    if (std::ferror(in) || std::fflush(out) != 0) ok = false;
    std::fclose(in);
    std::fclose(out);
    if (ok) std::remove(tmp.c_str());
    return ok;
}

static std::string downloadStats(curl_off_t got, curl_off_t total, int bytesPerSec) {
    int pct = 0;
    if (total > 0) {
        pct = static_cast<int>((got * 100) / total);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    char rate[24] = "0 B/s";
    if (bytesPerSec >= 1024 * 1024)
        std::snprintf(rate, sizeof rate, "%.1f MB/s", bytesPerSec / (1024.0 * 1024.0));
    else if (bytesPerSec >= 1024)
        std::snprintf(rate, sizeof rate, "%d KB/s", bytesPerSec / 1024);
    else if (bytesPerSec > 0)
        std::snprintf(rate, sizeof rate, "%d B/s", bytesPerSec);
    char line[48];
    std::snprintf(line, sizeof line, "%d%%  %s", pct, rate);
    return line;
}

static void checkUpdate(std::string nroPath, std::atomic<bool>* stop, std::mutex* mu, std::string* note, std::atomic<int>* phase, std::atomic<bool>* install, std::atomic<curl_off_t>* got, std::atomic<curl_off_t>* total, std::atomic<int>* speed) {
    CurlStop gate{stop, got, total, speed};
    std::string body;
    const std::string api = std::string("https://api.github.com/repos/") + kUpdateRepo + "/releases/latest";
    if (!stop->load() && curlToString(api, body, &gate)) {
        const std::string tag = jsonString(body, "tag_name");
        if (!tag.empty() && versionParts(tag) > versionParts(APP_VERSION)) {
            {
                std::lock_guard<std::mutex> lock(*mu);
                *note = "Updated to " + tag;
            }
            phase->store(1);
            const std::string url = std::string("https://github.com/") + kUpdateRepo + "/releases/download/" + tag + "/nexstatus.zip";
            while (!stop->load() && phase->load() != 3) {
                while (!stop->load() && !install->load())
                    svcSleepThread(50'000'000);
                if (stop->load()) break;
                install->store(false);
                got->store(0);
                total->store(0);
                speed->store(0);
                gate.markBytes = 0;
                gate.markTick = 0;
                phase->store(2);
                const std::string tmp = nroPath + ".zip";
                const bool replaced = !stop->load() && curlToFile(url, tmp, &gate) && installZip(tmp, nroPath);
                phase->store(replaced ? 3 : 4);
            }
        }
    }
}

static std::string consoleClock() {
    u64 ts = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts)) &&
        R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &ts)))
        ts = static_cast<u64>(time(nullptr));
    const time_t t = static_cast<time_t>(ts);
    tm parts{};
    gmtime_r(&t, &parts);
    char buf[16];
    if (!strftime(buf, sizeof buf, "%H:%M:%S", &parts)) return "--:--:--";
    return buf;
}

static std::string fitText(TTF_Font* font, const std::string& text, int maxW) {
    int w = 0, h = 0;
    if (TTF_SizeUTF8(font, text.c_str(), &w, &h) != 0 || w <= maxW) return text;
    std::string cut = text;
    while (!cut.empty()) {
        cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80)
            cut.pop_back();
        const std::string trial = cut + "...";
        if (TTF_SizeUTF8(font, trial.c_str(), &w, &h) == 0 && w <= maxW) return trial;
    }
    return "...";
}

struct ChipView {
    Label name;
    Label mark;
    Label extra;
    int w = 0;
    int h = 0;
    bool pal = false;
    bool mine = false;
};

struct CardView {
    Label title;
    Label mode;
    Label state;
    Label places;
    std::vector<ChipView> members;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int headH = 0;
    bool wrapMode = false;
    bool open = false;
    bool full = false;
};

struct LiveDraw {
    Label headline;
    Label note;
    Label lobbyHead;
    Label playerHead;
    std::vector<CardView> cards;
    std::vector<ChipView> players;
    int playersY = 0;
    int height = 0;
};

static void destroyChip(ChipView& chip) {
    destroyLabel(chip.name);
    destroyLabel(chip.mark);
    destroyLabel(chip.extra);
}

static void destroyLive(LiveDraw& live) {
    destroyLabel(live.headline);
    destroyLabel(live.note);
    destroyLabel(live.lobbyHead);
    destroyLabel(live.playerHead);
    for (auto& card : live.cards) {
        destroyLabel(card.title);
        destroyLabel(card.mode);
        destroyLabel(card.state);
        destroyLabel(card.places);
        for (auto& chip : card.members) destroyChip(chip);
    }
    for (auto& chip : live.players) destroyChip(chip);
    live = {};
}

static ChipView makeChip(SDL_Renderer* renderer, TTF_Font* nameFont, TTF_Font* tagFont, const LiveChip& src) {
    ChipView chip;
    chip.pal = src.mark == "Friend";
    chip.mine = src.mark == "You";
    const SDL_Color white{255, 255, 255, 255};
    const SDL_Color green{150, 230, 176, 255};
    const SDL_Color gray{176, 176, 184, 255};
    chip.name = makeLabel(renderer, nameFont, src.name.c_str(), white);
    if (!src.mark.empty()) chip.mark = makeLabel(renderer, tagFont, src.mark.c_str(), chip.pal || chip.mine ? green : gray);
    if (!src.extra.empty()) chip.extra = makeLabel(renderer, tagFont, src.extra.c_str(), gray);
    chip.h = std::max(chip.name.h, std::max(chip.mark.h, chip.extra.h)) + 14;
    chip.w = 14 + chip.name.w + 14;
    if (chip.mark.tex) chip.w += 8 + chip.mark.w;
    if (chip.extra.tex) chip.w += 8 + chip.extra.w;
    return chip;
}

static int flowHeight(const std::vector<ChipView>& chips, int width) {
    if (chips.empty() || width <= 0) return 0;
    int x = 0;
    int rows = 1;
    const int rowH = chips[0].h;
    for (const ChipView& chip : chips) {
        const int need = std::min(chip.w, width);
        if (x > 0 && x + need > width) {
            x = 0;
            ++rows;
        }
        x += need + 8;
    }
    return rows * rowH + (rows - 1) * 8;
}

static void drawChips(SDL_Renderer* renderer, const std::vector<ChipView>& chips, int x0, int y0, int width, const SDL_Rect& view) {
    int x = 0;
    int y = y0;
    const int rowH = chips.empty() ? 0 : chips[0].h;
    for (const ChipView& chip : chips) {
        const int need = std::min(chip.w, width);
        if (x > 0 && x + need > width) {
            x = 0;
            y += rowH + 8;
        }
        if (y + rowH >= view.y && y <= view.y + view.h) {
            SDL_Rect box{x0 + x, y, need, rowH};
            SDL_SetRenderDrawColor(renderer, chip.pal || chip.mine ? 36 : 54, chip.pal || chip.mine ? 72 : 54, chip.pal || chip.mine ? 52 : 62, 255);
            fillRound(renderer, box, rowH / 2);
            int tx = box.x + 14;
            const int ty = box.y + (rowH - chip.name.h) / 2;
            drawLabel(renderer, chip.name, tx, ty);
            tx += chip.name.w;
            if (chip.mark.tex) {
                tx += 8;
                drawLabel(renderer, chip.mark, tx, box.y + (rowH - chip.mark.h) / 2);
                tx += chip.mark.w;
            }
            if (chip.extra.tex) {
                tx += 8;
                drawLabel(renderer, chip.extra, tx, box.y + (rowH - chip.extra.h) / 2);
            }
        }
        x += need + 8;
    }
}

static void buildLive(LiveDraw& live, SDL_Renderer* renderer, TTF_Font* font, TTF_Font* hintFont, TTF_Font* tagFont, const LivePage& page, int width) {
    destroyLive(live);
    const SDL_Color white{255, 255, 255, 255};
    const SDL_Color hint{230, 230, 235, 255};
    const SDL_Color green{150, 230, 176, 255};
    const SDL_Color amber{255, 186, 140, 255};
    if (!page.headline.empty()) live.headline = makeLabel(renderer, hintFont, page.headline.c_str(), white);
    if (!page.note.empty()) live.note = makeLabel(renderer, hintFont, fitText(hintFont, page.note, width).c_str(), hint);
    const int cols = page.lobbies.size() > 1 ? 2 : 1;
    const int gap = 12;
    const int cardW = cols == 2 ? (width - gap) / 2 : width;
    for (size_t i = 0; i < page.lobbies.size(); ++i) {
        const LiveLobby& src = page.lobbies[i];
        CardView card;
        card.w = cardW;
        card.open = src.state == "Open";
        card.full = src.state == "Full";
        card.title = makeLabel(renderer, hintFont, ("Lobby " + std::to_string(i + 1)).c_str(), white);
        if (!src.mode.empty()) card.mode = makeLabel(renderer, tagFont, fitText(tagFont, src.mode, cardW - 32).c_str(), hint);
        if (!src.state.empty())
            card.state = makeLabel(renderer, tagFont, src.state.c_str(), card.open ? green : card.full ? amber : hint);
        if (!src.places.empty()) card.places = makeLabel(renderer, hintFont, src.places.c_str(), white);
        const int used = card.title.w + 16 + card.state.w + 16 + card.places.w;
        card.wrapMode = card.mode.tex && used + 16 + card.mode.w + 32 > cardW;
        card.headH = card.wrapMode ? card.title.h + 8 + card.mode.h : std::max(card.title.h, std::max(card.mode.h, card.places.h));
        for (const LiveChip& member : src.members) card.members.push_back(makeChip(renderer, hintFont, tagFont, member));
        const int chipsH = flowHeight(card.members, cardW - 32);
        card.h = 16 + card.headH + (chipsH ? 12 + chipsH : 0) + 16;
        live.cards.push_back(std::move(card));
    }
    int y = 0;
    if (live.headline.tex) y += live.headline.h + 8;
    if (live.note.tex) y += live.note.h + 16;
    if (!live.cards.empty()) {
        const std::string head = "Lobbies (" + std::to_string(live.cards.size()) + ")";
        live.lobbyHead = makeLabel(renderer, font, head.c_str(), white);
        y += live.lobbyHead.h + 12;
        for (size_t i = 0; i < live.cards.size(); ++i) {
            const int col = static_cast<int>(i % cols);
            if (col == 0) {
                int rowH = live.cards[i].h;
                if (cols == 2 && i + 1 < live.cards.size()) rowH = std::max(rowH, live.cards[i + 1].h);
                live.cards[i].y = y;
                if (cols == 2 && i + 1 < live.cards.size()) live.cards[i + 1].y = y;
                y += rowH + gap;
            }
            live.cards[i].x = col * (cardW + gap);
        }
    }
    if (!page.players.empty()) {
        live.playerHead = makeLabel(renderer, font, ("Players (" + std::to_string(page.players.size()) + ")").c_str(), white);
        y += 8 + live.playerHead.h + 12;
        live.playersY = y;
        for (const LiveChip& src : page.players) live.players.push_back(makeChip(renderer, hintFont, tagFont, src));
        y += flowHeight(live.players, width);
    }
    live.height = y;
}

static void drawLive(SDL_Renderer* renderer, const LiveDraw& live, int x0, int y0, int width, const SDL_Rect& view) {
    int y = y0;
    if (live.headline.tex) {
        drawLabel(renderer, live.headline, x0, y);
        y += live.headline.h + 8;
    }
    if (live.note.tex) {
        drawLabel(renderer, live.note, x0, y);
        y += live.note.h + 16;
    }
    if (live.lobbyHead.tex) {
        drawLabel(renderer, live.lobbyHead, x0, y);
        y += live.lobbyHead.h + 12;
    }
    for (const CardView& card : live.cards) {
        SDL_Rect box{x0 + card.x, y0 + card.y, card.w, card.h};
        if (box.y + box.h < view.y || box.y > view.y + view.h) continue;
        SDL_SetRenderDrawColor(renderer, 34, 34, 40, 255);
        fillRound(renderer, box, 20);
        int tx = box.x + 16;
        int ty = box.y + 16;
        drawLabel(renderer, card.title, tx, ty + (card.wrapMode ? 0 : (card.headH - card.title.h) / 2));
        tx += card.title.w + 16;
        if (!card.wrapMode && card.mode.tex) {
            drawLabel(renderer, card.mode, tx, ty + (card.headH - card.mode.h) / 2);
            tx += card.mode.w + 16;
        }
        if (card.state.tex) drawLabel(renderer, card.state, tx, ty + (card.headH - card.state.h) / 2);
        if (card.places.tex)
            drawLabel(renderer, card.places, box.x + card.w - 16 - card.places.w, ty + (card.wrapMode ? 0 : (card.headH - card.places.h) / 2));
        if (card.wrapMode && card.mode.tex) drawLabel(renderer, card.mode, box.x + 16, ty + card.title.h + 8);
        if (!card.members.empty()) drawChips(renderer, card.members, box.x + 16, box.y + 16 + card.headH + 12, card.w - 32, view);
    }
    if (live.playerHead.tex) drawLabel(renderer, live.playerHead, x0, y0 + live.playersY - live.playerHead.h - 12);
    if (!live.players.empty()) drawChips(renderer, live.players, x0, y0 + live.playersY, width, view);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (R_FAILED(plInitialize(PlServiceType_User)))
        return 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) {
        SDL_Quit();
        plExit();
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("nexstatus", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
    SDL_Renderer* renderer = window
        ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
        : nullptr;
    if (renderer) SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    TTF_Font* font = renderer ? openSystemFont(34) : nullptr;
    TTF_Font* codeFont = renderer ? openSystemFont(64) : nullptr;
    TTF_Font* hintFont = renderer ? openSystemFont(24) : nullptr;
    TTF_Font* creditFont = renderer ? openSystemFont(18) : nullptr;
    timeInitialize();

    const SDL_Color white{255, 255, 255, 255};
    const SDL_Color hint{230, 230, 235, 255};
    const SDL_Color credit{145, 145, 150, 255};

    Label leftHint = makeLabel(renderer, hintFont, "(A) Select   (-) Credits   (+) Exit", hint);
    Label creditTitle = makeLabel(renderer, font, "Credits", white);
    Label creditWho = makeLabel(renderer, hintFont, "ILALEX (SW-8382-3644-7463)", white);
    Label creditRole = makeLabel(renderer, hintFont, "Author", hint);
    Label creditNx = makeLabel(renderer, hintFont, "Nextendo", white);
    Label creditThanks = makeLabel(renderer, hintFont, "Thanks for providing the API.", hint);
    Label rightHint = makeLabel(renderer, creditFont, APP_VERSION, credit);
    PublicStatus servers;
    std::vector<ServerRow> serverRows;
    Label bannerTitle;
    Label bannerSub;
    Label bannerTime;
    Label updateTitle;
    Label updateBody;
    int serverRev = -1;
    float serverScroll = 0;
    float serverShown = 0;
    u64 listTick = 0;
    int focus = -1;
    bool focusPlaced = false;
    int detail = -1;
    float detailScroll = 0;
    float detailShown = 0;
    u64 detailTick = 0;
    bool pairing = false;
    bool account = false;
    bool credits = false;
    SDL_Texture* qrTex = nullptr;
    std::string qrFor;
    std::string pairNote;
    Label pairTitle = makeLabel(renderer, font, "Sign in", white);
    Label pairHelp = makeLabel(renderer, hintFont, "Scan the QR code with your phone.", hint);
    Label pairCode;
    Label pairStatus;
    Label loginLabel = makeLabel(renderer, hintFont, "Sign in", white);
    Label loginName;
    Label friendLabel;
    Label detailPrompt = makeLabel(renderer, hintFont, "Sign in to see lobby and player information.", hint);
    Label detailStatus;
    SDL_Texture* avatar = nullptr;
    LiveDraw liveDraw;
    std::string detailKey;
    NextendoAuth auth;
    AppConfig config;
    config.load(kSharedConfigPath);
    std::mutex liveMu;
    std::string liveBody;
    std::string liveError;
    bool liveFailed = false;
    std::atomic<bool> liveDone{true};
    std::atomic<bool> liveStop{false};
    std::atomic<u64> liveTick{0};
    std::thread liveThread;
    std::atomic<bool> profileDone{config.access_token.empty()};
    std::thread profileThread;
    auth.useTokens(config.access_token, config.refresh_token);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    socketInitializeDefault();
    sessionLog("start " APP_VERSION);
    bool batteryOn = R_SUCCEEDED(psmInitialize());
    int batteryPct = -1;
    Label batteryLabel;
    if (!config.access_token.empty()) {
        profileThread = std::thread([&auth, token = config.access_token, &profileDone] {
            auth.loadProfile(token);
            profileDone.store(true);
        });
    }
    u64 moveTick = 0;
    int hintMode = -1;
    std::atomic<bool> updateStop{false};
    std::atomic<bool> updateInstall{false};
    std::atomic<int> updatePhase{0};
    std::atomic<curl_off_t> updateGot{0};
    std::atomic<curl_off_t> updateTotal{0};
    std::atomic<int> updateSpeed{0};
    Label progressStats;
    std::string progressShown;
    std::mutex updateMu;
    std::string updateText;
    int updateShown = 0;
    std::string nroPath = (argc > 0 && argv && argv[0] && std::string(argv[0]).find(".nro") != std::string::npos)
        ? argv[0] : "sdmc:/switch/nexstatus.nro";
    servers.start();
    std::thread updateThread(checkUpdate, std::move(nroPath), &updateStop, &updateMu, &updateText, &updatePhase, &updateInstall, &updateGot, &updateTotal, &updateSpeed);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    while (renderer && appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const int phaseNow = updatePhase.load();
        servers.hold(phaseNow != 0);
        if (phaseNow == 3 && (down & HidNpadButton_A))
            break;
        if ((phaseNow == 1 || phaseNow == 4) && (down & HidNpadButton_A))
            updateInstall = true;
        if (phaseNow != 2 && (down & HidNpadButton_Plus))
            break;

        int ww = 1280, wh = 720;
        SDL_GetRendererOutputSize(renderer, &ww, &wh);
        if (ww <= 0 || wh <= 0) {
            ww = 1280;
            wh = 720;
        }

        const int barH = 60;
        const int barY = wh - barH;

        if (servers.ready() && !servers.failed()) {
            const int rev = servers.revision();
            if (rev != serverRev) {
                const auto games = servers.snapshot();
                std::string keptDetail;
                std::string keptFocus;
                if (detail >= 0 && detail < static_cast<int>(serverRows.size())) keptDetail = serverRows[detail].titleId;
                if (focus >= 0 && focus < static_cast<int>(serverRows.size())) keptFocus = serverRows[focus].titleId;
                const SDL_Color onlineColor{120, 196, 130, 255};
                const SDL_Color offlineColor{214, 96, 108, 255};
                const SDL_Color quietColor{160, 160, 166, 255};
                const int nameMax = ((ww - 16 * 2 - 10) / 2) - 230;
                bool same = serverRows.size() == games.size();
                for (size_t i = 0; same && i < games.size(); ++i)
                    same = serverRows[i].titleId == games[i].titleId;
                if (!same) {
                    for (auto& row : serverRows) {
                        destroyLabel(row.name);
                        destroyLabel(row.status);
                        destroyLabel(row.number);
                        destroyLabel(row.word);
                        if (row.icon) SDL_DestroyTexture(row.icon);
                    }
                    serverRows.assign(games.size(), {});
                }
                int active = 0, playing = 0;
                bool countsChanged = !bannerTime.tex;
                for (size_t i = 0; i < games.size(); ++i) {
                    const int kind = games[i].online ? 0 : (games[i].nostatus ? 1 : 2);
                    if (kind != 2) ++active;
                    if (games[i].online) playing += games[i].players;
                    ServerRow& row = serverRows[i];
                    if (row.titleId != games[i].titleId) {
                        row.titleId = games[i].titleId;
                        row.fullName = games[i].name;
                        row.iconDone = false;
                        if (row.icon) SDL_DestroyTexture(row.icon);
                        row.icon = nullptr;
                        const std::string fitted = fitText(hintFont, games[i].name, nameMax);
                        setText(row.name, renderer, hintFont, fitted.c_str(), white);
                    }
                    if (row.kind != kind || row.players != games[i].players) {
                        countsChanged = true;
                        row.kind = kind;
                        row.players = games[i].players;
                        const SDL_Color statusColor = kind == 0 ? onlineColor : (kind == 1 ? quietColor : offlineColor);
                        const char* statusText = kind == 0 ? "Online" : (kind == 1 ? "No status" : "Offline");
                        const char* word = "";
                        if (games[i].online)
                            word = games[i].rooms
                                ? (games[i].players == 1 ? "player in rooms" : "players in rooms")
                                : (games[i].players == 1 ? "player" : "players");
                        const std::string number = games[i].online ? std::to_string(games[i].players) : "-";
                        setText(row.status, renderer, hintFont, statusText, statusColor);
                        setText(row.number, renderer, font, number.c_str(), white);
                        setText(row.word, renderer, hintFont, word, hint);
                    }
                    if (row.iconDone || games[i].jpeg.size() < 4) continue;
                    row.icon = textureFromJpeg(renderer, games[i].jpeg);
                    row.iconDone = true;
                }
                const std::string head = std::to_string(playing) + (playing == 1 ? " player" : " players");
                const std::string sub = std::to_string(active) + (active == 1 ? " server active" : " servers active");
                setText(bannerTitle, renderer, font, head.c_str(), white);
                setText(bannerSub, renderer, hintFont, sub.c_str(), hint);
                if (countsChanged) {
                    const std::string stamp = "Updated at " + consoleClock();
                    setText(bannerTime, renderer, hintFont, stamp.c_str(), hint);
                }
                if (!keptDetail.empty()) {
                    detail = -1;
                    for (int i = 0; i < static_cast<int>(serverRows.size()); ++i)
                        if (serverRows[i].titleId == keptDetail) { detail = i; break; }
                    detailKey.clear();
                }
                if (!keptFocus.empty()) {
                    const int oldFocus = focus;
                    int found = -1;
                    for (int i = 0; i < static_cast<int>(serverRows.size()); ++i)
                        if (serverRows[i].titleId == keptFocus) { found = i; break; }
                    if (found >= 0 && oldFocus >= 0) {
                        const float delta = static_cast<float>(found / 2 - oldFocus / 2) * 122.f;
                        serverScroll += delta;
                        serverShown += delta;
                    }
                    focus = found >= 0 ? found : 0;
                    const int rows = (static_cast<int>(serverRows.size()) + 1) / 2;
                    const float content = rows > 0 ? rows * 122.f - 10.f : 0.f;
                    const float view = static_cast<float>(barY - (16 + 84 + 12) - 24);
                    const float maxScroll = std::max(0.f, content - view);
                    if (serverScroll < 0) serverScroll = 0;
                    if (serverScroll > maxScroll) serverScroll = maxScroll;
                    if (serverShown < 0) serverShown = 0;
                    if (serverShown > maxScroll) serverShown = maxScroll;
                }
                serverRev = rev;
            }
        }

        if (liveDone.load() && liveThread.joinable()) {
            liveThread.join();
            if (config.access_token.empty()) {
                liveBody.clear();
                liveError.clear();
                liveFailed = false;
                liveTick = 0;
            } else if (!auth.accessToken().empty() && auth.accessToken() != config.access_token) {
                config.access_token = auth.accessToken();
                config.refresh_token = auth.refreshToken();
                config.saveShared();
            }
        }
        if (profileDone.load() && profileThread.joinable()) {
            profileThread.join();
            setText(loginName, renderer, hintFont, auth.displayName().empty() ? "Signed in" : auth.displayName().c_str(), white);
            const std::string codeLine = auth.friendCode().empty() ? "" : "Friend code  " + auth.friendCode();
            setText(friendLabel, renderer, hintFont, codeLine.c_str(), hint);
            if (auth.accessToken() != config.access_token && !auth.accessToken().empty()) {
                config.access_token = auth.accessToken();
                config.refresh_token = auth.refreshToken();
                config.saveShared();
            }
        }

        const bool browsing = phaseNow == 0 && detail < 0;
        if (!focusPlaced && !serverRows.empty()) { focus = 0; focusPlaced = true; }
        if (browsing && !serverRows.empty() && focus >= static_cast<int>(serverRows.size())) focus = 0;
        if (phaseNow == 0 && account) {
            if (down & HidNpadButton_B) account = false;
            else if (down & HidNpadButton_X) {
                config.access_token.clear();
                config.refresh_token.clear();
                config.saveShared();
                auth.useTokens({}, {});
                destroyLabel(loginName);
                destroyLabel(friendLabel);
                liveBody.clear();
                liveError.clear();
                liveFailed = false;
                liveTick = 0;
                detailKey.clear();
                account = false;
            }
        } else if (phaseNow == 0 && pairing) {
            if (down & HidNpadButton_B) {
                auth.cancelPair();
                pairing = false;
            }
            std::string access, refresh;
            if (auth.takeTokens(access, refresh)) {
                config.access_token = access;
                config.refresh_token = refresh;
                config.saveShared();
                auth.loadProfile(config.access_token);
                setText(loginName, renderer, hintFont, auth.displayName().empty() ? "Signed in" : auth.displayName().c_str(), white);
                const std::string codeLine = auth.friendCode().empty() ? "" : "Friend code  " + auth.friendCode();
                setText(friendLabel, renderer, hintFont, codeLine.c_str(), hint);
                liveBody.clear();
                liveTick = 0;
                pairing = false;
            }
        } else if (phaseNow == 0) {
            const u64 held = padGetButtons(&pad);
            const u64 nowTick = armGetSystemTick();
            const u64 wait = armGetSystemTickFreq() / 6;
            auto ready = [&](u64 bit) { return (down & bit) || ((held & bit) && nowTick >= moveTick); };
            int dCol = 0, dRow = 0;
            if (detail < 0 && !credits) {
                if (ready(HidNpadButton_Left)) dCol = -1;
                else if (ready(HidNpadButton_Right)) dCol = 1;
                else if (ready(HidNpadButton_Up)) dRow = -1;
                else if (ready(HidNpadButton_Down)) dRow = 1;
                else {
                    const HidAnalogStickState stick = padGetStickPos(&pad, 0);
                    if (nowTick >= moveTick) {
                        if (stick.x < -16000) dCol = -1;
                        else if (stick.x > 16000) dCol = 1;
                        else if (stick.y > 16000) dRow = -1;
                        else if (stick.y < -16000) dRow = 1;
                    }
                }
            } else if (!credits) {
                const u64 freq = armGetSystemTickFreq();
                if (detailTick == 0) detailTick = nowTick;
                float dt = freq ? static_cast<float>(nowTick - detailTick) / static_cast<float>(freq) : 0.016f;
                detailTick = nowTick;
                if (dt > 0.05f) dt = 0.05f;
                const HidAnalogStickState stick = padGetStickPos(&pad, 0);
                float axis = 0;
                if (held & HidNpadButton_Up) axis = -1.f;
                else if (held & HidNpadButton_Down) axis = 1.f;
                else if (stick.y > 8000 || stick.y < -8000) axis = -static_cast<float>(stick.y) / 32767.f;
                detailScroll += axis * 640.f * dt;
                if (detailScroll < 0) detailScroll = 0;
                detailShown += (detailScroll - detailShown) * std::min(1.f, dt * 14.f);
            }
            if (dCol || dRow) {
                const int n = static_cast<int>(serverRows.size());
                if (focus < 0) {
                    if (dRow > 0 && n > 0) focus = 0;
                } else if (dCol) {
                    const int col = focus % 2 + dCol;
                    if (col >= 0 && col < 2 && (focus / 2) * 2 + col < n) focus += dCol;
                } else if (dRow < 0) {
                    focus = focus < 2 ? -1 : focus - 2;
                } else if (focus + 2 < n) {
                    focus += 2;
                }
                moveTick = nowTick + wait;
                if (focus >= 0) {
                    const int pitch = 112 + 10;
                    const int listTop = 16 + 84 + 12;
                    const int row = focus / 2;
                    const float top = static_cast<float>(row * pitch);
                    const float bottom = top + 112;
                    const float view = static_cast<float>(barY - listTop - 24);
                    const int rows = (n + 1) / 2;
                    const float content = rows > 0 ? rows * static_cast<float>(pitch) - 10.f : 0.f;
                    const float maxScroll = std::max(0.f, content - view);
                    if (top < serverScroll) serverScroll = top;
                    if (bottom > serverScroll + view) serverScroll = bottom - view;
                    if (serverScroll < 0) serverScroll = 0;
                    if (serverScroll > maxScroll) serverScroll = maxScroll;
                }
            }
            if (credits) {
                if (down & (HidNpadButton_B | HidNpadButton_Minus)) credits = false;
            } else if ((down & HidNpadButton_Minus) && detail < 0 && !account && !pairing) {
                credits = true;
            } else if ((down & HidNpadButton_A) && detail < 0 && focus < 0) {
                if (config.access_token.empty()) {
                    pairing = true;
                    auth.beginPair();
                } else account = true;
            } else if ((down & HidNpadButton_A) && detail < 0 && focus >= 0 && focus < static_cast<int>(serverRows.size())) {
                detail = focus;
                detailScroll = 0;
                detailShown = 0;
                detailTick = 0;
                detailKey.clear();
                if (!liveThread.joinable()) liveTick.store(0);
            } else if ((down & HidNpadButton_B) && detail >= 0) {
                detail = -1;
            } else if ((down & HidNpadButton_A) && detail >= 0 && config.access_token.empty()) {
                pairing = true;
                auth.beginPair();
            }
        }
        if (detail >= 0 && !config.access_token.empty() && !liveThread.joinable()) {
            const u64 freq = armGetSystemTickFreq();
            const u64 fetched = liveTick.load();
            const bool stale = fetched == 0 || armGetSystemTick() - fetched > freq * 15;
            if (stale) {
                liveDone.store(false);
                const std::string token = config.access_token;
                liveThread = std::thread([&auth, &liveMu, &liveBody, &liveError, &liveFailed, &liveTick, &liveDone, &liveStop, token] {
                    std::string body;
                    std::string error;
                    const bool ok = auth.fetchLive(token, body, error, liveStop);
                    {
                        std::lock_guard<std::mutex> lock(liveMu);
                        if (ok) { liveBody = std::move(body); liveError.clear(); liveFailed = false; }
                        else if (liveBody.empty()) { liveError = std::move(error); liveFailed = true; }
                    }
                    liveTick = armGetSystemTick();
                    liveDone.store(true);
                });
            }
        }

        SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
        SDL_RenderClear(renderer);

        if (account) {
            const Label& who = loginName.tex ? loginName : loginLabel;
            int y = 220;
            drawLabel(renderer, who, (ww - who.w) / 2, y);
            y += who.h + 16;
            if (friendLabel.tex) {
                drawLabel(renderer, friendLabel, (ww - friendLabel.w) / 2, y);
                y += friendLabel.h + 28;
            } else y += 28;
            Label out = makeLabel(renderer, hintFont, "(X) Log out", hint);
            drawLabel(renderer, out, (ww - out.w) / 2, y);
            destroyLabel(out);
        } else if (credits) {
            int y = 150;
            drawLabel(renderer, creditTitle, (ww - creditTitle.w) / 2, y);
            y += creditTitle.h + 36;
            drawLabel(renderer, creditWho, (ww - creditWho.w) / 2, y);
            y += creditWho.h + 8;
            drawLabel(renderer, creditRole, (ww - creditRole.w) / 2, y);
            y += creditRole.h + 32;
            drawLabel(renderer, creditNx, (ww - creditNx.w) / 2, y);
            y += creditNx.h + 8;
            drawLabel(renderer, creditThanks, (ww - creditThanks.w) / 2, y);
        } else if (pairing) {
            if (qrFor != auth.pairUrl()) {
                if (qrTex) SDL_DestroyTexture(qrTex);
                qrFor = auth.pairUrl();
                qrTex = makeQrTexture(renderer, qrFor);
                setText(pairCode, renderer, codeFont, auth.pairCode().c_str(), white);
            }
            const std::string note = auth.statusText();
            if (note != pairNote) {
                pairNote = note;
                setText(pairStatus, renderer, hintFont, pairNote.c_str(), hint);
            }
            const int qrSize = 360;
            SDL_Rect box{72, (barY - qrSize) / 2, qrSize, qrSize};
            if (qrTex) SDL_RenderCopy(renderer, qrTex, nullptr, &box);
            const int textX = box.x + box.w + 48;
            drawLabel(renderer, pairTitle, textX, box.y + 24);
            drawLabel(renderer, pairCode, textX, box.y + 80);
            drawLabel(renderer, pairHelp, textX, box.y + 180);
            if (pairStatus.tex) drawLabel(renderer, pairStatus, textX, box.y + 180 + pairHelp.h + 16);
        } else if (detail >= 0 && detail < static_cast<int>(serverRows.size())) {
            const ServerRow& row = serverRows[detail];
            const int iconSize = 112;
            SDL_Rect icon{36, 28, iconSize, iconSize};
            if (row.icon) SDL_RenderCopy(renderer, row.icon, nullptr, &icon);
            Label heading = makeLabel(renderer, font, fitText(font, row.fullName, ww - 200).c_str(), white);
            drawLabel(renderer, heading, icon.x + iconSize + 24, icon.y + (iconSize - heading.h) / 2);
            destroyLabel(heading);
            const int bodyY = icon.y + iconSize + 28;
            SDL_Rect clip{36, bodyY, ww - 72, barY - 16 - bodyY};
            const bool signedIn = !config.access_token.empty();
            if (!signedIn) {
                drawLabel(renderer, detailPrompt, clip.x, clip.y);
                if (detailStatus.tex) drawLabel(renderer, detailStatus, clip.x, clip.y + detailPrompt.h + 16);
            } else {
                std::string bodyCopy;
                std::string errorCopy;
                bool failed = false;
                {
                    std::lock_guard<std::mutex> lock(liveMu);
                    bodyCopy = liveBody;
                    errorCopy = liveError;
                    failed = liveFailed;
                }
                const std::string key = row.titleId + ":" + std::to_string(row.players) + ":" + std::to_string(liveTick.load()) + (failed ? ":f" : bodyCopy.empty() ? ":w" : ":d");
                if (key != detailKey) {
                    detailKey = key;
                    LivePage page;
                    if (failed) {
                        if (row.players > 0)
                            page.headline = std::to_string(row.players) + (row.players == 1 ? " player online." : " players online.");
                        page.note = errorCopy.empty() ? "Couldn't load lobby details." : "Couldn't load lobby details. " + errorCopy;
                    } else if (bodyCopy.empty()) page.note = "Loading...";
                    else page = describeLive(bodyCopy, row.titleId, row.players);
                    buildLive(liveDraw, renderer, font, hintFont, creditFont, page, clip.w);
                    sessionLogf("game %s count=%d players=%zu lobbies=%zu %s%s",
                        row.titleId.c_str(), row.players, page.players.size(), page.lobbies.size(),
                        page.headline.c_str(), page.note.empty() ? "" : page.note.c_str());
                }
                const int maxScroll = std::max(0, liveDraw.height - clip.h);
                if (detailScroll > maxScroll) detailScroll = static_cast<float>(maxScroll);
                if (detailShown > maxScroll) detailShown = static_cast<float>(maxScroll);
                if (detailShown < 0) detailShown = 0;
                SDL_RenderSetClipRect(renderer, &clip);
                drawLive(renderer, liveDraw, clip.x, clip.y - static_cast<int>(detailShown), clip.w, clip);
                SDL_RenderSetClipRect(renderer, nullptr);
            }
        } else {
            if (!servers.ready()) {
                Label loading = makeLabel(renderer, font, "Loading servers...", white);
                drawLabel(renderer, loading, (ww - loading.w) / 2, (barY - loading.h) / 2);
                destroyLabel(loading);
            } else if (servers.failed() || serverRows.empty()) {
                Label missing = makeLabel(renderer, font, "Couldn't load servers", white);
                drawLabel(renderer, missing, (ww - missing.w) / 2, (barY - missing.h) / 2);
                destroyLabel(missing);
            } else {
                const int cardH = 112;
                const int gap = 10;
                const int margin = 16;
                const int listTop = margin + 84 + 12;
                const int pitch = cardH + gap;
                const int cardW = (ww - margin * 2 - gap) / 2;
                const u64 nowList = armGetSystemTick();
                const u64 listFreq = armGetSystemTickFreq();
                float listDt = 0.016f;
                if (listTick != 0 && listFreq != 0)
                    listDt = std::min(0.05f, static_cast<float>(nowList - listTick) / static_cast<float>(listFreq));
                listTick = nowList;
                serverShown += (serverScroll - serverShown) * std::min(1.f, listDt * 12.f);
                const int scrollI = std::max(0, static_cast<int>(serverShown));
                const int first = scrollI / pitch;
                const int off = scrollI - first * pitch;
                const int rows = (barY - listTop) / pitch + 2;
                const int iconSize = 80;
                SDL_Rect listClip{0, listTop, ww, barY - listTop};
                SDL_RenderSetClipRect(renderer, &listClip);
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < 2; ++c) {
                        const int index = (first + r) * 2 + c;
                        if (index < 0 || index >= static_cast<int>(serverRows.size())) continue;
                        ServerRow& row = serverRows[index];
                        const int x = margin + c * (cardW + gap);
                        const int y = listTop - off + r * pitch;
                        if (y >= barY || y + cardH <= listTop) continue;
                        SDL_Rect card{x, y, cardW, cardH};
                        SDL_SetRenderDrawColor(renderer, 42, 42, 46, 255);
                        fillRound(renderer, card, 22);
                        const int blockH = row.name.h + 6 + row.status.h;
                        const int textY = y + (cardH - blockH) / 2;
                        const int iconY = y + (cardH - iconSize) / 2;
                        SDL_Rect icon{x + 16, iconY, iconSize, iconSize};
                        if (row.icon) SDL_RenderCopy(renderer, row.icon, nullptr, &icon);
                        const int textX = x + 16 + iconSize + 16;
                        drawLabel(renderer, row.name, textX, textY);
                        const int statusY = textY + row.name.h + 6;
                        const int dotR = 6;
                        if (row.kind == 2) SDL_SetRenderDrawColor(renderer, 214, 96, 108, 255);
                        else if (row.kind == 1) SDL_SetRenderDrawColor(renderer, 160, 160, 166, 255);
                        else SDL_SetRenderDrawColor(renderer, 120, 196, 130, 255);
                        fillDisk(renderer, textX + dotR, statusY + row.status.h / 2, dotR);
                        drawLabel(renderer, row.status, textX + dotR * 2 + 8, statusY);
                        const int numBlock = row.number.h + (row.word.tex ? 2 + row.word.h : 0);
                        const int numY = y + (cardH - numBlock) / 2;
                        drawLabel(renderer, row.number, x + cardW - 18 - row.number.w, numY);
                        if (row.word.tex)
                            drawLabel(renderer, row.word, x + cardW - 18 - row.word.w, numY + row.number.h + 2);
                        if (detail < 0 && index == focus) glowRing(renderer, card, 22, static_cast<float>(armGetSystemTick()) / armGetSystemTickFreq());
                    }
                }
                SDL_RenderSetClipRect(renderer, nullptr);
                SDL_Rect banner{margin, margin, ww - margin * 2, 84};
                SDL_SetRenderDrawColor(renderer, 50, 50, 54, 255);
                fillRound(renderer, banner, 28);
                SDL_SetRenderDrawColor(renderer, 120, 196, 130, 255);
                const int bannerTextY = banner.y + (banner.h - bannerTitle.h) / 2;
                fillDisk(renderer, banner.x + 28, bannerTextY + bannerTitle.h / 2, 7);
                drawLabel(renderer, bannerTitle, banner.x + 46, bannerTextY);
                const int rightH = bannerSub.h + 2 + bannerTime.h;
                const int rightY = bannerTextY + (bannerTitle.h - rightH) / 2;
                const bool signedIn = !config.access_token.empty();
                const int pillH = 52;
                const int pillW = signedIn ? loginName.w + 36 : loginLabel.w + 36;
                SDL_Rect pill{banner.x + banner.w - 18 - pillW, banner.y + (banner.h - pillH) / 2, pillW, pillH};
                const int statsRight = pill.x - 18;
                drawLabel(renderer, bannerSub, statsRight - bannerSub.w, rightY);
                drawLabel(renderer, bannerTime, statsRight - bannerTime.w, rightY + bannerSub.h + 2);
                SDL_SetRenderDrawColor(renderer, 32, 32, 36, 255);
                fillRound(renderer, pill, pillH / 2);
                drawLabel(renderer, signedIn ? loginName : loginLabel,
                    pill.x + (pillW - (signedIn ? loginName.w : loginLabel.w)) / 2,
                    pill.y + (pillH - (signedIn ? loginName.h : loginLabel.h)) / 2);
                if (detail < 0 && focus < 0) glowRing(renderer, pill, pillH / 2, static_cast<float>(armGetSystemTick()) / armGetSystemTickFreq());
            }
        }
        SDL_Rect bar{0, barY, ww, barH};
        SDL_SetRenderDrawColor(renderer, 28, 28, 32, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_SetRenderDrawColor(renderer, 70, 70, 76, 255);
        SDL_RenderDrawLine(renderer, 0, barY, ww, barY);
        const int wantHint = credits ? 1 : account ? 5 : pairing ? 1 : detail < 0 ? (focus < 0 ? (config.access_token.empty() ? 2 : 4) : 0) : (config.access_token.empty() ? 3 : 1);
        if (wantHint != hintMode) {
            hintMode = wantHint;
            const char* hintText = "(A) Select   (-) Credits   (+) Exit";
            if (wantHint == 1) hintText = "(B) Back";
            else if (wantHint == 2) hintText = "(A) Sign in   (-) Credits   (+) Exit";
            else if (wantHint == 3) hintText = "(A) Sign in   (B) Back";
            else if (wantHint == 4) hintText = "(A) Account   (-) Credits   (+) Exit";
            else if (wantHint == 5) hintText = "(X) Log out   (B) Back";
            setText(leftHint, renderer, hintFont, hintText, hint);
        }
        drawLabel(renderer, leftHint, 32, barY + (barH - leftHint.h) / 2);
        int creditRight = ww - 32;
        if (batteryOn) {
            u32 pct = 0;
            PsmChargerType charger = PsmChargerType_Unconnected;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) {
                if (static_cast<int>(pct) != batteryPct) {
                    batteryPct = static_cast<int>(pct);
                    const std::string text = std::to_string(batteryPct) + "%";
                    setText(batteryLabel, renderer, creditFont, text.c_str(), credit);
                }
            }
            psmGetChargerType(&charger);
            const bool charging = charger != PsmChargerType_Unconnected;
            const int iconW = 34;
            creditRight -= iconW;
            SDL_Rect body{creditRight, barY + (barH - 14) / 2, 28, 14};
            SDL_SetRenderDrawColor(renderer, 145, 145, 150, 255);
            SDL_RenderDrawRect(renderer, &body);
            SDL_Rect nub{body.x + body.w, body.y + 4, 3, 6};
            SDL_RenderFillRect(renderer, &nub);
            int fill = batteryPct < 0 ? 0 : batteryPct * 24 / 100;
            if (batteryPct > 0 && fill < 1) fill = 1;
            if (fill > 0) {
                SDL_Rect juice{body.x + 2, body.y + 2, fill, 10};
                SDL_SetRenderDrawColor(renderer, batteryPct <= 20 && !charging ? 214 : 120, batteryPct <= 20 && !charging ? 96 : 196, batteryPct <= 20 && !charging ? 108 : 130, 255);
                SDL_RenderFillRect(renderer, &juice);
            }
            if (batteryLabel.tex) {
                creditRight -= 8 + batteryLabel.w;
                drawLabel(renderer, batteryLabel, creditRight, barY + (barH - batteryLabel.h) / 2);
            }
            creditRight -= 18;
        }
        drawLabel(renderer, rightHint, creditRight - rightHint.w, barY + (barH - rightHint.h) / 2);
        if (phaseNow != 0) {
            if (phaseNow != updateShown) {
                updateShown = phaseNow;
                std::string headline = "Update available";
                std::string detail = "Press A to install.";
                if (phaseNow == 2) {
                    headline = "Downloading update...";
                    detail.clear();
                } else if (phaseNow == 3) {
                    headline = "Installation complete";
                    detail = "Press A to close.";
                } else if (phaseNow == 4) {
                    headline = "Update failed";
                    detail = "Press A to try again.";
                }
                setText(updateTitle, renderer, font, headline.c_str(), white);
                setText(updateBody, renderer, hintFont, detail.c_str(), white);
            }
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
            SDL_Rect shade{0, 0, ww, wh};
            SDL_RenderFillRect(renderer, &shade);
            SDL_Rect box{(ww - 760) / 2, (wh - 240) / 2, 760, 240};
            SDL_Rect frame{box.x - 4, box.y - 4, box.w + 8, box.h + 8};
            SDL_SetRenderDrawColor(renderer, 176, 96, 104, 255);
            SDL_RenderFillRect(renderer, &frame);
            SDL_SetRenderDrawColor(renderer, 46, 46, 50, 255);
            SDL_RenderFillRect(renderer, &box);
            const int blockH = updateTitle.h + (updateBody.tex ? 16 + updateBody.h : 0);
            int textY = box.y + (box.h - blockH) / 2;
            drawLabel(renderer, updateTitle, box.x + (box.w - updateTitle.w) / 2, textY);
            if (updateBody.tex)
                drawLabel(renderer, updateBody, box.x + (box.w - updateBody.w) / 2, textY + updateTitle.h + 16);
            if (phaseNow == 2) {
                SDL_Rect track{box.x + 48, box.y + box.h - 56, box.w - 96, 18};
                SDL_SetRenderDrawColor(renderer, 30, 30, 34, 255);
                SDL_RenderFillRect(renderer, &track);
                const curl_off_t got = updateGot.load();
                const curl_off_t total = updateTotal.load();
                int fillW = total > 0 ? static_cast<int>((track.w * got) / total) : 0;
                if (fillW > track.w) fillW = track.w;
                if (fillW > 0) {
                    SDL_Rect fill{track.x, track.y, fillW, track.h};
                    SDL_SetRenderDrawColor(renderer, 176, 96, 104, 255);
                    SDL_RenderFillRect(renderer, &fill);
                }
                const std::string stats = downloadStats(got, total, updateSpeed.load());
                if (stats != progressShown) {
                    progressShown = stats;
                    setText(progressStats, renderer, creditFont, stats.c_str(), white);
                }
                int statsX = track.x + track.w - progressStats.w;
                if (statsX < track.x) statsX = track.x;
                drawLabel(renderer, progressStats, statsX, track.y - progressStats.h - 6);
            }
        }
        SDL_RenderPresent(renderer);
    }

    updateStop = true;
    liveStop.store(true);
    if (updateThread.joinable()) updateThread.join();
    if (liveThread.joinable()) liveThread.join();
    auth.cancelPair();
    if (profileThread.joinable()) profileThread.join();
    servers.stop();
    for (auto& row : serverRows) {
        destroyLabel(row.name);
        destroyLabel(row.status);
        destroyLabel(row.number);
        destroyLabel(row.word);
        if (row.icon) SDL_DestroyTexture(row.icon);
    }
    if (qrTex) SDL_DestroyTexture(qrTex);
    if (avatar) SDL_DestroyTexture(avatar);
    destroyLive(liveDraw);
    destroyLabel(pairTitle);
    destroyLabel(pairHelp);
    destroyLabel(pairCode);
    destroyLabel(pairStatus);
    destroyLabel(loginLabel);
    destroyLabel(loginName);
    destroyLabel(friendLabel);
    destroyLabel(batteryLabel);
    destroyLabel(detailPrompt);
    destroyLabel(detailStatus);
    destroyLabel(leftHint);
    destroyLabel(creditTitle);
    destroyLabel(creditWho);
    destroyLabel(creditRole);
    destroyLabel(creditNx);
    destroyLabel(creditThanks);
    destroyLabel(rightHint);
    destroyLabel(bannerTitle);
    destroyLabel(bannerSub);
    destroyLabel(bannerTime);
    destroyLabel(updateTitle);
    destroyLabel(updateBody);
    destroyLabel(progressStats);
    if (creditFont) TTF_CloseFont(creditFont);
    if (hintFont) TTF_CloseFont(hintFont);
    if (codeFont) TTF_CloseFont(codeFont);
    if (font) TTF_CloseFont(font);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    if (batteryOn) psmExit();
    sessionLog("exit");
    sessionLogClose();
    socketExit();
    timeExit();
    plExit();
    return renderer ? 0 : 1;
}
