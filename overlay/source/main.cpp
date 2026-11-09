#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <curl/curl.h>
#include <switch.h>

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

// Структура для хранения данных статуса
struct NextendoData {
    std::string onlineCount = "---";
    std::string liveStatus  = "---";
    std::string username    = "---";
    std::string statusText  = "Ожидание данных...";
};

// Буфер для приема данных libcurl
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    // Ограничиваем размер ответа в буфере до 64 КБ для защиты ОЗУ
    if (str->size() + totalSize > 65536) return 0;
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// Легковесный GET-запрос через curl
static std::string httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    std::string response;

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "NextendoOverlay/1.0 (Nintendo Switch)");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);

        // Отключаем проверку сертификатов для экономии памяти на bundle сертификатов
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    return response;
}

// Простое извлечение значений без тяжелого AST-дерева в памяти
static std::string extractJsonValue(const std::string& json, const std::string& key) {
    std::string target = "\"" + key + "\"";
    size_t pos = json.find(target);
    if (pos == std::string::npos) return "N/A";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "N/A";

    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "N/A";

    if (json[pos] == '"') {
        size_t end = json.find('"', pos + 1);
        if (end != std::string::npos) {
            return json.substr(pos + 1, end - pos - 1);
        }
    } else {
        size_t end = json.find_first_of(",}\n\r ", pos);
        if (end != std::string::npos) {
            return json.substr(pos, end - pos);
        }
    }
    return "N/A";
}

// Глобальное состояние для передачи данных между оверлеем и потоком
static std::atomic<bool> g_isOverlayVisible(false);
static std::atomic<bool> g_shouldExit(false);
static std::mutex g_dataMutex;
static NextendoData g_statusData;
static std::thread g_workerThread;

// Фоновый цикл обновления
static void networkWorkerLoop() {
    auto lastUpdate = std::chrono::steady_clock::now() - std::chrono::seconds(20);

    while (!g_shouldExit.load()) {
        if (g_isOverlayVisible.load()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdate).count();

            // Запросы уходят только если оверлей открыт и прошло не менее 15 секунд
            if (elapsed >= 15) {
                // 1. Запрос online-counts
                std::string resOnline = httpGet("https://nextendo.network/api/online-counts");
                // 2. Запрос live
                std::string resLive = httpGet("https://nextendo.network/api/live");
                // 3. Запрос me
                std::string resMe = httpGet("https://nextendo.network/api/me");

                NextendoData temp;
                temp.onlineCount = resOnline.empty() ? "Offline" : extractJsonValue(resOnline, "online");
                if (temp.onlineCount == "N/A") temp.onlineCount = resOnline.substr(0, 8);

                temp.liveStatus = resLive.empty() ? "Offline" : extractJsonValue(resLive, "status");
                if (temp.liveStatus == "N/A") temp.liveStatus = "Active";

                temp.username = resMe.empty() ? "Guest" : extractJsonValue(resMe, "username");

                temp.statusText = "OK (" + std::to_string(elapsed) + "s)";

                {
                    std::lock_guard<std::mutex> lock(g_dataMutex);
                    g_statusData = temp;
                }
                lastUpdate = std::chrono::steady_clock::now();
            }
        }

        // Сон 250 мс, чтобы не нагружать процессор
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// Графический интерфейс
class NextendoGui : public tsl::Gui {
private:
    tsl::elm::ListItem* itemOnline;
    tsl::elm::ListItem* itemLive;
    tsl::elm::ListItem* itemUser;

public:
    NextendoGui() {}

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("Nextendo Network", "Status");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Мониторинг сети"));

        itemOnline = new tsl::elm::ListItem("Онлайн:", "Загрузка...");
        itemLive   = new tsl::elm::ListItem("Сервер:", "Загрузка...");
        itemUser   = new tsl::elm::ListItem("Аккаунт:", "Загрузка...");

        list->addItem(itemOnline);
        list->addItem(itemLive);
        list->addItem(itemUser);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        // Обновляем текст на экране из закэшированных данных
        std::lock_guard<std::mutex> lock(g_dataMutex);
        if (itemOnline) itemOnline->setValue(g_statusData.onlineCount);
        if (itemLive)   itemLive->setValue(g_statusData.liveStatus);
        if (itemUser)   itemUser->setValue(g_statusData.username);
    }
};

// Главный класс оверлея
class NextendoOverlay : public tsl::Overlay {
public:
    void initServices() override {
        // Выделение памяти под сетевые сокеты
        socketInitializeDefault();
        curl_global_init(CURL_GLOBAL_DEFAULT);

        g_shouldExit = false;
        g_workerThread = std::thread(networkWorkerLoop);
    }

    void exitServices() override {
        g_shouldExit = true;
        if (g_workerThread.joinable()) {
            g_workerThread.join();
        }

        curl_global_cleanup();
        socketExit();
    }

    void onShow() override {
        // Оверлей развернут: разрешаем отправку запросов
        g_isOverlayVisible = true;
    }

    void onHide() override {
        // Оверлей скрыт: сетевые запросы мгновенно останавливаются
        g_isOverlayVisible = false;
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return std::make_unique<NextendoGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<NextendoOverlay>(argc, argv);
}