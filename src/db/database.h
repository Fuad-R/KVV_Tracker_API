#pragma once

#include "../config/config.h"
#include <libpq-fe.h>

class Database {
public:
    bool loadConfig(const std::string& path);
    bool hasConfig() const;
    PGconn* connect() const;
    const DbConfig& config() const;

private:
    std::optional<DbConfig> config_;
};
