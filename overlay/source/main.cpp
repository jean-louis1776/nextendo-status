#define TESLA_INIT_IMPL
#include <tesla.hpp>

#define CURL_STATICLIB
#include <curl/curl.h>
#include <switch.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "config.hpp"

// Tesla's framebuffer already uses most of a 4 MiB heap, so the BSD buffer has to stay small.
extern "C" u64 __nx_heap_size = 0x800000;

struct Row {
    std::string name;
    std::string titleId;
    int players = 0;
};

struct Person {
    std::string name;
    std::string mark;
};

struct Room {
    std::string title;
    std::string places;
    std::vector<Person> members;
};

struct DetailSnap {
    std::string note;
    std::vector<Person> lines;
    std::vector<Room> rooms;
    std::vector<Person> players;
    int generation = 0;
    bool ready = false;
};

struct Snap {
    std::string summary = "Loading...";
    std::vector<Row> rows;
    int generation = 0;
    bool ok = false;
};

static std::atomic<bool> g_run{true};
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_abort{false};
static std::atomic<bool> g_refresh{false};
static std::atomic<bool> g_fetched{false};
static std::atomic<bool> g_detail{false};
static std::atomic<bool> g_detailFetch{false};
static std::mutex g_mu;
static Snap g_snap;
static DetailSnap g_detailSnap;
static std::string g_detailTitle;
static std::string g_detailToken;
static int g_detailPlayers = 0;
static Thread g_workerThread;
static bool g_workerStarted = false;
alignas(0x1000) static u8 g_workerStack[0x40000];
static size_t g_bodyCap = 96 * 1024;

static size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (g_abort.load() || !g_run.load()) return 0;
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > g_bodyCap) return 0;
    out->append(ptr, n);
    return n;
}

static std::atomic<u64> g_deadline{0};

static int curlAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    if (g_deadline.load() != 0 && armGetSystemTick() > g_deadline.load()) return 1;
    return (g_abort.load() || !g_run.load()) ? 1 : 0;
}

static bool completeJson(const std::string& body) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    bool started = false;
    for (const char ch : body) {
        if (inString) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') inString = true;
        else if (ch == '{') { ++depth; started = true; }
        else if (ch == '}') {
            --depth;
            if (started && depth == 0) return true;
        }
    }
    return false;
}

static bool httpGet(const char* url, const char* ip, std::string& body, const char* token = nullptr, long timeoutSec = 12, bool gzip = false) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const u64 freq = armGetSystemTickFreq();
    g_deadline = armGetSystemTick() + (freq ? freq * static_cast<u64>(timeoutSec) : 0);
    std::string authz;
    std::string client;
    curl_slist* headers = nullptr;
    if (token != nullptr && token[0] != '\0') {
        authz = std::string("Authorization: Bearer ") + token;
        client = "X-Nextendo-Client-Id: nxc_dyNRS0L9SLND";
        headers = curl_slist_append(nullptr, authz.c_str());
        headers = curl_slist_append(headers, client.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (gzip) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeoutSec > 4 ? 4L : timeoutSec);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    struct curl_slist* resolve = nullptr;
    if (ip != nullptr && ip[0] != '\0') {
        char line[80];
        std::snprintf(line, sizeof line, "nextendo.network:443:%s", ip);
        resolve = curl_slist_append(nullptr, line);
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    }
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlAbort);
    const CURLcode rc = curl_easy_perform(curl);
    g_deadline = 0;
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
        curl_easy_cleanup(curl);
    curl_slist_free_all(resolve);
    curl_slist_free_all(headers);
    if (http >= 400 || body.empty()) return false;
    if (rc == CURLE_OK && http == 200) return true;
    return (http == 200 || http == 0) && completeJson(body);
}

struct CatalogItem {
    const char* name;
    const char* ids;
};

