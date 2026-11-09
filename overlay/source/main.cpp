#define TESLA_INIT_IMPL
#include <tesla.hpp>

#define CURL_STATICLIB
#include <curl/curl.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
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
    if (armGetSystemTick() > g_deadline.load()) return 1;
    return (g_abort.load() || !g_run.load() || !g_open.load()) ? 1 : 0;
}

static bool httpGet(const char* url, std::string& body) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NexStatus");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlAbort);
    const CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && http == 200 && !body.empty();
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

static bool openSocket(bool& own) {
    own = false;
    SocketInitConfig cfg{};
    cfg.tcp_tx_buf_size = 0x800;
    cfg.tcp_rx_buf_size = 0x800;
    cfg.tcp_tx_buf_max_size = 0x2500;
    cfg.tcp_rx_buf_max_size = 0x2500;
    cfg.udp_tx_buf_size = 0x800;
    cfg.udp_rx_buf_size = 0x800;
    cfg.sb_efficiency = 1;
    cfg.num_bsd_sessions = 1;
    cfg.bsd_service_type = BsdServiceType_User;
    Result rc = 1;
    bool nifmOwn = false;
    tsl::hlp::doWithSmSession([&] {
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) nifmOwn = true;
        rc = socketInitialize(&cfg);
    });
    if (R_SUCCEEDED(rc)) {
        own = true;
        return true;
    }
    if (R_DESCRIPTION(rc) == LibnxError_AlreadyInitialized) return true;
    if (nifmOwn) nifmExit();
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
    bool own = false;
    if (!openSocket(own)) {
        Snap failed;
        failed.summary = "No network";
        publish(failed);
        return;
    }
    const u64 freq = armGetSystemTickFreq();
    g_deadline = armGetSystemTick() + (freq ? freq * 12 : 0);
    std::string body;
    const bool ok = httpGet("https://nextendo.network/api/online-counts", body);
    if (own) {
        socketExit();
        nifmExit();
    }
    if (!ok || g_abort.load() || !g_open.load()) {
        Snap failed;
        failed.summary = "Couldn't load";
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
