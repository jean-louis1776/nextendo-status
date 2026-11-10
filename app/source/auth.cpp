#define CURL_STATICLIB
#include "auth.h"
#include "log.hpp"

#include <switch.h>
#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Paste the new Nextendo application id here. Redirect in the portal must be kRedirect.
constexpr const char* kClientId = "nxc_dyNRS0L9SLND";
constexpr const char* kRelay = "https://d5de3i1ormj1kpci9q3d.764nr5vy.apigw.yandexcloud.net";
constexpr const char* kRedirect = "https://d5de3i1ormj1kpci9q3d.764nr5vy.apigw.yandexcloud.net/callback";
constexpr char kPairAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr const char* kTokenUrl = "https://nextendo.network/oauth/token";
constexpr const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
constexpr int kVerifierLen = 64;

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > 16 * 1024 * 1024) return 0;
    out->append(ptr, n);
    return n;
}

std::string escape(CURL* curl, const char* value) {
    char* raw = curl_easy_escape(curl, value, 0);
    if (!raw) return {};
    std::string out = raw;
    curl_free(raw);
    return out;
}

std::string jsonString(const std::string& body, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = body.find(pat);
    if (k == std::string::npos) return {};
    const auto colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return {};
    const auto q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos || q1 > colon + 8) return {};
    const auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return body.substr(q1 + 1, q2 - q1 - 1);
}


std::string base64Url(const unsigned char* data, size_t len) {
    unsigned char buf[64];
    size_t olen = 0;
    if (mbedtls_base64_encode(buf, sizeof(buf), &olen, data, len) != 0) return {};
    std::string out(reinterpret_cast<char*>(buf), olen);
    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

void tune(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

long httpPost(const std::string& url, const std::string& payload, std::string& body) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    tune(curl);
    long http = 0;
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    logHttp("POST", url, http, rc, body.size(), http == 200 ? nullptr : &body);
    return http;
}

long httpGet(const std::string& url, std::string& body, curl_slist* headers, long timeout = 15, bool follow = false, bool gzip = false, CURLcode* err = nullptr, const std::atomic<bool>* stop = nullptr) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    tune(curl);
    if (follow) curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (gzip) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (stop) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<const std::atomic<bool>*>(userdata)->load() ? 1 : 0;
        });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(stop));
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    long http = 0;
    const CURLcode rc = curl_easy_perform(curl);
    if (err) *err = rc;
    if (rc == CURLE_OK || !body.empty())
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    logHttp("GET", url, http, rc, body.size(), http == 200 ? nullptr : &body);
    return http;
}

curl_slist* authHeaders(const std::string& token) {
    const std::string authz = "Authorization: Bearer " + token;
    const std::string client = std::string("X-Nextendo-Client-Id: ") + kClientId;
    curl_slist* headers = curl_slist_append(nullptr, authz.c_str());
    return curl_slist_append(headers, client.c_str());
}

}  // namespace

bool NextendoAuth::generatePkce() {
    verifier_.assign(kVerifierLen, '\0');
    u8 rnd[kVerifierLen];
    randomGet(rnd, sizeof(rnd));
    const int alphabet = static_cast<int>(sizeof(kAlphabet) - 1);
    for (int i = 0; i < kVerifierLen; ++i)
        verifier_[i] = kAlphabet[rnd[i] % alphabet];

    unsigned char hash[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(verifier_.data()), verifier_.size(), hash, 0) != 0) {
        verifier_.clear();
        challenge_.clear();
        return false;
    }
    challenge_ = base64Url(hash, sizeof(hash));
    if (challenge_.empty()) verifier_.clear();
    return !challenge_.empty();
}

bool NextendoAuth::exchangeCode(const std::string& code) {
    if (code.empty() || verifier_.empty()) return false;
    std::string response;
    bool ok = false;
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string body =
        "grant_type=authorization_code"
        "&client_id=" + escape(curl, kClientId) +
        "&code=" + escape(curl, code.c_str()) +
        "&redirect_uri=" + escape(curl, kRedirect) +
        "&code_verifier=" + escape(curl, verifier_.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, kTokenUrl);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    tune(curl);
    long http = 0;
    const CURLcode rc = curl_easy_perform(curl);
    ok = rc == CURLE_OK;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    logHttp("POST", kTokenUrl, http, rc, response.size(), nullptr);
    std::lock_guard<std::mutex> lock(mu_);
    if (!ok || http < 200 || http >= 300) {
        failReason_ = response.empty() ? "Token request failed" : response.substr(0, 140);
        return false;
    }
    accessToken_ = jsonString(response, "access_token");
    refreshToken_ = jsonString(response, "refresh_token");
    if (accessToken_.empty()) {
        failReason_ = "No access token in the response";
        return false;
    }
    return true;
}