static const CatalogItem kCatalog[] = {
    {"SpeedRunners 2: King of Speed", "01004cc023582000"},
    {"SEGA Genesis - Nintendo Classics", "0100b3c014bda000"},
    {"Just Dance 2022", "0100ea6014bb8000"},
    {"Mario Kart 8 Deluxe", "0100152000022000"},
    {"Splatoon 2", "0100f8f0000a2000,01003bc0000a0000,01003c700009c800"},
    {"Splatoon 3", "0100c2500fc20000"},
    {"Super Smash Bros. Ultimate", "01006a800016e000"},
    {"Animal Crossing: New Horizons", "01006f8002326000"},
    {"Luigi's Mansion 3", "0100dca0064a6000"},
    {"Minecraft: Nintendo Switch Edition", "01006bd001e06000"},
    {"ARMS", "01009b500007c000"},
    {"Mario Tennis Aces", "0100bde00862a000"},
    {"Super Mario Maker 2", "01009b90006dc000"},
    {"PAC-MAN 99", "0100ad9012510000"},
    {"Among Us", "0100b0c013912000"},
    {"Mario & Sonic at the Olympic Games Tokyo 2020", "010086000e148000"},
    {"Tetris 99", "010040600c5ce000"},
    {"Super Mario Bros. 35", "0100277011f1a000"},
    {"Super Mario Bros. Wonder", "010015100b514000"},
    {"Clubhouse Games: 51 Worldwide Classics", "010047700d540000"},
    {"Monster Hunter Generations Ultimate", "0100770008dd8000"},
    {"Crash Team Racing Nitro-Fueled", "0100f9f00c696000"},
    {"Diablo III: Eternal Collection", "01001b300b9be000"},
    {"Mario Party Superstars", "01006fe013472000"},
    {"Super Mario Party Jamboree", "0100965017338000"},
    {"Kirby's Dream Buffet", "0100a8e016236000"},
    {"Stardew Valley", "0100e65002bb8000"},
    {"Overcooked! 2", "01006fd0080b2000"},
    {"Pokémon Scarlet and Violet", "0100a3d008c5c000,01008f6008c5e000"},
    {"Nintendo 64 - Nintendo Classics", "0100c9a00ece6000"},
    {"NES - Nintendo Classics", "0100d870045b6000"},
    {"SNES - Nintendo Classics", "01008d300c50c000"},
    {"Game Boy - Nintendo Classics", "0100c62011050000"},
    {"Game Boy Advance - Nintendo Classics", "010012f017576000"},
    {"Pokémon Legends: Z-A", "0100f43008c44000"},
    {"Pokémon Sword and Shield", "0100abf008968000,01008db008c2c000"},
    {"Super Monkey Ball Banana Mania", "010001701248c000"},
    {"Mario Golf: Super Rush", "0100c9c00e25c000"},
    {"Super Mario 3D World + Bowser's Fury", "010028600ebda000"},
    {"Super Mario Odyssey", "0100000000010000"},
    {"METAL GEAR SOLID: Peace Walker", "0100c6f01c4f8000"},
    {"Mario Strikers: Battle League", "010019401051c000"},
    {"Minecraft Dungeons II", "0100a7c01b792000"},
    {"Pokkén Tournament DX", "0100b3f000be2000"},
    {"Saints Row: The Third - The Full Package", "0100de600beee000"},
    {"UNO", "0100b6e012ebe000"},
    {"Dying Light: Platinum Edition", "01008c8012920000"},
    {"Guilty Gear -Strive-", "0100ab401f136000"},
    {"Kirby Fighters 2", "0100227010460000"},
    {"Terraria", "0100e46006708000"},
    {"Goat Simulator 3", "01001cc01b2d4000"},
    {"Super Kirby Clash", "01003fb00c5a8000"},
    {"Ultimate Chicken Horse", "0100fcf002a58000"},
    {"Crypt of the NecroDancer", "0100cea007d08000"},
    {"Nintendo Switch Sports", "0100d2f00d5c0000"},
    {"Monopoly for Nintendo Switch", "01007430037f6000"},
    {"Dragon Ball FighterZ", "0100a250097f0000"},
    {"Friday the 13th: The Game", "010092a00c4b6000"},
    {"Monster Hunter Rise", "0100b04011742000"},
    {"Miitopia", "01003da010e8a000"},
    {"Portal 2", "0100abd01785c000"},
    {"Game Builder Garage", "0100fa5010788000"},
    {"Zombie Army 4: Dead War", "01000bf0152fa000"},
};

static const char* kServiceIds =
    "0100d870045b6000,01008d300c50c000,0100c62011050000,010012f017576000,0100b3c014bda000,"
    "0100fcf002a58000,0100ab401f136000,01003fb00c5a8000,0100227010460000,0100e46006708000,01001cc01b2d4000,"
    "0100e65002bb8000";

using Counts = std::map<std::string, int>;

static bool countFor(const std::string& body, const std::string& id, int& out) {
    const std::string key = "\"" + id + "\":";
    const auto pos = body.find(key);
    if (pos == std::string::npos) return false;
    out = std::atoi(body.c_str() + pos + key.size());
    return true;
}

