#define CURL_STATICLIB
#include "status.h"
#include "log.hpp"

#include <switch.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace {

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > 262144) return 0;
    out->append(ptr, n);
    return n;
}

long httpGet(const std::string& url, std::string& body, const std::atomic<bool>& halt, long timeout = 12) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus/" APP_VERSION);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<const std::atomic<bool>*>(userdata)->load() ? 1 : 0;
    });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(&halt));
    long http = 0;
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    logHttp("GET", url, http, rc, body.size(), http == 200 ? nullptr : &body);
    return http;
}

struct CatalogItem {
    const char* name;
    const char* ids;
    bool nostatus;
    bool rooms;
};

const CatalogItem kCatalog[] = {
    {"SpeedRunners 2: King of Speed", "01004cc023582000", true, false},
    {"SEGA Genesis - Nintendo Classics", "0100b3c014bda000", false, false},
    {"Just Dance 2022", "0100ea6014bb8000", false, false},
    {"Mario Kart 8 Deluxe", "0100152000022000", false, false},
    {"Splatoon 2", "0100f8f0000a2000,01003bc0000a0000,01003c700009c800", false, false},
    {"Splatoon 3", "0100c2500fc20000", false, false},
    {"Super Smash Bros. Ultimate", "01006a800016e000", false, false},
    {"Animal Crossing: New Horizons", "01006f8002326000", false, false},
    {"Luigi's Mansion 3", "0100dca0064a6000", false, false},
    {"Minecraft: Nintendo Switch Edition", "01006bd001e06000", false, false},
    {"ARMS", "01009b500007c000", false, false},
    {"Mario Tennis Aces", "0100bde00862a000", false, false},
    {"Super Mario Maker 2", "01009b90006dc000", false, false},
    {"PAC-MAN 99", "0100ad9012510000", false, false},
    {"Among Us", "0100b0c013912000", false, false},
    {"Mario & Sonic at the Olympic Games Tokyo 2020", "010086000e148000", false, false},
    {"Tetris 99", "010040600c5ce000", false, false},
    {"Super Mario Bros. 35", "0100277011f1a000", false, false},
    {"Super Mario Bros. Wonder", "010015100b514000", false, false},
    {"Clubhouse Games: 51 Worldwide Classics", "010047700d540000", false, false},
    {"Monster Hunter Generations Ultimate", "0100770008dd8000", false, false},
    {"Crash Team Racing Nitro-Fueled", "0100f9f00c696000", false, false},
    {"Diablo III: Eternal Collection", "01001b300b9be000", false, false},
    {"Mario Party Superstars", "01006fe013472000", false, false},
    {"Super Mario Party Jamboree", "0100965017338000", false, false},
    {"Kirby's Dream Buffet", "0100a8e016236000", false, false},
    {"Stardew Valley", "0100e65002bb8000", false, false},
    {"Overcooked! 2", "01006fd0080b2000", false, false},
    {"Pokémon Scarlet and Violet", "0100a3d008c5c000,01008f6008c5e000", false, false},
    {"Nintendo 64 - Nintendo Classics", "0100c9a00ece6000", false, false},
    {"NES - Nintendo Classics", "0100d870045b6000", false, false},
    {"SNES - Nintendo Classics", "01008d300c50c000", false, false},
    {"Game Boy - Nintendo Classics", "0100c62011050000", false, false},
    {"Game Boy Advance - Nintendo Classics", "010012f017576000", false, false},
    {"Pokémon Legends: Z-A", "0100f43008c44000", false, false},
    {"Pokémon Sword and Shield", "0100abf008968000,01008db008c2c000", false, false},
    {"Super Monkey Ball Banana Mania", "010001701248c000", false, false},
    {"Mario Golf: Super Rush", "0100c9c00e25c000", false, false},
    {"Super Mario 3D World + Bowser's Fury", "010028600ebda000", false, false},
    {"Super Mario Odyssey", "0100000000010000", false, false},
    {"METAL GEAR SOLID: Peace Walker", "0100c6f01c4f8000", false, false},
    {"Mario Strikers: Battle League", "010019401051c000", false, false},
    {"Minecraft Dungeons II", "0100a7c01b792000", false, false},
    {"Pokkén Tournament DX", "0100b3f000be2000", false, false},
    {"Saints Row: The Third - The Full Package", "0100de600beee000", false, false},
    {"UNO", "0100b6e012ebe000", false, false},
    {"Dying Light: Platinum Edition", "01008c8012920000", false, false},
    {"Guilty Gear -Strive-", "0100ab401f136000", false, false},
    {"Kirby Fighters 2", "0100227010460000", false, false},
    {"Terraria", "0100e46006708000", false, false},
    {"Goat Simulator 3", "01001cc01b2d4000", false, false},
    {"Super Kirby Clash", "01003fb00c5a8000", false, false},
    {"Ultimate Chicken Horse", "0100fcf002a58000", true, false},
    {"Crypt of the NecroDancer", "0100cea007d08000", true, true},
    {"Nintendo Switch Sports", "0100d2f00d5c0000", false, false},
    {"Monopoly for Nintendo Switch", "01007430037f6000", false, false},
    {"Dragon Ball FighterZ", "0100a250097f0000", false, false},
    {"Friday the 13th: The Game", "010092a00c4b6000", false, false},
    {"Monster Hunter Rise", "0100b04011742000", false, false},
    {"Miitopia", "01003da010e8a000", false, false},
    {"Game Builder Garage", "0100fa5010788000", false, false},
    {"Zombie Army 4: Dead War", "01000bf0152fa000", false, false},
};

