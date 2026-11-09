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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Tesla's framebuffer already uses most of a 4 MiB heap, so the BSD buffer has to stay small.
extern "C" u64 __nx_heap_size = 0x800000;

struct Row {
    std::string name;
    int players = 0;
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
static std::mutex g_mu;
static Snap g_snap;
static std::thread g_worker;

static size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (g_abort.load() || !g_run.load()) return 0;
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > 96 * 1024) return 0;
    out->append(ptr, n);
    return n;
}

static std::atomic<u64> g_deadline{0};

static int curlAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    if (g_deadline.load() != 0 && armGetSystemTick() > g_deadline.load()) return 1;
    return (g_abort.load() || !g_run.load()) ? 1 : 0;
}

static CURLcode httpGet(const char* url, const char* ip, std::string& body) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
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
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    curl_slist_free_all(resolve);
    if (rc != CURLE_OK || http != 200 || body.empty()) return rc == CURLE_OK ? CURLE_HTTP_RETURNED_ERROR : rc;
    return CURLE_OK;
}

static void parseJeux(const std::string& body, std::vector<Row>& rows, int& players) {
    rows.clear();
    players = 0;
    size_t pos = body.find("\"jeux\"");
    if (pos == std::string::npos) return;
    const auto noms = body.find("\"noms\"");
    while (rows.size() < 80) {
        const auto nom = body.find("\"nom\":\"", pos);
        if (nom == std::string::npos || (noms != std::string::npos && nom > noms)) break;
        const auto n1 = nom + 7;
        const auto n2 = body.find('"', n1);
        if (n2 == std::string::npos) break;
        const auto joueurs = body.find("\"joueurs\":", n2);
        if (joueurs == std::string::npos) break;
        Row row;
        row.name = body.substr(n1, n2 - n1);
        for (size_t i = 0; i + 5 < row.name.size(); ) {
            if (row.name[i] == '\\' && row.name[i + 1] == 'u') {
                int cp = 0;
                bool hex = true;
                for (int k = 0; k < 4; ++k) {
                    const char h = row.name[i + 2 + k];
                    int v = 0;
                    if (h >= '0' && h <= '9') v = h - '0';
                    else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                    else { hex = false; break; }
                    cp = (cp << 4) | v;
                }
                if (hex && cp > 0 && cp < 128) {
                    row.name.replace(i, 6, 1, static_cast<char>(cp));
                    continue;
                }
            }
            ++i;
        }
        row.players = std::atoi(body.c_str() + joueurs + 10);
        if (row.players < 0) row.players = 0;
        players += row.players;
        rows.push_back(std::move(row));
        pos = n2 + 1;
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
    resolverSetEnableServiceDiscovery(false);
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo("nextendo.network", "443", &hints, &res) == 0 && res != nullptr && res->ai_addr != nullptr) {
        inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr, ip, ipLen);
        freeaddrinfo(res);
        return ip[0] != '\0';
    }
    if (res) freeaddrinfo(res);
    u32 servers[4];
    int count = 0;
    u32 addr = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&addr, &mask, &gateway, &dns1, &dns2))) {
        if (dns1) servers[count++] = dns1;
        if (dns2) servers[count++] = dns2;
    }
    servers[count++] = htonl(0x01010101);
    if (count < 4) servers[count++] = htonl(0x08080808);
    struct in_addr found{};
    for (int i = 0; i < count; ++i) {
        if (!queryA("nextendo.network", servers[i], &found)) continue;
        inet_ntop(AF_INET, &found, ip, ipLen);
        return ip[0] != '\0';
    }
    return false;
}

static bool openNet(Net& net, char* ip, size_t ipLen) {
    SocketInitConfig cfg{};
    cfg.tcp_tx_buf_size = 0x4000;
    cfg.tcp_rx_buf_size = 0x4000;
    cfg.tcp_tx_buf_max_size = 0x8000;
    cfg.tcp_rx_buf_max_size = 0x8000;
    cfg.udp_tx_buf_size = 0x800;
    cfg.udp_rx_buf_size = 0x800;
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
    const u64 freq = armGetSystemTickFreq();
    g_deadline = armGetSystemTick() + (freq ? freq * 20 : 0);
    std::string body;
    const CURLcode rc = httpGet("https://nextendo.network/api/online-counts", ip, body);
    g_deadline = 0;
    closeNet(net);
    if (rc != CURLE_OK || g_abort.load()) {
        Snap failed;
        failed.summary = std::string("Couldn't load ") + std::to_string(static_cast<int>(rc));
        publish(failed);
        return;
    }
    Snap next;
    int players = 0;
    parseJeux(body, next.rows, players);
    next.ok = true;
    next.summary = std::to_string(players);
    publish(next);
}

static void worker() {
    while (g_run.load()) {
        if (!g_open.load()) {
            svcSleepThread(100'000'000);
            continue;
        }
        g_abort = false;
        fetchOnce();
        for (int i = 0; i < 150 && g_run.load() && g_open.load() && !g_refresh.load(); ++i)
            svcSleepThread(100'000'000);
        g_refresh = false;
    }
}

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
        for (const auto& row : snap.rows)
            list->addItem(new tsl::elm::ListItem(row.name, std::to_string(row.players)));
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
        g_worker = std::thread(worker);
    }

    void exitServices() override {
        g_run = false;
        g_abort = true;
        g_open = false;
        if (g_worker.joinable()) g_worker.join();
        curl_global_cleanup();
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