static void readIds(const std::string& body, Counts& counts, const char* csv, bool replace) {
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

static bool jsonInt(const std::string& body, const char* key, int& out) {
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

static bool jsonOnline(const std::string& body) {
    return body.find("\"online\":true") != std::string::npos || body.find("\"online\": true") != std::string::npos;
}

static bool stopped() {
    return g_abort.load() || g_detail.load() || !g_run.load();
}

static void setConnected(const char* ip, Counts& counts, const char* url, const char* id, bool replace) {
    if (stopped()) return;
    if (replace) counts.erase(id);
    std::string body;
    int n = 0;
    if (!httpGet(url, ip, body, nullptr, 5)) return;
    if (replace && !jsonOnline(body)) return;
    if (jsonInt(body, "connected", n) && n >= 0) counts[id] = n;
}

static void fillRows(const Counts& counts, std::vector<Row>& rows, int& players) {
    rows.clear();
    players = 0;
    for (const auto& item : kCatalog) {
        std::string ids = item.ids;
        std::string first;
        int best = 0;
        bool found = false;
        while (!ids.empty()) {
            const auto comma = ids.find(',');
            const std::string id = ids.substr(0, comma);
            if (first.empty()) first = id;
            const auto it = counts.find(id);
            if (it != counts.end()) {
                found = true;
                if (it->second > best) best = it->second;
            }
            if (comma == std::string::npos) break;
            ids.erase(0, comma + 1);
        }
        if (!found || best <= 0) continue;
        players += best;
        rows.push_back(Row{item.name, std::move(first), best});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.players != b.players) return a.players > b.players;
        return a.name < b.name;
    });
}

static void publish(Snap next) {
    std::lock_guard<std::mutex> lock(g_mu);
    next.generation = g_snap.generation + 1;
    if (!next.ok && g_snap.ok) return;
    g_snap = std::move(next);
}

struct Net {
    bool socketOwn = false;
    bool nifmOwn = false;
    bool requestOpen = false;
    NifmRequest request{};
};

static void closeNet(Net& net) {
    if (net.requestOpen) {
        nifmRequestClose(&net.request);
        net.requestOpen = false;
    }
    if (net.socketOwn) {
        socketExit();
        net.socketOwn = false;
    }
    if (net.nifmOwn) {
        nifmExit();
        net.nifmOwn = false;
    }
}

static bool skipDnsName(const uint8_t* r, int got, size_t& i) {
    while (i < static_cast<size_t>(got)) {
        if ((r[i] & 0xC0) == 0xC0) {
            i += 2;
            return true;
        }
        const uint8_t len = r[i];
        if (len == 0) {
            i++;
            return true;
        }
        i += static_cast<size_t>(1 + len);
    }
    return false;
}

static bool queryA(const char* host, u32 dns, struct in_addr* out) {
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return false;
    uint8_t q[256]{};
    q[0] = 0x12;
    q[1] = 0x34;
    q[2] = 0x01;
    q[5] = 1;
    size_t n = 12;
    const char* label = host;
    while (*label && n < 200) {
        const char* dot = std::strchr(label, '.');
        const size_t len = dot ? static_cast<size_t>(dot - label) : std::strlen(label);
        if (len == 0 || len > 63) {
            close(fd);
            return false;
        }
        q[n++] = static_cast<uint8_t>(len);
        std::memcpy(q + n, label, len);
        n += len;
        if (!dot) break;
        label = dot + 1;
    }
    q[n++] = 0;
    q[n++] = 0;
    q[n++] = 1;
    q[n++] = 0;
    q[n++] = 1;
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = dns;
    if (sendto(fd, q, n, 0, reinterpret_cast<struct sockaddr*>(&dst), sizeof dst) < 0) {
        close(fd);
        return false;
    }
    struct pollfd wait{fd, POLLIN, 0};
    if (poll(&wait, 1, 2000) <= 0) {
        close(fd);
        return false;
    }
    uint8_t reply[512];
    const int got = recvfrom(fd, reply, sizeof reply, 0, nullptr, nullptr);
    close(fd);
    if (got < 12) return false;
    const int answers = (reply[6] << 8) | reply[7];
    size_t i = 12;
    if (!skipDnsName(reply, got, i)) return false;
    i += 4;
    for (int a = 0; a < answers && i + 10 < static_cast<size_t>(got); ++a) {
        if (!skipDnsName(reply, got, i) || i + 10 > static_cast<size_t>(got)) return false;
        const int type = (reply[i] << 8) | reply[i + 1];
        const int klass = (reply[i + 2] << 8) | reply[i + 3];
        const int rdlen = (reply[i + 8] << 8) | reply[i + 9];
        i += 10;
        if (i + static_cast<size_t>(rdlen) > static_cast<size_t>(got)) return false;
        if (type == 1 && klass == 1 && rdlen == 4) {
            std::memcpy(&out->s_addr, reply + i, 4);
            return true;
        }
        i += rdlen;
    }
    return false;
}

