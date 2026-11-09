#define CURL_STATICLIB
#include "auth.h"

#include <switch.h>
#include <curl/curl.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
constexpr char kPairAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int kVerifierLen = 64;
constexpr const char* kTokenUrl = "https://nextendo.network/oauth/token";
constexpr const char* kClientId = "nxc_rVab8rabbZ7j";
constexpr const char* kRelay = "https://nexstatus.nx-auth.workers.dev";

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > 262144) return 0;
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

// ponytail: grabs a JSON string field. Breaks if the token itself contains an escaped quote.
std::string jsonString(const std::string& body, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = body.find(pat);
    if (k == std::string::npos) return {};
    const auto colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return {};
    const auto q1 = body.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    const auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return body.substr(q1 + 1, q2 - q1 - 1);
}

std::string jsonField(const std::string& body, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = body.find(pat);
    if (k == std::string::npos) return {};
    const auto colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return {};
    size_t p = colon + 1;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\n' || body[p] == '\r')) ++p;
    if (p >= body.size()) return {};
    if (body[p] == '"') {
        const auto q2 = body.find('"', p + 1);
        if (q2 == std::string::npos) return {};
        return body.substr(p + 1, q2 - p - 1);
    }
    size_t e = p;
    while (e < body.size() && std::isdigit(static_cast<unsigned char>(body[e]))) ++e;
    return body.substr(p, e - p);
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

std::string hexRandom(size_t bytes) {
    u8 rnd[32] = {};
    if (bytes > sizeof(rnd)) bytes = sizeof(rnd);
    randomGet(rnd, bytes);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.resize(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        out[i * 2] = hex[rnd[i] >> 4];
        out[i * 2 + 1] = hex[rnd[i] & 0xf];
    }
    return out;
}

int abortTransfer(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* stop = static_cast<std::atomic<bool>*>(userdata);
    return stop && stop->load() ? 1 : 0;
}

void tune(CURL* curl, std::atomic<bool>* stop) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (!stop) return;
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortTransfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, stop);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

long httpGet(const std::string& url, std::string& body, curl_slist* headers = nullptr, std::atomic<bool>* stop = nullptr) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    tune(curl, stop);
    long http = 0;
    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    return http;
}

}  // namespace

struct NextendoAuth::Lan {
    std::thread thread;
    std::mutex mu;
    std::atomic<bool> stop{false};
    std::atomic<int> phase{0};  // 1 waiting, 2 ready, 3 failed
    bool sockets = false;
};

NextendoAuth::NextendoAuth() {
    generatePkce();
}

NextendoAuth::~NextendoAuth() {
    cancelLan();
}

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
    if (challenge_.empty()) {
        verifier_.clear();
        return false;
    }
    return true;
}

std::string NextendoAuth::authorizeUrl() const {
    return lanUrl_;
}

void NextendoAuth::joinThread() {
    if (lan_ && lan_->thread.joinable()) lan_->thread.join();
}

void NextendoAuth::loadProfile(const std::string& token, std::atomic<bool>* stop) {
    profileReady_.store(false);
    displayName_.clear();
    avatarJpeg_.clear();
    if (!token.empty()) {
        const std::string authz = "Authorization: Bearer " + token;
        const std::string client = std::string("X-Nextendo-Client-Id: ") + kClientId;
        curl_slist* headers = curl_slist_append(nullptr, authz.c_str());
        headers = curl_slist_append(headers, client.c_str());
        std::string me;
        const long http = httpGet("https://nextendo.network/api/me", me, headers, stop);
        curl_slist_free_all(headers);
        if (http == 200 && !(stop && stop->load())) {
            displayName_ = jsonField(me, "username");
            const std::string pid = jsonField(me, "pid");
            if (!pid.empty()) {
                std::string jpeg;
                if (httpGet("https://nextendo.network/api/avatar?pid=" + pid, jpeg, nullptr, stop) == 200 &&
                    jpeg.size() >= 2 && static_cast<unsigned char>(jpeg[0]) == 0xFF)
                    avatarJpeg_ = std::move(jpeg);
            }
        }
    }
    profileReady_.store(true);
}