void NextendoAuth::cancelPair() {
    stop_.store(true);
    if (pairThread_.joinable()) pairThread_.join();
    stop_.store(false);
}

std::string NextendoAuth::statusText() const {
    std::lock_guard<std::mutex> lock(mu_);
    return failReason_;
}

bool NextendoAuth::takeTokens(std::string& access, std::string& refresh) {
    if (!ready_.load()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (accessToken_.empty()) return false;
    access = accessToken_;
    refresh = refreshToken_;
    ready_.store(false);
    return true;
}

void NextendoAuth::beginPair() {
    cancelPair();
    {
        std::lock_guard<std::mutex> lock(mu_);
        accessToken_.clear();
        refreshToken_.clear();
        failReason_.clear();
        ready_.store(false);
    }
    if (!kClientId[0] || !generatePkce()) {
        std::lock_guard<std::mutex> lock(mu_);
        failReason_ = "Couldn't prepare sign-in";
        return;
    }
    state_.assign(32, '0');
    u8 rnd[16];
    randomGet(rnd, sizeof(rnd));
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        state_[i * 2] = hex[rnd[i] >> 4];
        state_[i * 2 + 1] = hex[rnd[i] & 0xf];
    }
    pairCode_.assign(6, 'A');
    u8 pick[6];
    randomGet(pick, sizeof(pick));
    const int alphabet = static_cast<int>(sizeof(kPairAlphabet) - 1);
    for (int i = 0; i < 6; ++i) pairCode_[i] = kPairAlphabet[pick[i] % alphabet];
    pairUrl_ = std::string(kRelay) + "/?c=" + pairCode_;
    stop_.store(false);
    pairThread_ = std::thread([this] { runPair(); });
}

void NextendoAuth::runPair() {
    std::string body;
    const std::string payload = std::string("{\"code\":\"") + pairCode_ + "\",\"state\":\"" + state_ + "\",\"challenge\":\"" + challenge_ + "\"}";
    if (httpPost(std::string(kRelay) + "/pair", payload, body) != 200) {
        std::lock_guard<std::mutex> lock(mu_);
        failReason_ = "Couldn't reach the sign-in site";
        return;
    }
    const std::string pollUrl = std::string(kRelay) + "/poll?state=" + state_;
    for (int i = 0; i < 150 && !stop_.load(); ++i) {
        for (int step = 0; step < 10 && !stop_.load(); ++step) svcSleepThread(200'000'000);
        if (stop_.load()) return;
        std::string reply;
        if (httpGet(pollUrl, reply, nullptr, 4) != 200) continue;
        const std::string code = jsonString(reply, "code");
        if (code.empty()) continue;
        if (!exchangeCode(code)) return;
        ready_.store(true);
        return;
    }
    if (!stop_.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        failReason_ = "Sign-in timed out";
    }
}

void NextendoAuth::useTokens(const std::string& access, const std::string& refresh) {
    accessToken_ = access;
    refreshToken_ = refresh;
}

bool NextendoAuth::refresh() {
    if (refreshToken_.empty() || !kClientId[0]) return false;
    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string form =
        "grant_type=refresh_token&client_id=" + escape(curl, kClientId) +
        "&refresh_token=" + escape(curl, refreshToken_.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, kTokenUrl);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    tune(curl);
    long http = 0;
    const CURLcode rc = curl_easy_perform(curl);
    const bool ok = rc == CURLE_OK;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    logHttp("POST", kTokenUrl, http, rc, response.size(), nullptr);
    if (!ok || http < 200 || http >= 300) return false;
    const std::string access = jsonString(response, "access_token");
    if (access.empty()) return false;
    accessToken_ = access;
    const std::string next = jsonString(response, "refresh_token");
    if (!next.empty()) refreshToken_ = next;
    return true;
}

bool NextendoAuth::readProfile(const std::string& token) {
    displayName_.clear();
    friendCode_.clear();
    avatarJpeg_.clear();
    if (token.empty() || !kClientId[0]) return false;
    curl_slist* headers = authHeaders(token);
    std::string me;
    const long http = httpGet("https://nextendo.network/api/me", me, headers);
    curl_slist_free_all(headers);
    if (http != 200) return false;
    displayName_ = jsonString(me, "username");
    if (displayName_.empty()) displayName_ = jsonString(me, "name");
    friendCode_ = jsonString(me, "friend_code");
    return !displayName_.empty();
}