static bool lookupHost(char* ip, size_t ipLen) {
    ip[0] = '\0';
    u32 addr = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    nifmGetCurrentIpConfigInfo(&addr, &mask, &gateway, &dns1, &dns2);
    const u32 servers[2] = {dns1 ? dns1 : htonl(0x01010101), htonl(0x01010101)};
    struct in_addr found{};
    for (u32 dns : servers) {
        if (!queryA("nextendo.network", dns, &found)) continue;
        inet_ntop(AF_INET, &found, ip, ipLen);
        if (ip[0] != '\0') return true;
    }
    std::snprintf(ip, ipLen, "188.114.96.1");
    return true;
}

static bool openNet(Net& net, char* ip, size_t ipLen) {
    SocketInitConfig cfg{};
    cfg.tcp_tx_buf_size = 0x4000;
    cfg.tcp_rx_buf_size = 0x4000;
    cfg.tcp_tx_buf_max_size = 0x8000;
    cfg.tcp_rx_buf_max_size = 0x8000;
    cfg.udp_tx_buf_size = 0x800;
    cfg.udp_rx_buf_size = 0xA500;
    cfg.sb_efficiency = 1;
    cfg.num_bsd_sessions = 1;
    cfg.bsd_service_type = BsdServiceType_User;
    Result rc = 1;
    tsl::hlp::doWithSmSession([&] {
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) net.nifmOwn = true;
        if (R_SUCCEEDED(nifmCreateRequest(&net.request, true))) {
            net.requestOpen = true;
            nifmRequestSubmit(&net.request);
            const u64 freq = armGetSystemTickFreq();
            const u64 start = armGetSystemTick();
            while (freq != 0 && armGetSystemTick() - start < freq * 3) {
                NifmRequestState state = NifmRequestState_Invalid;
                if (R_FAILED(nifmGetRequestState(&net.request, &state))) break;
                if (state == NifmRequestState_Available) break;
                svcSleepThread(50'000'000);
            }
        }
        rc = socketInitialize(&cfg);
        if (R_SUCCEEDED(rc) || R_DESCRIPTION(rc) == LibnxError_AlreadyInitialized)
            lookupHost(ip, ipLen);
    });
    if (R_SUCCEEDED(rc) || R_DESCRIPTION(rc) == LibnxError_AlreadyInitialized) {
        net.socketOwn = R_SUCCEEDED(rc);
        return true;
    }
    closeNet(net);
    return false;
}

static std::string loadToken();

static bool detailedTitle(const std::string& id) {
    const char* ids[] = {
        "0100d870045b6000", "01008d300c50c000", "0100c62011050000", "010012f017576000", "0100b3c014bda000",
        "0100fcf002a58000", "0100ab401f136000", "01003fb00c5a8000", "0100227010460000", "0100e46006708000",
        "01001cc01b2d4000", "0100e65002bb8000", "0100ea6014bb8000",
    };
    for (const char* item : ids) if (id == item) return true;
    return false;
}

static size_t matchEnd(const std::string& body, size_t open, char left, char right) {
    bool str = false;
    bool esc = false;
    int depth = 0;
    for (size_t i = open; i < body.size(); ++i) {
        const char c = body[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == left) ++depth;
        else if (c == right && --depth == 0) return i;
    }
    return std::string::npos;
}

static void applyLiveCounts(const std::string& body, Counts& counts) {
    const char* ids[] = {
        "0100d870045b6000", "01008d300c50c000", "0100c62011050000", "010012f017576000", "0100b3c014bda000",
        "0100fcf002a58000", "0100ab401f136000", "01003fb00c5a8000", "0100227010460000", "0100e46006708000",
        "01001cc01b2d4000", "0100e65002bb8000", "0100ea6014bb8000",
    };
    for (const char* id : ids) {
        const std::string needle = std::string("\"") + id + "\"";
        for (size_t at = 0; (at = body.find(needle, at)) != std::string::npos; at += needle.size()) {
            const auto open = body.rfind('{', at);
            if (open == std::string::npos) continue;
            const auto close = matchEnd(body, open, '{', '}');
            if (close == std::string::npos || close < at) continue;
            const std::string game = body.substr(open, close - open + 1);
            if (game.find("\"players\"") == std::string::npos || game.find("\"lobbies\"") == std::string::npos) continue;
            const auto titlesAt = game.find("\"titles\"");
            const auto titlesOpen = titlesAt == std::string::npos ? std::string::npos : game.find('[', titlesAt);
            const auto titlesClose = titlesOpen == std::string::npos ? std::string::npos : matchEnd(game, titlesOpen, '[', ']');
            if (titlesClose == std::string::npos) continue;
            int quotes = 0;
            for (size_t i = titlesOpen; i <= titlesClose; ++i) if (game[i] == '"') ++quotes;
            if (quotes != 2) continue;
            const auto playersAt = game.find("\"players\"");
            const auto playersOpen = game.find('[', playersAt);
            const auto playersClose = playersOpen == std::string::npos ? std::string::npos : matchEnd(game, playersOpen, '[', ']');
            if (playersClose == std::string::npos) continue;
            int names = 0;
            for (size_t p = playersOpen; p < playersClose; ) {
                p = game.find("\"name\"", p);
                if (p == std::string::npos || p >= playersClose) break;
                ++names;
                p += 6;
            }
            counts[id] = names;
            break;
        }
    }
}

