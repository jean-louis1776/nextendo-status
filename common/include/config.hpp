#pragma once
#include <string>
#include <fstream>
#include <sys/stat.h>

inline constexpr const char* kSharedConfigPath = "sdmc:/config/nexstatus/config.ini";

struct AppConfig {
    std::string access_token;
    std::string refresh_token;

    bool load(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return false;
        std::string line;
        while (std::getline(file, line)) {
            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            if (!value.empty() && value.back() == '\r') value.pop_back();
            if (key == "access_token") access_token = value;
            else if (key == "refresh_token") refresh_token = value;
        }
        return true;
    }

    bool saveShared() {
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/nexstatus", 0777);
        std::ofstream file(kSharedConfigPath);
        if (!file.is_open()) return false;
        file << "access_token=" << access_token << "\n";
        file << "refresh_token=" << refresh_token << "\n";
        return true;
    }
};
