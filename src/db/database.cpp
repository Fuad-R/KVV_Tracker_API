#include "database.h"

bool Database::loadConfig(const std::string& path) {
    config_ = loadDbConfig(path);
    return config_.has_value();
}

bool Database::loadConfigFromEnv() {
    config_ = loadDbConfigFromEnv();
    return config_.has_value();
}

bool Database::hasConfig() const {
    return config_.has_value();
}

PGconn* Database::connect() const {
    if (!config_) return nullptr;
    const auto& cfg = *config_;
    const char* keywords[] = {"host", "port", "dbname", "user", "password", "sslmode", nullptr};
    const char* values[] = {
        cfg.host.c_str(),
        cfg.port.c_str(),
        cfg.dbname.c_str(),
        cfg.user.c_str(),
        cfg.password.c_str(),
        cfg.sslmode.empty() ? nullptr : cfg.sslmode.c_str(),
        nullptr
    };
    return PQconnectdbParams(keywords, values, 0);
}

const DbConfig& Database::config() const {
    return *config_;
}