static void fetchOnce() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_snap.ok) {
            g_snap.summary = "Connecting...";
            g_snap.generation++;
        }
    }
    Net net;
    char ip[16]{};
    if (!openNet(net, ip, sizeof ip)) {
        Snap failed;
        failed.summary = "No network";
        publish(failed);
        return;
    }
    g_bodyCap = 256 * 1024;
    std::string body;
    bool ok = httpGet("https://nextendo.network/api/online-counts", ip, body);
    if (!ok && !stopped()) {
        const char* fallback = std::strcmp(ip, "188.114.96.1") == 0 ? "188.114.97.1" : "188.114.96.1";
        ok = httpGet("https://nextendo.network/api/online-counts", fallback, body);
        if (ok) std::snprintf(ip, sizeof ip, "%s", fallback);
    }
    if (stopped()) {
        closeNet(net);
        return;
    }
    if (!ok) {
        closeNet(net);
        Snap failed;
        failed.summary = "Couldn't load";
        publish(failed);
        return;
    }
    Counts counts;
    for (const auto& item : kCatalog) readIds(body, counts, item.ids, false);
    body.clear();
    body.shrink_to_fit();
    auto show = [&] {
        if (stopped()) return;
        Snap next;
        int players = 0;
        fillRows(counts, next.rows, players);
        next.ok = true;
        next.summary = std::to_string(players);
        publish(next);
    };
    show();

    setConnected(ip, counts, "https://nextendo.network/game-stats/sm3dw", "010028600ebda000", false);
    setConnected(ip, counts, "https://nextendo.network/game-stats/jamboree", "0100965017338000", true);
    setConnected(ip, counts, "https://nextendo.network/game-stats/kirby", "0100a8e016236000", true);
    setConnected(ip, counts, "https://nextendo.network/game-stats/justdance22", "0100ea6014bb8000", true);

    if (!stopped()) {
        for (const char* id : {"0100d870045b6000", "01008d300c50c000", "0100c62011050000", "010012f017576000",
                "0100b3c014bda000", "0100fcf002a58000", "0100ab401f136000", "01003fb00c5a8000",
                "0100227010460000", "0100e46006708000", "01001cc01b2d4000", "0100e65002bb8000"})
            counts.erase(id);
        if (httpGet("https://nextendo.network/service-catalog/v1/online-counts", ip, body, nullptr, 5))
            readIds(body, counts, kServiceIds, false);
        body.clear();
        body.shrink_to_fit();
    }
    if (!stopped()) {
        int n = 0;
        if (httpGet("https://nextendo.network/game-stats/uno-count", ip, body, nullptr, 5) &&
            body.find("\"game\":\"UNO\"") != std::string::npos && jsonInt(body, "connected", n) && n >= 0)
            counts["0100b6e012ebe000"] = n;
        body.clear();
    }
    if (!stopped()) {
        counts.erase("0100cea007d08000");
        int n = 0;
        if (httpGet("https://nextendo.network/service-catalog/v1/crypt-status", ip, body, nullptr, 5) &&
            jsonOnline(body) && body.find("room_members") != std::string::npos &&
            jsonInt(body, "players", n) && n >= 0 && n <= 1024)
            counts["0100cea007d08000"] = n;
        body.clear();
    }
    if (!stopped()) {
        counts.erase("01004cc023582000");
        int n = 0;
        if (httpGet("https://nextendo.network/service-catalog/v1/sr2-status", ip, body, nullptr, 5) &&
            jsonOnline(body) && body.find("native_transport_connections") != std::string::npos &&
            jsonInt(body, "players", n) && n >= 0 && n <= 50000)
            counts["01004cc023582000"] = n;
        body.clear();
    }
    if (!stopped()) {
        const std::string token = loadToken();
        if (!token.empty()) {
            g_bodyCap = 1024 * 1024;
            if (httpGet("https://nextendo.network/service-catalog/v1/status/live", ip, body, token.c_str(), 20, true))
                applyLiveCounts(body, counts);
            body.clear();
            body.shrink_to_fit();
            g_bodyCap = 256 * 1024;
        }
    }
    closeNet(net);
    show();
}

static std::string clip(std::string text, size_t limit) {
    if (text.size() > limit) text.resize(limit);
    return text;
}