bool NextendoAuth::loadProfile(const std::string& token) {
    if (!token.empty()) accessToken_ = token;
    const bool ok = readProfile(accessToken_) || (refresh() && readProfile(accessToken_));
    sessionLog(ok ? "profile ok" : "profile failed");
    return ok;
}

namespace {

size_t matchClose(const std::string& s, size_t open) {
    const char close = s[open] == '{' ? '}' : ']';
    int depth = 0;
    bool str = false;
    bool esc = false;
    for (size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') ++depth;
        else if ((c == '}' || c == ']') && --depth == 0) return c == close ? i : std::string::npos;
    }
    return std::string::npos;
}

size_t findKey(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    int depth = 0;
    bool str = false;
    bool esc = false;
    for (size_t i = 0; i < obj.size(); ++i) {
        const char c = obj[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') {
            if (depth == 1 && obj.compare(i, pat.size(), pat) == 0) return i;
            str = true;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
    }
    return std::string::npos;
}

std::string rawString(const std::string& obj, const char* key) {
    const size_t k = findKey(obj, key);
    if (k == std::string::npos) return {};
    const size_t colon = obj.find(':', k + 1);
    if (colon == std::string::npos) return {};
    const size_t q1 = obj.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    std::string out;
    bool esc = false;
    for (size_t i = q1 + 1; i < obj.size(); ++i) {
        const char c = obj[i];
        if (esc) {
            if (c == 'u' && i + 4 < obj.size()) {
                int cp = 0;
                for (int h = 1; h <= 4; ++h) {
                    const char d = obj[i + h];
                    cp <<= 4;
                    if (d >= '0' && d <= '9') cp += d - '0';
                    else if (d >= 'a' && d <= 'f') cp += d - 'a' + 10;
                    else if (d >= 'A' && d <= 'F') cp += d - 'A' + 10;
                }
                i += 4;
                if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            } else if (c == 'n') out.push_back('\n');
            else out.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') esc = true;
        else if (c == '"') break;
        else out.push_back(c);
    }
    return out;
}

int rawInt(const std::string& obj, const char* key, int missing) {
    const size_t k = findKey(obj, key);
    if (k == std::string::npos) return missing;
    const size_t colon = obj.find(':', k + 1);
    if (colon == std::string::npos) return missing;
    size_t p = colon + 1;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\n' || obj[p] == '\r' || obj[p] == '\t')) ++p;
    int sign = 1;
    if (p < obj.size() && obj[p] == '-') { sign = -1; ++p; }
    if (p >= obj.size() || obj[p] < '0' || obj[p] > '9') return missing;
    int v = 0;
    while (p < obj.size() && obj[p] >= '0' && obj[p] <= '9') v = v * 10 + (obj[p++] - '0');
    return sign * v;
}

std::string rawArray(const std::string& obj, const char* key) {
    const size_t k = findKey(obj, key);
    if (k == std::string::npos) return {};
    const size_t b = obj.find('[', k + 1);
    if (b == std::string::npos) return {};
    const size_t e = matchClose(obj, b);
    if (e == std::string::npos) return {};
    return obj.substr(b, e - b + 1);
}

std::vector<std::string> rawObjects(const std::string& arr) {
    std::vector<std::string> out;
    int depth = 0;
    bool str = false;
    bool esc = false;
    size_t start = std::string::npos;
    for (size_t i = 0; i < arr.size(); ++i) {
        const char c = arr[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') {
            if (c == '{' && depth == 1) start = i;
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
            if (c == '}' && depth == 1 && start != std::string::npos) {
                out.push_back(arr.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

std::string gameObject(const std::string& body, const std::string& titleId) {
    const std::string needle = "\"" + titleId + "\"";
    bool str = false;
    bool esc = false;
    std::vector<size_t> opens;
    std::vector<size_t> hits;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') {
            if (!opens.empty() && body.compare(i, needle.size(), needle) == 0) hits.push_back(opens.back());
            str = true;
            continue;
        }
        if (c == '{') opens.push_back(i);
        else if (c == '}' && !opens.empty()) opens.pop_back();
    }
    std::string best;
    int bestNames = -1;
    for (size_t open : hits) {
        const size_t close = matchClose(body, open);
        if (close == std::string::npos) continue;
        const std::string game = body.substr(open, close - open + 1);
        int names = 0;
        for (size_t p = 0; (p = game.find("\"name\"", p)) != std::string::npos; p += 6) ++names;
        if (names > bestNames) {
            bestNames = names;
            best = game;
        }
    }
    return best;
}

LiveChip chipFrom(const std::string& obj, bool withLobby) {
    LiveChip chip;
    chip.name = rawString(obj, "name");
    const bool you = obj.find("\"you\":true") != std::string::npos || obj.find("\"you\": true") != std::string::npos;
    const bool pal = obj.find("\"friend\":true") != std::string::npos || obj.find("\"friend\": true") != std::string::npos;
    if (you) chip.mark = "You";
    else if (pal) chip.mark = "Friend";
    if (withLobby) {
        const int lobby = rawInt(obj, "lobby", -1);
        if (lobby >= 0) chip.extra = "Lobby " + std::to_string(lobby + 1);
    }
    const std::string activity = rawString(obj, "activity");
    if (!activity.empty()) {
        if (!chip.extra.empty()) chip.extra += "  ";
        chip.extra += activity;
    }
    return chip;
}

const char* lobbyState(const std::string& state) {
    if (state == "searching") return "Searching";
    if (state == "matched") return "In a match";
    if (state == "full") return "Full";
    if (state == "open") return "Open";
    return nullptr;
}

}  // namespace

LivePage describeLive(const std::string& body, const std::string& titleId, int onlineCount) {
    LivePage page;
    const std::string game = gameObject(body, titleId);
    const auto lobbies = game.empty() ? std::vector<std::string>{} : rawObjects(rawArray(game, "lobbies"));
    const auto players = game.empty() ? std::vector<std::string>{} : rawObjects(rawArray(game, "players"));
    if (players.empty()) {
        if (onlineCount > 0)
            page.headline = std::to_string(onlineCount) + (onlineCount == 1 ? " player online." : " players online.");
        page.note = onlineCount > 0 ? "Live player and lobby details are temporarily unavailable." : "Nobody is online right now.";
        return page;
    }
    const bool roomsHidden = game.find("\"roomsAvailable\":false") != std::string::npos || game.find("\"roomsAvailable\": false") != std::string::npos;
    if (roomsHidden) page.note = "Player presence is live. Room details are temporarily unavailable.";
    else if (lobbies.empty()) page.note = "These players are online but are not in a room yet.";
    if (!roomsHidden) {
        for (size_t i = 0; i < lobbies.size(); ++i) {
            LiveLobby lobby;
            lobby.mode = rawString(lobbies[i], "mode");
            const char* stateWord = lobbyState(rawString(lobbies[i], "state"));
            lobby.state = stateWord ? stateWord : rawString(lobbies[i], "state");
            const int count = rawInt(lobbies[i], "count", -1);
            const int max = rawInt(lobbies[i], "max", -1);
            if (count >= 0 && max > 0) lobby.places = std::to_string(count) + "/" + std::to_string(max);
            else if (count >= 0) lobby.places = std::to_string(count);
            for (const std::string& member : rawObjects(rawArray(lobbies[i], "players"))) {
                LiveChip chip = chipFrom(member, false);
                if (!chip.name.empty()) lobby.members.push_back(std::move(chip));
            }
            page.lobbies.push_back(std::move(lobby));
        }
    }
    for (const std::string& player : players) {
        LiveChip chip = chipFrom(player, true);
        if (!chip.name.empty()) page.players.push_back(std::move(chip));
    }
    return page;
}

bool NextendoAuth::fetchLive(const std::string& token, std::string& body, std::string& error, const std::atomic<bool>& stop) {
    body.clear();
    error.clear();
    if (token.empty()) {
        error = "Not signed in";
        return false;
    }
    std::string access = token;
    curl_slist* headers = authHeaders(access);
    std::string mainBody;
    std::string extra;
    CURLcode mainErr = CURLE_OK;
    long mainHttp = httpGet("https://nextendo.network/api/status/live", mainBody, headers, 40, false, true, &mainErr, &stop);
    if (stop.load()) {
        curl_slist_free_all(headers);
        error = "Cancelled";
        return false;
    }
    if (mainHttp == 401 && refresh()) {
        access = accessToken_;
        curl_slist_free_all(headers);
        headers = authHeaders(access);
        mainHttp = httpGet("https://nextendo.network/api/status/live", mainBody, headers, 40, false, true, &mainErr, &stop);
    }
    if (stop.load()) {
        curl_slist_free_all(headers);
        error = "Cancelled";
        return false;
    }
    const long extraHttp = httpGet("https://nextendo.network/service-catalog/v1/status/live", extra, headers, 40, false, true, nullptr, &stop);
    curl_slist_free_all(headers);
    if (mainHttp == 200) body += mainBody;
    if (extraHttp == 200) body += extra;
    if (!body.empty()) return true;
    if (mainHttp > 0) error = "HTTP " + std::to_string(mainHttp);
    else error = curl_easy_strerror(mainErr);
    return false;
}
