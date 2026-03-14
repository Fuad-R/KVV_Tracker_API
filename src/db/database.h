#pragma once

#include "../config/config.h"
#include <libpq-fe.h>
#include <string>
#include <optional>

class Database {
public:
    bool loadConfig(const std::string& path);
    bool loadConfigFromEnv();
    bool hasConfig() const;
    PGconn* connect() const;
    const DbConfig& config() const;

private:
    std::optional<DbConfig> config_;
};