static std::string rawString(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = obj.find(pat);
    if (k == std::string::npos) return {};
    const auto colon = obj.find(':', k + pat.size());
    if (colon == std::string::npos) return {};
    const auto q1 = obj.find('"', colon + 1);
    if (q1 == std::string::npos || q1 > colon + 8) return {};
    const auto q2 = obj.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return clip(obj.substr(q1 + 1, q2 - q1 - 1), 64);
}

static int rawInt(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = obj.find(pat);
    if (k == std::string::npos) return -1;
    const auto colon = obj.find(':', k + pat.size());
    if (colon == std::string::npos) return -1;
    return std::atoi(obj.c_str() + colon + 1);
}

static size_t matchClose(const std::string& body, size_t open) {
    if (open >= body.size() || body[open] != '{') return std::string::npos;
    bool str = false;
    bool esc = false;
    int depth = 0;
    for (size_t i = open; i < body.size(); ++i) {
        const char c = body[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

static std::string rawArray(const std::string& obj, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = obj.find(pat);
    if (k == std::string::npos) return {};
    const auto open = obj.find('[', k + pat.size());
    if (open == std::string::npos) return {};
    bool str = false;
    bool esc = false;
    int depth = 0;
    for (size_t i = open; i < obj.size(); ++i) {
        const char c = obj[i];
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        else if (c == '[') ++depth;
        else if (c == ']') {
            --depth;
            if (depth == 0) return obj.substr(open, i - open + 1);
        }
    }
    return {};
}

static std::vector<std::string> rawObjects(const std::string& arr, size_t limit) {
    std::vector<std::string> out;
    if (arr.empty()) return out;
    bool str = false;
    bool esc = false;
    int depth = 0;
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
                if (out.size() >= limit) break;
            }
        }
    }
    return out;
}

static bool sameId(const std::string& body, size_t at, const std::string& id) {
    if (at == 0 || at + id.size() >= body.size() || body[at - 1] != '"' || body[at + id.size()] != '"') return false;
    for (size_t i = 0; i < id.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(body[at + i]);
        unsigned char b = static_cast<unsigned char>(id[i]);
        if (a >= 'A' && a <= 'F') a = static_cast<unsigned char>(a + 32);
        if (b >= 'A' && b <= 'F') b = static_cast<unsigned char>(b + 32);
        if (a != b) return false;
    }
    return true;
}

static std::string gameObject(const std::string& body, const std::string& titleId) {
    if (titleId.empty()) return {};
    std::string best;
    for (size_t at = 0; at + titleId.size() < body.size(); ++at) {
        if (!sameId(body, at, titleId)) continue;
        const auto open = body.rfind('{', at);
        if (open == std::string::npos) continue;
        const auto close = matchClose(body, open);
        if (close == std::string::npos) continue;
        std::string game = body.substr(open, close - open + 1);
        if ((game.find("\"lobbies\"") != std::string::npos || game.find("\"players\"") != std::string::npos) && game.size() > best.size())
            best = std::move(game);
        at += titleId.size();
    }
    return best;
}

static void publishDetail(DetailSnap next) {
    std::lock_guard<std::mutex> lock(g_mu);
    next.generation = g_detailSnap.generation + 1;
    next.ready = true;
    g_detailSnap = std::move(next);
}

static Person personFrom(const std::string& obj) {
    Person person;
    person.name = rawString(obj, "name");
    if (obj.find("\"you\":true") != std::string::npos) person.mark = "You";
    else if (obj.find("\"friend\":true") != std::string::npos) person.mark = "Friend";
    return person;
}

static DetailSnap lobbiesFrom(const std::string& game) {
    DetailSnap page;
    const bool roomsHidden = game.find("\"roomsAvailable\":false") != std::string::npos || game.find("\"nolist\":true") != std::string::npos;
    if (!roomsHidden) {
        const auto lobbies = rawObjects(rawArray(game, "lobbies"), 24);
        for (size_t i = 0; i < lobbies.size(); ++i) {
            Room room;
            const std::string mode = rawString(lobbies[i], "mode");
            room.title = "Lobby " + std::to_string(i + 1);
            if (!mode.empty()) room.title += "  " + mode;
            const int count = rawInt(lobbies[i], "count");
            const int max = rawInt(lobbies[i], "max");
            if (count >= 0 && max > 0) room.places = std::to_string(count) + "/" + std::to_string(max);
            else if (count >= 0) room.places = std::to_string(count);
            for (const std::string& member : rawObjects(rawArray(lobbies[i], "players"), 32)) {
                Person person = personFrom(member);
                if (!person.name.empty()) room.members.push_back(std::move(person));
            }
            page.rooms.push_back(std::move(room));
        }
    }
    for (const std::string& player : rawObjects(rawArray(game, "players"), 48)) {
        Person person = personFrom(player);
        if (!person.name.empty()) page.players.push_back(std::move(person));
    }
    if (roomsHidden) page.note = "Rooms are temporarily unavailable.";
    return page;
}

