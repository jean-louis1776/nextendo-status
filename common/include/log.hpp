#pragma once

#include <switch.h>
#define CURL_STATICLIB
#include <curl/curl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>

// One file per launch: sdmc:/config/nexstatus/logs/YYYY-MM-DD_HH-MM-SS.log
// ponytail: one mutex and fflush each line; no rotation until the card fills up.

inline std::mutex& sessionLogMu() {
    static std::mutex mu;
    return mu;
}

inline FILE*& sessionLogFile() {
    static FILE* file = nullptr;
    return file;
}

inline time_t sessionNow() {
    u64 ts = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &ts)) &&
        R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &ts)))
        ts = static_cast<u64>(time(nullptr));
    return static_cast<time_t>(ts);
}

inline void sessionStamp(char* buf, size_t n) {
    const time_t t = sessionNow();
    tm parts{};
    gmtime_r(&t, &parts);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &parts);
}

inline void sessionLog(const char* line) {
    std::lock_guard<std::mutex> lock(sessionLogMu());
    FILE*& file = sessionLogFile();
    if (!file) {
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/nexstatus", 0777);
        mkdir("sdmc:/config/nexstatus/logs", 0777);
        const time_t t = sessionNow();
        tm parts{};
        gmtime_r(&t, &parts);
        char path[96];
        strftime(path, sizeof path, "sdmc:/config/nexstatus/logs/%Y-%m-%d_%H-%M-%S.log", &parts);
        file = std::fopen(path, "a");
        if (!file) return;
    }
    char stamp[32];
    sessionStamp(stamp, sizeof stamp);
    std::fprintf(file, "%s  %s\n", stamp, line ? line : "");
    std::fflush(file);
}

inline void sessionLogf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    sessionLog(buf);
}

inline void sessionLogClose() {
    std::lock_guard<std::mutex> lock(sessionLogMu());
    FILE*& file = sessionLogFile();
    if (!file) return;
    std::fclose(file);
    file = nullptr;
}

inline void logHttp(const char* method, const std::string& url, long http, int curlCode, size_t bytes, const std::string* body) {
    std::string path = url;
    const auto query = path.find('?');
    if (query != std::string::npos) path.erase(query);
    if (http == 200 && path.find("/api/game-icon/") != std::string::npos) return;
    char extra[140] = "";
    if (body && http != 200 && !body->empty() && body->size() < 300 && body->find("token") == std::string::npos) {
        std::string snip = body->substr(0, 80);
        for (char& c : snip)
            if (static_cast<unsigned char>(c) < 32) c = ' ';
        std::snprintf(extra, sizeof extra, " \"%.80s\"", snip.c_str());
    }
    const char* err = http == 0 ? curl_easy_strerror(static_cast<CURLcode>(curlCode)) : "";
    sessionLogf("%s %s -> %ld %s %zuB%s", method, path.c_str(), http, err, bytes, extra);
}
