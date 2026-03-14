#pragma once

#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <map>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>
#include <iostream>
#include <locale>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// --- Configuration Constants ---
inline const std::string Provider_DM_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
inline const std::string Provider_SEARCH_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";
inline const std::string DB_CONFIG_PATH = "db_connection.txt";
inline const std::string DB_CONFIG_CONTAINER_PATH = "/config/db_connection.txt";
inline constexpr long UPSTREAM_TIMEOUT_SECONDS = 15;
inline constexpr size_t MAX_QUERY_LENGTH = 200;
inline constexpr size_t MAX_STOPID_LENGTH = 100;
inline constexpr int CACHE_TTL_SECONDS = 30;
inline constexpr size_t MAX_CACHE_ENTRIES = 10000;

inline const std::vector<std::string> NOTIFICATION_API_PROVIDERS = {
    "https://www.efa-bw.de/nvbw/",
    "https://efa.vrr.de/standard/"
};

// --- Cache Structure ---
struct CacheEntry {
    json data;
    std::chrono::steady_clock::time_point timestamp;
};

// --- Database Configuration ---
struct DbConfig {
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
    std::string sslmode;
};

// --- String Utilities ---
inline std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

inline std::string trim(const std::string& input) {
    auto start = std::find_if_not(input.begin(), input.end(),
                                  [](unsigned char c){ return std::isspace(c); });
    auto end = std::find_if_not(input.rbegin(), input.rend(),
                                [](unsigned char c){ return std::isspace(c); }).base();
    if (start >= end) return "";
    return std::string(start, end);
}

// --- Input Validation ---
inline bool isValidStopId(const std::string& stopId) {
    if (stopId.empty() || stopId.size() > MAX_STOPID_LENGTH) return false;
    static const std::regex stopIdPattern("^[a-zA-Z0-9:_. -]+$");
    return std::regex_match(stopId, stopIdPattern);
}

inline bool isValidSearchQuery(const std::string& query) {
    if (query.empty() || query.size() > MAX_QUERY_LENGTH) return false;
    for (unsigned char c : query) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// --- Cache Eviction (caller must hold the appropriate mutex) ---
inline void evictExpiredCacheEntries(std::map<std::string, CacheEntry>& cache) {
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache.begin(); it != cache.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count() >= CACHE_TTL_SECONDS) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

// --- JSON Utilities ---
inline std::optional<std::string> jsonToString(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_float()) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(15) << value.get<double>();
        return stream.str();
    }
    return std::nullopt;
}

inline std::optional<double> jsonToDouble(const json& value) {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> getJsonString(const json& obj, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (obj.contains(key)) {
            auto value = jsonToString(obj.at(key));
            if (value && !value->empty()) return value;
        }
    }
    return std::nullopt;
}

inline std::string formatDouble(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(8) << value;
    return stream.str();
}

// --- Database Config Loading from Environment Variables ---
inline std::optional<DbConfig> loadDbConfigFromEnv() {
    auto getEnv = [](const char* name) -> std::string {
        const char* val = std::getenv(name);
        return (val && val[0] != '\0') ? std::string(val) : "";
    };

    DbConfig config;
    config.host     = getEnv("DB_HOST");
    config.port     = getEnv("DB_PORT");
    config.dbname   = getEnv("DB_NAME");
    config.user     = getEnv("DB_USER");
    config.password  = getEnv("DB_PASSWORD");
    config.sslmode  = getEnv("DB_SSLMODE");

    if (config.host.empty() || config.port.empty() || config.dbname.empty() ||
        config.user.empty() || config.password.empty()) {
        return std::nullopt;
    }

    if (config.sslmode.empty()) {
        config.sslmode = "require";
    }

    return config;
}

// --- Database Config Loading from File ---
inline std::optional<DbConfig> loadDbConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Database config not found: " << path << std::endl;
        return std::nullopt;
    }

    DbConfig config;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, pos)));
        std::string value = trim(line.substr(pos + 1));
        if (key == "host") config.host = value;
        else if (key == "port") config.port = value;
        else if (key == "dbname") config.dbname = value;
        else if (key == "user") config.user = value;
        else if (key == "password") config.password = value;
        else if (key == "sslmode") config.sslmode = value;
    }

    if (config.host.empty() || config.port.empty() || config.dbname.empty() ||
        config.user.empty() || config.password.empty()) {
        std::cerr << "Database config missing required fields in: " << path << std::endl;
        return std::nullopt;
    }

    if (config.sslmode.empty()) {
        config.sslmode = "require";
    }

    return config;
}
