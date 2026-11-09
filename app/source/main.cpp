#include "auth.h"
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
#include <cstdio>
#include <sys/stat.h>
#include <ctime>
#include <cstring>
#include <mutex>
#include <setjmp.h>
#include <string>
#include <thread>
#include <vector>

enum AppState {
    STATE_SERVERS,
    STATE_AUTH_PENDING,
    STATE_LOADING,
    STATE_MAIN_MENU,
};

struct Label {
    SDL_Texture* tex = nullptr;
    int w = 0;
    int h = 0;
};

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

#if 0
static SDL_Texture* makeQr(SDL_Renderer* renderer, const char* text, int* outDim) {
    *outDim = 0;
    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text, qrcodegen::QrCode::Ecc::LOW);
    const int quiet = 4;
    const int scale = 8;
    const int dim = (qr.getSize() + quiet * 2) * scale;
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, dim, dim);
    if (!tex) return nullptr;
    SDL_SetRenderTarget(renderer, tex);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            if (!qr.getModule(x, y)) continue;
            SDL_Rect cell{(quiet + x) * scale, (quiet + y) * scale, scale, scale};
            SDL_RenderFillRect(renderer, &cell);
        }
    }
    SDL_SetRenderTarget(renderer, nullptr);
    *outDim = dim;
    return tex;
}
#endif

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpegExit(j_common_ptr cinfo) {
    longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1);
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
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
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
            rgba[i] = rgb[static_cast<size_t>(y) * w * 3 + x * 3];
            rgba[i + 1] = rgb[static_cast<size_t>(y) * w * 3 + x * 3 + 1];
            rgba[i + 2] = rgb[static_cast<size_t>(y) * w * 3 + x * 3 + 2];
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