bool countFor(const std::string& body, const std::string& id, int& out) {
    const std::string key = "\"" + id + "\":";
    const auto pos = body.find(key);
    if (pos == std::string::npos) return false;
    out = std::atoi(body.c_str() + pos + key.size());
    return true;
}

using Counts = std::map<std::string, int>;

bool jsonInt(const std::string& body, const char* key, int& out) {
    std::string token = std::string("\"") + key + "\":";
    auto pos = body.find(token);
    if (pos == std::string::npos) {
        token = std::string("\"") + key + "\": ";
        pos = body.find(token);
    }
    if (pos == std::string::npos) return false;
    out = std::atoi(body.c_str() + pos + token.size());
    return true;
}

bool jsonOnline(const std::string& body) {
    return body.find("\"online\":true") != std::string::npos || body.find("\"online\": true") != std::string::npos;
}

void readIds(const std::string& body, Counts& counts, const char* csv, bool replace) {
    std::string ids = csv;
    while (!ids.empty()) {
        const auto comma = ids.find(',');
        const std::string id = ids.substr(0, comma);
        if (replace) counts.erase(id);
        int n = 0;
        if (countFor(body, id, n) && n >= 0) counts[id] = n;
        if (comma == std::string::npos) break;
        ids.erase(0, comma + 1);
    }
}

void setConnected(const std::atomic<bool>& stop, Counts& counts, const char* url, const char* id, bool replace) {
    if (stop.load()) return;
    if (replace) counts.erase(id);
    std::string body;
    int n = 0;
    if (httpGet(url, body, stop, 5) != 200) return;
    if (replace && !jsonOnline(body)) return;
    if (jsonInt(body, "connected", n) && n >= 0) counts[id] = n;
}

const char* kServiceIds =
    "0100d870045b6000,01008d300c50c000,0100c62011050000,010012f017576000,0100b3c014bda000,"
    "0100fcf002a58000,0100ab401f136000,01003fb00c5a8000,0100227010460000,0100e46006708000,01001cc01b2d4000";

bool loadCounts(const std::atomic<bool>& stop, Counts& counts) {
    counts.clear();
    std::string body;
    if (httpGet("https://nextendo.network/api/online-counts", body, stop) != 200) return false;
    for (const auto& item : kCatalog) readIds(body, counts, item.ids, false);
    if (stop.load()) return true;

    setConnected(stop, counts, "https://nextendo.network/game-stats/sm3dw", "010028600ebda000", false);
    setConnected(stop, counts, "https://nextendo.network/game-stats/jamboree", "0100965017338000", true);
    setConnected(stop, counts, "https://nextendo.network/game-stats/stardew", "0100e65002bb8000", true);
    setConnected(stop, counts, "https://nextendo.network/game-stats/kirby", "0100a8e016236000", true);
    setConnected(stop, counts, "https://nextendo.network/game-stats/justdance22", "0100ea6014bb8000", true);

    if (!stop.load()) {
        for (const char* id : {"0100d870045b6000", "01008d300c50c000", "0100c62011050000", "010012f017576000",
                "0100b3c014bda000", "0100fcf002a58000", "0100ab401f136000", "01003fb00c5a8000",
                "0100227010460000", "0100e46006708000", "01001cc01b2d4000"})
            counts.erase(id);
        std::string catalog;
        if (httpGet("https://nextendo.network/service-catalog/v1/online-counts", catalog, stop, 5) == 200)
            readIds(catalog, counts, kServiceIds, false);
    }

    if (!stop.load()) {
        std::string uno;
        int n = 0;
        if (httpGet("https://nextendo.network/game-stats/uno-count", uno, stop, 5) == 200 &&
            uno.find("\"game\":\"UNO\"") != std::string::npos && jsonInt(uno, "connected", n) && n >= 0)
            counts["0100b6e012ebe000"] = n;
    }
    if (!stop.load()) {
        counts.erase("0100cea007d08000");
        std::string crypt;
        int n = 0;
        if (httpGet("https://nextendo.network/service-catalog/v1/crypt-status", crypt, stop, 5) == 200 &&
            jsonOnline(crypt) && crypt.find("room_members") != std::string::npos &&
            jsonInt(crypt, "players", n) && n >= 0 && n <= 1024)
            counts["0100cea007d08000"] = n;
    }
    if (!stop.load()) {
        counts.erase("01004cc023582000");
        std::string sr2;
        int n = 0;
        if (httpGet("https://nextendo.network/service-catalog/v1/sr2-status", sr2, stop, 5) == 200 &&
            jsonOnline(sr2) && sr2.find("native_transport_connections") != std::string::npos &&
            jsonInt(sr2, "players", n) && n >= 0 && n <= 50000)
            counts["01004cc023582000"] = n;
    }
    return true;
}

