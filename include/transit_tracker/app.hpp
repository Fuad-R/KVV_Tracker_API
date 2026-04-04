#pragma once

#include <chrono>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "crow.h"
#include <libpq-fe.h>

#include "transit_tracker/core.hpp"

namespace transit_tracker {

struct AppDependencies {
    std::function<json(const std::string&, const std::string&, bool)> search_stops;
    std::function<json(const std::string&)> fetch_departures;
    std::function<json(const std::string&, const std::string&)> fetch_notifications_from_provider;
    std::function<void(const json&, const std::string&)> ensure_stops_in_database;
    std::function<bool(const std::string&)> validate_api_key;
    std::function<std::chrono::steady_clock::time_point()> steady_now;
    std::function<std::time_t()> system_now;
};

struct AppState {
    std::mutex cache_mutex;
    std::map<std::string, CacheEntry> stop_cache;

    bool auth_enabled = false;
    std::string api_key;

    std::optional<DbConfig> db_config;
    std::mutex auth_db_mutex;
    PGconn* auth_db_conn = nullptr;
    std::map<std::string, std::chrono::steady_clock::time_point> last_used_updates;

    AppState() = default;
    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;
    ~AppState();
};

using TransitApp = crow::SimpleApp;

void setSecurityHeaders(crow::response& res);
crow::response unauthorizedResponse();

crow::response handleHealth();
crow::response handleSearchStops(const crow::request& req, AppState& state, const AppDependencies& dependencies);
crow::response handleDepartures(
    const crow::request& req,
    const std::string& stop_id,
    AppState& state,
    const AppDependencies& dependencies);
crow::response handleCurrentNotifications(
    const crow::request& req,
    AppState& state,
    const AppDependencies& dependencies);

std::unique_ptr<TransitApp> createApp(const std::shared_ptr<AppState>& state, AppDependencies dependencies);

}  // namespace transit_tracker