static std::string loadToken() {
    std::string token;
    tsl::hlp::doWithSDCardHandle([&] {
        AppConfig config;
        config.load(kSharedConfigPath);
        token = std::move(config.access_token);
    });
    return token;
}

static void fetchDetail() {
    std::string titleId;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        titleId = g_detailTitle;
    }
    token = loadToken();
    if (token.empty()) {
        DetailSnap page;
        page.note = "Not signed in";
        page.lines.push_back({"Sign in with the NexStatus app.", {}});
        page.lines.push_back({"Hold R and launch a game", {}});
        page.lines.push_back({"to open the Homebrew Menu.", {}});
        page.lines.push_back({"Start NexStatus and sign in,", {}});
        page.lines.push_back({"then open this overlay again.", {}});
        publishDetail(std::move(page));
        return;
    }
    if (titleId.empty()) {
        DetailSnap page;
        page.note = "This game has no title id.";
        publishDetail(std::move(page));
        return;
    }
    Net net;
    char ip[16]{};
    if (!openNet(net, ip, sizeof ip)) {
        DetailSnap page;
        page.note = "No network";
        publishDetail(std::move(page));
        return;
    }
    const u64 freq = armGetSystemTickFreq();
    g_deadline = armGetSystemTick() + (freq ? freq * 20 : 0);
    g_bodyCap = 1024 * 1024;
    std::string body;
    std::string game;
    const bool detailed = detailedTitle(titleId);
    if (detailed && g_detail.load() && !g_abort.load() &&
        httpGet("https://nextendo.network/service-catalog/v1/status/live", ip, body, token.c_str(), 20, true))
        game = gameObject(body, titleId);
    body.clear();
    body.shrink_to_fit();
    if (game.empty() && g_detail.load() && !g_abort.load() &&
        httpGet("https://nextendo.network/api/status/live", ip, body, token.c_str(), 20, true))
        game = gameObject(body, titleId);
    body.clear();
    body.shrink_to_fit();
    if (!detailed && game.empty() && g_detail.load() && !g_abort.load() &&
        httpGet("https://nextendo.network/service-catalog/v1/status/live", ip, body, token.c_str(), 20, true))
        game = gameObject(body, titleId);
    body.clear();
    body.shrink_to_fit();
    g_deadline = 0;
    closeNet(net);
    if (!g_detail.load() || g_abort.load()) return;
    if (game.empty()) {
        DetailSnap page;
        page.note = "Couldn't load lobbies.";
        publishDetail(std::move(page));
        return;
    }
    DetailSnap page = lobbiesFrom(game);
    game.clear();
    game.shrink_to_fit();
    publishDetail(std::move(page));
}

static void worker(void*) {
    while (g_run.load()) {
        if (g_detail.load()) {
            if (g_detailFetch.exchange(false)) {
                g_abort = false;
                fetchDetail();
            }
            svcSleepThread(100'000'000);
            continue;
        }
        if (!g_open.load()) {
            svcSleepThread(100'000'000);
            continue;
        }
        const bool refresh = g_refresh.exchange(false);
        if (g_fetched.load() && !refresh) {
            svcSleepThread(100'000'000);
            continue;
        }
        g_abort = false;
        fetchOnce();
        g_fetched = true;
    }
}

class LobbyGui : public tsl::Gui {
    std::string title_;
    std::vector<Person> members_;

public:
    LobbyGui(std::string title, std::vector<Person> members)
        : title_(std::move(title)), members_(std::move(members)) {}

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("NexStatus", title_);
        auto* rows = new tsl::elm::List();
        if (members_.empty()) rows->addItem(new tsl::elm::ListItem("No players in this lobby."));
        else for (const auto& member : members_)
            rows->addItem(new tsl::elm::ListItem(member.name, member.mark));
        frame->setContent(rows);
        return frame;
    }
};

class GameGui : public tsl::Gui {
    tsl::elm::List* list = nullptr;
    int shown = -1;
    std::string name_;
    std::string titleId_;
    int players_ = 0;