std::vector<PublicGame> parseCounts(const Counts& counts) {
    std::vector<PublicGame> games;
    for (const auto& item : kCatalog) {
        PublicGame game;
        game.name = item.name;
        game.rooms = item.rooms;
        int best = 0;
        bool found = false;
        std::string ids = item.ids;
        while (!ids.empty()) {
            const auto comma = ids.find(',');
            const std::string id = ids.substr(0, comma);
            if (game.titleId.empty()) game.titleId = id;
            const auto it = counts.find(id);
            if (it != counts.end()) {
                found = true;
                if (it->second > best) best = it->second;
            }
            if (comma == std::string::npos) break;
            ids.erase(0, comma + 1);
        }
        game.online = found;
        game.nostatus = item.nostatus && !found;
        game.players = found ? best : 0;
        games.push_back(std::move(game));
    }
    std::sort(games.begin(), games.end(), [](const PublicGame& a, const PublicGame& b) {
        if (a.players != b.players) return a.players > b.players;
        return a.name < b.name;
    });
    return games;
}

}  // namespace

struct PublicStatus::State {
    std::thread thread;
    std::mutex mu;
    std::atomic<bool> stop{false};
    std::atomic<bool> hold{false};
    std::atomic<bool> halt{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<int> revision{0};
    std::vector<PublicGame> games;
};

void PublicStatus::start() {
    stop();
    state_ = new State();
    state_->thread = std::thread([this] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        Counts counts;
        while (state_->hold.load() && !state_->stop.load())
            svcSleepThread(50'000'000);
        if (!loadCounts(state_->halt, counts)) {
            if (state_->stop) return;
            if (!state_->halt.load() && !state_->hold.load()) {
                state_->failed = true;
                state_->ready = true;
                return;
            }
        } else if (!state_->stop) {
            auto games = parseCounts(counts);
            {
                std::lock_guard<std::mutex> lock(state_->mu);
                state_->games = games;
            }
            state_->revision++;
            state_->ready = true;
            for (size_t i = 0; i < games.size() && !state_->stop; ++i) {
                if (games[i].titleId.empty()) continue;
                std::string jpeg;
                while (state_->hold.load() && !state_->stop.load())
                    svcSleepThread(50'000'000);
                if (state_->stop.load() || state_->halt.load()) break;
                const long icon = httpGet("https://nextendo.network/api/game-icon/" + games[i].titleId + ".jpg", jpeg, state_->halt);
                if (state_->stop) break;
                if (icon == 200 && jpeg.size() >= 2 && static_cast<unsigned char>(jpeg[0]) == 0xFF) {
                    std::lock_guard<std::mutex> lock(state_->mu);
                    if (i < state_->games.size()) state_->games[i].jpeg = std::move(jpeg);
                    state_->revision++;
                }
            }
        }
        while (!state_->stop) {
            for (int i = 0; i < 150 && !state_->stop; ++i)
                svcSleepThread(100'000'000);
            if (state_->stop) break;
            while (state_->hold.load() && !state_->stop.load())
                svcSleepThread(50'000'000);
            if (state_->stop) break;
            Counts again;
            if (!loadCounts(state_->halt, again)) continue;
            auto fresh = parseCounts(again);
            std::lock_guard<std::mutex> lock(state_->mu);
            for (auto& game : fresh) {
                for (const auto& old : state_->games) {
                    if (old.titleId == game.titleId) {
                        game.jpeg = old.jpeg;
                        break;
                    }
                }
            }
            state_->games = std::move(fresh);
            state_->revision++;
        }
    });
}

void PublicStatus::hold(bool on) {
    if (!state_) return;
    state_->hold = on;
    state_->halt = on || state_->stop.load();
}

void PublicStatus::stop() {
    if (!state_) return;
    state_->stop = true;
    state_->halt = true;
    if (state_->thread.joinable()) state_->thread.join();
    delete state_;
    state_ = nullptr;
}

bool PublicStatus::ready() const { return state_ && state_->ready.load(); }
bool PublicStatus::failed() const { return state_ && state_->failed.load(); }
int PublicStatus::revision() const { return state_ ? state_->revision.load() : 0; }

std::vector<PublicGame> PublicStatus::snapshot() const {
    if (!state_) return {};
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->games;
}