#if 0
static bool pointIn(const SDL_Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
#endif

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // auth paused: sign-in stays in the sources, nothing on screen opens it.
#if 0
    NextendoAuth auth;
    AppConfig config;
    config.load(kSharedConfigPath);
#endif

    if (R_FAILED(plInitialize(PlServiceType_User)))
        return 1;
    if (SDL_Init(SDL_INIT_VIDEO) != 0 || TTF_Init() != 0) {
        SDL_Quit();
        plExit();
        return 1;
    }
    hidInitializeTouchScreen();

    SDL_Window* window = SDL_CreateWindow("nexstatus", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
    SDL_Renderer* renderer = window
        ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
        : nullptr;
    TTF_Font* font = renderer ? openSystemFont(34) : nullptr;
    TTF_Font* hintFont = renderer ? openSystemFont(24) : nullptr;
    TTF_Font* creditFont = renderer ? openSystemFont(18) : nullptr;
    timeInitialize();

    const SDL_Color white{255, 255, 255, 255};
    const SDL_Color hint{230, 230, 235, 255};
    const SDL_Color credit{145, 145, 150, 255};
#if 0
    const SDL_Color link{120, 190, 255, 255};
#endif
#if 0
    const bool haveToken = !config.access_token.empty();
#endif

    Label title = makeLabel(renderer, font, "NexStatus", white);
    Label buttonText = makeLabel(renderer, font, "Sign in", white);
    Label step1 = makeLabel(renderer, hintFont, "1. Phone: scan the QR code", hint);
    Label step2 = makeLabel(renderer, hintFont, "2. PC: open the site and type the code", hint);
    Label step3 = makeLabel(renderer, hintFont, "3. Allow NexStatus. This screen continues", hint);
    Label urlLabel;
    Label codeLabel;
    Label leftHint = makeLabel(renderer, hintFont, "+ Exit", hint);
    Label rightHint = makeLabel(renderer, creditFont, APP_VERSION "  by ILALEX & Nextendo", credit);
    Label errorLine;
    Label welcomeLabel = makeLabel(renderer, font, "Welcome", white);
    Label nameLabel;

    SDL_Texture* qr = nullptr;
    SDL_Texture* avatar = nullptr;
#if 0
    int qrDim = 0;
#endif
    AppState state = STATE_SERVERS;
    PublicStatus servers;
    std::vector<ServerRow> serverRows;
    Label bannerTitle;
    Label bannerSub;
    Label bannerTime;
    Label updateTitle;
    Label updateBody;
    int serverRev = -1;
    float serverScroll = 0;
    float serverVel = 0;
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
    curl_global_init(CURL_GLOBAL_DEFAULT);
    socketInitializeDefault();
    servers.start();
    std::thread updateThread(checkUpdate, std::move(nroPath), &updateStop, &updateMu, &updateText, &updatePhase, &updateInstall, &updateGot, &updateTotal, &updateSpeed);
    bool wasTouching = false;
#if 0
    bool profileDrawn = false;
    std::thread profileThread;
    if (haveToken) {
        profileThread = std::thread([&] {
            socketInitializeDefault();
            curl_global_init(CURL_GLOBAL_DEFAULT);
            auth.loadProfile(config.access_token);
            socketExit();
        });
    }
#endif

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

#if 0
    auto beginLogin = [&] {
        if (profileThread.joinable()) profileThread.join();
        profileDrawn = false;
        if (avatar) SDL_DestroyTexture(avatar);
        avatar = nullptr;
        destroyLabel(nameLabel);
        if (!auth.startLanLogin()) {
            setText(errorLine, renderer, hintFont, "Couldn't start sign-in", white);
            return;
        }
        if (qr) SDL_DestroyTexture(qr);
        qr = makeQr(renderer, auth.lanUrl().c_str(), &qrDim);
        setText(urlLabel, renderer, hintFont, "nexstatus.nx-auth.workers.dev", link);
        setText(codeLabel, renderer, font, auth.pairCode().c_str(), white);
        setText(title, renderer, font, "Sign in", white);
        setText(leftHint, renderer, hintFont, "(B) Back", hint);
        destroyLabel(errorLine);
        state = STATE_LOADING;
    };

    auto showLogin = [&] {
        state = STATE_AUTH_PENDING;
        setText(title, renderer, font, "NexStatus", white);
        setText(leftHint, renderer, hintFont, "(A) OK", hint);
        destroyLabel(errorLine);
    };

    auto showHome = [&] {
        state = STATE_MAIN_MENU;
        setText(leftHint, renderer, hintFont, "(Y) Log out", hint);
        destroyLabel(errorLine);
    };

    auto logout = [&] {
        if (profileThread.joinable()) profileThread.join();
        config.access_token.clear();
        config.refresh_token.clear();
        config.saveShared();
        profileDrawn = false;
        if (avatar) SDL_DestroyTexture(avatar);
        avatar = nullptr;
        destroyLabel(nameLabel);
        showLogin();
    };
#endif

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

        HidTouchScreenState touch{};
        const bool touching = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
        const bool tapped = touching && !wasTouching;
        wasTouching = touching;
        int tx = 0, ty = 0;
        if (tapped) {
            tx = static_cast<int>(touch.touches[0].x);
            ty = static_cast<int>(touch.touches[0].y);
        }

        int ww = 1280, wh = 720;
        SDL_GetRendererOutputSize(renderer, &ww, &wh);
        if (ww <= 0 || wh <= 0) {
            ww = 1280;
            wh = 720;
        }
        if (tapped) {
            tx = tx * ww / 1280;
            ty = ty * wh / 720;
        }

        const int barH = 60;
        const int barY = wh - barH;
#if 0
        SDL_Rect button{(ww - 460) / 2, (barY - 88) / 2, 460, 88};
#endif

        if (state == STATE_SERVERS && servers.ready() && !servers.failed()) {
            const int rev = servers.revision();
            if (rev != serverRev) {
                const auto games = servers.snapshot();
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
                int offline = 0, active = 0, playing = 0;
                bool countsChanged = !bannerTime.tex;
                for (size_t i = 0; i < games.size(); ++i) {
                    const int kind = games[i].online ? 0 : (games[i].nostatus ? 1 : 2);
                    if (kind == 2) ++offline;
                    else ++active;
                    if (games[i].online) playing += games[i].players;
                    ServerRow& row = serverRows[i];
                    if (row.titleId != games[i].titleId) {
                        row.titleId = games[i].titleId;
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
                const std::string head = offline == 0 ? "All services active"
                    : offline == 1 ? "1 game server is offline"
                    : std::to_string(offline) + " game servers are offline";
                const std::string sub = std::to_string(active) + " of " + std::to_string(games.size())
                    + " game servers active  ·  " + std::to_string(playing) + " players online.";
                setText(bannerTitle, renderer, font, head.c_str(), white);
                setText(bannerSub, renderer, hintFont, sub.c_str(), hint);
                if (countsChanged) {
                    const std::string stamp = "Updated at " + consoleClock();
                    setText(bannerTime, renderer, hintFont, stamp.c_str(), hint);
                }
                serverRev = rev;
            }
            const int cardH = 112;
            const int gap = 10;
            const int listTop = 16 + 84 + 12;
            const int pitch = cardH + gap;
            const float maxScroll = std::max(0.f, ((static_cast<int>(serverRows.size()) + 1) / 2) * static_cast<float>(pitch) - (barY - listTop));
            if (updatePhase.load() != 0) {
                serverVel = 0;
            } else {
                const u64 held = padGetButtons(&pad);
                const HidAnalogStickState stick = padGetStickPos(&pad, 0);
                float axis = 0;
                if (held & HidNpadButton_Up) axis = -1.f;
                else if (held & HidNpadButton_Down) axis = 1.f;
                else if (stick.y > 6000 || stick.y < -6000) axis = -stick.y / 32767.f;
                if (axis != 0.f) serverVel = axis * 18.f;
                else serverVel *= 0.84f;
                if (serverVel > -0.2f && serverVel < 0.2f) serverVel = 0;
                serverScroll += serverVel;
                if (serverScroll < 0) { serverScroll = 0; serverVel = 0; }
                if (serverScroll > maxScroll) { serverScroll = maxScroll; serverVel = 0; }
            }
        }

#if 0
        if (state == STATE_LOADING) {
            const AuthPhase phase = auth.poll();
            if (phase == AuthPhase::Ok) {
                config.access_token = auth.accessToken();
                config.refresh_token = auth.refreshToken();
                showHome();
                if (!config.saveShared())
                    setText(errorLine, renderer, hintFont, "Couldn't save the config", white);
                profileDrawn = false;
                if (profileThread.joinable()) profileThread.join();
                const std::string token = config.access_token;
                profileThread = std::thread([&auth, token] {
                    socketInitializeDefault();
                    curl_global_init(CURL_GLOBAL_DEFAULT);
                    auth.loadProfile(token);
                    socketExit();
                });
            } else if (phase == AuthPhase::Failed) {
                state = STATE_AUTH_PENDING;
                setText(title, renderer, font, "Sign-in failed", white);
                const char* why = auth.failReason().empty() ? "Press the button to try again" : auth.failReason().c_str();
                setText(errorLine, renderer, hintFont, why, white);
                setText(leftHint, renderer, hintFont, "(A) OK", hint);
            } else if (down & HidNpadButton_B) {
                auth.cancelLan();
                destroyLabel(errorLine);
                if (!config.access_token.empty()) showHome();
                else showLogin();
            } else if (auth.relayDown() && !errorLine.tex) {
                setText(errorLine, renderer, hintFont, "Console can't reach the sign-in site", white);
            }
        } else if (state == STATE_MAIN_MENU && (down & HidNpadButton_Y)) {
            logout();
        } else if (state == STATE_AUTH_PENDING && ((down & HidNpadButton_A) || (tapped && pointIn(button, tx, ty)))) {
            beginLogin();
        }

        if (state == STATE_MAIN_MENU && auth.profileReady() && !profileDrawn) {
            setText(nameLabel, renderer, font, auth.displayName().c_str(), white);
            if (avatar) SDL_DestroyTexture(avatar);
            avatar = textureFromJpeg(renderer, auth.avatarJpeg());
            profileDrawn = true;
        }
#endif

        SDL_SetRenderDrawColor(renderer, 18, 18, 20, 255);
        SDL_RenderClear(renderer);

        if (state == STATE_SERVERS) {
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
                const int first = static_cast<int>(serverScroll) / pitch;
                const int off = static_cast<int>(serverScroll) - first * pitch;
                const int rows = (barY - listTop) / pitch + 2;
                const int iconSize = 80;
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
                        SDL_RenderFillRect(renderer, &card);
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
                    }
                }
                SDL_Rect banner{margin, margin, ww - margin * 2, 84};
                SDL_SetRenderDrawColor(renderer, 50, 50, 54, 255);
                SDL_RenderFillRect(renderer, &banner);
                SDL_SetRenderDrawColor(renderer, 120, 196, 130, 255);
                const int bannerTextY = banner.y + (banner.h - bannerTitle.h) / 2;
                fillDisk(renderer, banner.x + 28, bannerTextY + bannerTitle.h / 2, 7);
                drawLabel(renderer, bannerTitle, banner.x + 46, bannerTextY);
                const int rightH = bannerSub.h + 2 + bannerTime.h;
                const int rightY = bannerTextY + (bannerTitle.h - rightH) / 2;
                drawLabel(renderer, bannerSub, banner.x + banner.w - 28 - bannerSub.w, rightY);
                drawLabel(renderer, bannerTime, banner.x + banner.w - 28 - bannerTime.w, rightY + bannerSub.h + 2);
            }
        }
#if 0
        else if (state == STATE_LOADING && qr) {
            const int qx = 72;
            const int qy = 70;
            SDL_Rect qdst{qx, qy, qrDim, qrDim};
            SDL_RenderCopy(renderer, qr, nullptr, &qdst);
            int x = qx + qrDim + 36;
            int y = qy + 8;
            drawLabel(renderer, title, x, y);
            y += title.h + 28;
            drawLabel(renderer, step1, x, y);
            y += step1.h + 10;
            drawLabel(renderer, step2, x, y);
            y += step2.h + 10;
            drawLabel(renderer, step3, x, y);
            y += step3.h + 22;
            drawLabel(renderer, urlLabel, x, y);
            y += urlLabel.h + 16;
            drawLabel(renderer, codeLabel, x, y);
            if (auth.relayDown() && errorLine.tex) {
                y += codeLabel.h + 16;
                drawLabel(renderer, errorLine, x, y);
            }
        } else if (state == STATE_MAIN_MENU) {
            const int av = 96;
            const int blockH = welcomeLabel.h + 18 + (avatar ? av + 14 : 0) + nameLabel.h;
            int y = (barY - blockH) / 2;
            drawLabel(renderer, welcomeLabel, (ww - welcomeLabel.w) / 2, y);
            y += welcomeLabel.h + 18;
            if (avatar) {
                SDL_Rect dst{(ww - av) / 2, y, av, av};
                SDL_RenderCopy(renderer, avatar, nullptr, &dst);
                y += av + 14;
            }
            drawLabel(renderer, nameLabel, (ww - nameLabel.w) / 2, y);
            if (errorLine.tex)
                drawLabel(renderer, errorLine, (ww - errorLine.w) / 2, y + nameLabel.h + 16);
        } else {
            drawLabel(renderer, title, (ww - title.w) / 2, button.y - title.h - 36);
            SDL_SetRenderDrawColor(renderer, 196, 32, 48, 255);
            SDL_RenderFillRect(renderer, &button);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            for (int i = 1; i <= 3; ++i) {
                SDL_Rect outline{button.x - i * 2, button.y - i * 2, button.w + i * 4, button.h + i * 4};
                SDL_RenderDrawRect(renderer, &outline);
            }
            drawLabel(renderer, buttonText, button.x + (button.w - buttonText.w) / 2, button.y + (button.h - buttonText.h) / 2);
            if (errorLine.tex)
                drawLabel(renderer, errorLine, (ww - errorLine.w) / 2, button.y + button.h + 18);
        }
#endif

        SDL_Rect bar{0, barY, ww, barH};
        SDL_SetRenderDrawColor(renderer, 28, 28, 32, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_SetRenderDrawColor(renderer, 70, 70, 76, 255);
        SDL_RenderDrawLine(renderer, 0, barY, ww, barY);
        drawLabel(renderer, leftHint, 32, barY + (barH - leftHint.h) / 2);
        drawLabel(renderer, rightHint, ww - 32 - rightHint.w, barY + (barH - rightHint.h) / 2);
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
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
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
    if (updateThread.joinable()) updateThread.join();
    servers.stop();
    for (auto& row : serverRows) {
        destroyLabel(row.name);
        destroyLabel(row.status);
        destroyLabel(row.number);
        destroyLabel(row.word);
        if (row.icon) SDL_DestroyTexture(row.icon);
    }
#if 0
    if (profileThread.joinable()) profileThread.join();
    auth.cancelLan();
#endif
    if (qr) SDL_DestroyTexture(qr);
    if (avatar) SDL_DestroyTexture(avatar);
    destroyLabel(title);
    destroyLabel(buttonText);
    destroyLabel(step1);
    destroyLabel(step2);
    destroyLabel(step3);
    destroyLabel(urlLabel);
    destroyLabel(codeLabel);
    destroyLabel(leftHint);
    destroyLabel(rightHint);
    destroyLabel(errorLine);
    destroyLabel(welcomeLabel);
    destroyLabel(nameLabel);
    destroyLabel(bannerTitle);
    destroyLabel(bannerSub);
    destroyLabel(bannerTime);
    destroyLabel(updateTitle);
    destroyLabel(updateBody);
    destroyLabel(progressStats);
    if (creditFont) TTF_CloseFont(creditFont);
    if (hintFont) TTF_CloseFont(hintFont);
    if (font) TTF_CloseFont(font);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    socketExit();
    timeExit();
    plExit();
    return renderer ? 0 : 1;
}