    void rebuild(const DetailSnap& snap) {
        removeFocus();
        list->clear();
        auto* refresh = new tsl::elm::ListItem("Refresh");
        refresh->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                g_detailFetch = true;
                return true;
            }
            return false;
        });
        list->addItem(refresh);
        const bool failed = snap.rooms.empty() && snap.players.empty() && !snap.note.empty() && snap.note != "Rooms are temporarily unavailable.";
        if (failed) {
            list->addItem(new tsl::elm::ListItem(snap.note));
            for (const auto& line : snap.lines)
                list->addItem(new tsl::elm::ListItem(line.name));
            shown = snap.generation;
            return;
        }
        const int count = g_detailPlayers;
        list->addItem(new tsl::elm::ListItem(std::to_string(count) + (count == 1 ? " player" : " players")));
        list->addItem(new tsl::elm::CategoryHeader("Lobbies"));
        if (snap.rooms.empty()) {
            list->addItem(new tsl::elm::ListItem(snap.note.empty() ? "No lobbies right now." : snap.note.c_str()));
        } else {
            for (const auto& room : snap.rooms) {
                auto* item = new tsl::elm::ListItem(room.title, room.places);
                const std::string title = room.title;
                const std::vector<Person> members = room.members;
                item->setClickListener([title, members](u64 keys) {
                    if (keys & HidNpadButton_A) {
                        tsl::changeTo<LobbyGui>(title, members);
                        return true;
                    }
                    return false;
                });
                list->addItem(item);
            }
        }
        if (!snap.players.empty()) {
            list->addItem(new tsl::elm::CategoryHeader("Players"));
            for (const auto& player : snap.players)
                list->addItem(new tsl::elm::ListItem(player.name, player.mark));
        }
        shown = snap.generation;
    }

public:
    GameGui(std::string name, std::string titleId, int players)
        : name_(std::move(name)), titleId_(std::move(titleId)), players_(players) {}

    ~GameGui() override {
        g_abort = true;
        g_detail = false;
        std::lock_guard<std::mutex> lock(g_mu);
        g_detailSnap = DetailSnap{};
        g_detailTitle.clear();
        g_detailToken.clear();
    }

    tsl::elm::Element* createUI() override {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_detailTitle = titleId_;
            g_detailPlayers = players_;
            g_detailToken.clear();
            g_detailSnap = DetailSnap{};
            g_detailSnap.note = "Loading...";
        }
        g_abort = true;
        g_detail = true;
        g_detailFetch = true;
        auto* frame = new tsl::elm::OverlayFrame("NexStatus", name_);
        list = new tsl::elm::List();
        list->addItem(new tsl::elm::ListItem("Loading..."));
        frame->setContent(list);
        return frame;
    }

    void update() override {
        DetailSnap snap;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            snap = g_detailSnap;
        }
        if (list == nullptr || !snap.ready || snap.generation == shown) return;
        rebuild(snap);
    }
};

class StatusGui : public tsl::Gui {
    tsl::elm::List* list = nullptr;
    int shown = -1;

    void rebuild(const Snap& snap) {
        removeFocus();
        list->clear();
        auto* refresh = new tsl::elm::ListItem("Refresh");
        refresh->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                g_refresh = true;
                return true;
            }
            return false;
        });
        list->addItem(refresh);
        list->addItem(new tsl::elm::ListItem("Online", snap.summary));
        if (!snap.rows.empty())
            list->addItem(new tsl::elm::CategoryHeader("Games"));
        for (const auto& row : snap.rows) {
            auto* item = new tsl::elm::ListItem(row.name, std::to_string(row.players));
            const std::string name = row.name;
            const std::string titleId = row.titleId;
            const int players = row.players;
            item->setClickListener([name, titleId, players](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<GameGui>(name, titleId, players);
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }
        shown = snap.generation;
    }

public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("NexStatus", "Servers");
        list = new tsl::elm::List();
        frame->setContent(list);
        return frame;
    }

    void update() override {
        g_open = true;
        Snap snap;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            snap = g_snap;
        }
        if (list == nullptr || snap.generation == shown) return;
        rebuild(snap);
    }
};

class NexOverlay : public tsl::Overlay {
public:
    void initServices() override {
        timeInitialize();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_run = true;
        if (R_SUCCEEDED(threadCreate(&g_workerThread, worker, nullptr, g_workerStack, sizeof(g_workerStack), 0x2C, -2))) {
            g_workerStarted = R_SUCCEEDED(threadStart(&g_workerThread));
            if (!g_workerStarted) threadClose(&g_workerThread);
        }
    }

    void exitServices() override {
        g_run = false;
        g_abort = true;
        g_open = false;
        g_detail = false;
        if (g_workerStarted) {
            s32 index = -1;
            const Result waited = svcWaitSynchronization(&index, &g_workerThread.handle, 1, 300'000'000ULL);
            if (R_SUCCEEDED(waited)) {
                threadClose(&g_workerThread);
                g_workerStarted = false;
                curl_global_cleanup();
            }
        } else {
            curl_global_cleanup();
        }
        timeExit();
    }

    void onShow() override { g_open = true; }
    void onHide() override {
        g_abort = true;
        g_open = false;
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<StatusGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<NexOverlay>(argc, argv);
}