void NextendoAuth::serve() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    const std::string pollUrl = std::string(kRelay) + "/api/poll?state=" + state_;
    const std::string pairUrl = std::string(kRelay) + "/api/pair?code=" + pairCode_ + "&state=" + state_ + "&challenge=" + challenge_;
    bool paired = pairCode_.empty();
    while (!lan_->stop && std::chrono::steady_clock::now() < deadline) {
        std::string body;
        const long http = httpGet(pollUrl, body, nullptr, &lan_->stop);
        if (http == 0) relayDown_.store(true);
        else if (http == 200 || http == 204) relayDown_.store(false);
        if (http == 200) {
            const std::string code = jsonString(body, "code");
            if (!code.empty()) {
                std::string tokenBody;
                const bool exchanged = exchangeCode(code, tokenBody);
                const std::string access = exchanged ? jsonString(tokenBody, "access_token") : "";
                const std::string refresh = exchanged ? jsonString(tokenBody, "refresh_token") : "";
                if (!access.empty()) {
                    std::lock_guard<std::mutex> lock(lan_->mu);
                    accessToken_ = access;
                    refreshToken_ = refresh;
                    lan_->phase = 2;
                } else {
                    std::lock_guard<std::mutex> lock(lan_->mu);
                    failReason_ = tokenBody.empty() ? "Token request failed" : tokenBody.substr(0, 140);
                    lan_->phase = 3;
                }
                return;
            }
        }
        if (!paired && !lan_->stop) {
            std::string ignored;
            const long posted = httpGet(pairUrl, ignored, nullptr, &lan_->stop);
            paired = posted >= 200 && posted < 300;
        }
        for (int i = 0; i < 10 && !lan_->stop; ++i)
            svcSleepThread(100'000'000);
    }
    if (!lan_->stop) lan_->phase = 3;
}

bool NextendoAuth::startLanLogin() {
    cancelLan();
    accessToken_.clear();
    refreshToken_.clear();
    failReason_.clear();
    relayDown_.store(false);
    if (challenge_.empty() && !generatePkce()) return false;

    state_ = hexRandom(16);
    lanUrl_ = std::string(kRelay) + "/api/go?state=" + state_ + "&challenge=" + challenge_;
    pairCode_.assign(6, '\0');
    u8 pairRnd[6];
    randomGet(pairRnd, sizeof(pairRnd));
    const int pairAlphabet = static_cast<int>(sizeof(kPairAlphabet) - 1);
    for (int i = 0; i < 6; ++i)
        pairCode_[i] = kPairAlphabet[pairRnd[i] % pairAlphabet];

    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    lan_ = new Lan();
    lan_->sockets = true;
    lan_->phase = 1;
    lan_->thread = std::thread([this] { serve(); });
    return true;
}

AuthPhase NextendoAuth::poll() {
    if (!lan_) return AuthPhase::Idle;
    const int phase = lan_->phase.load();
    if (phase == 1) return AuthPhase::Waiting;
    joinThread();

    const bool ok = phase == 2 && !accessToken_.empty();

    if (lan_->sockets) {
        socketExit();
        lan_->sockets = false;
    }
    delete lan_;
    lan_ = nullptr;
    return ok ? AuthPhase::Ok : AuthPhase::Failed;
}

void NextendoAuth::cancelLan() {
    if (!lan_) return;
    lan_->stop = true;
    joinThread();
    if (lan_->sockets) socketExit();
    delete lan_;
    lan_ = nullptr;
}

bool NextendoAuth::exchangeCode(const std::string& code, std::string& responseBody) {
    responseBody.clear();
    if (code.empty() || verifier_.empty()) return false;

    bool ok = false;
    CURL* curl = curl_easy_init();
    if (curl) {
        const std::string body =
            "grant_type=authorization_code"
            "&client_id=" + escape(curl, kClientId) +
            "&code=" + escape(curl, code.c_str()) +
            "&redirect_uri=" + escape(curl, (std::string(kRelay) + "/api/callback").c_str()) +
            "&code_verifier=" + escape(curl, verifier_.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, kTokenUrl);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        tune(curl, lan_ ? &lan_->stop : nullptr);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        long http = 0;
        ok = curl_easy_perform(curl) == CURLE_OK;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        ok = ok && http >= 200 && http < 300;
        curl_easy_cleanup(curl);
    }

    return ok;
}
