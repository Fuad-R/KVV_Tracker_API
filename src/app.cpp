#include "transit_tracker/app.hpp"

#include <utility>

namespace transit_tracker {

namespace {

AppDependencies withDefaults(AppDependencies dependencies) {
    if (!dependencies.search_stops) {
        dependencies.search_stops = [](const std::string&, const std::string&, bool) {
            return json::array();
        };
    }
    if (!dependencies.fetch_departures) {
        dependencies.fetch_departures = [](const std::string&) {
            return json::object({{"error", "No departure provider configured"}});
        };
    }
    if (!dependencies.fetch_notifications_from_provider) {
        dependencies.fetch_notifications_from_provider = [](const std::string&, const std::string&) {
            return json::object();
        };
    }
    if (!dependencies.ensure_stops_in_database) {
        dependencies.ensure_stops_in_database = [](const json&, const std::string&) {};
    }
    if (!dependencies.validate_api_key) {
        dependencies.validate_api_key = [](const std::string&) { return false; };
    }
    if (!dependencies.steady_now) {
        dependencies.steady_now = [] { return std::chrono::steady_clock::now(); };
    }
    if (!dependencies.system_now) {
        dependencies.system_now = [] { return std::time(nullptr); };
    }
    return dependencies;
}

bool isAuthenticated(const crow::request& req, const AppState& state, const AppDependencies& dependencies) {
    if (!state.auth_enabled) return true;

    std::string provided_key = req.get_header_value("X-API-Key");
    if (provided_key.empty()) return false;
    return dependencies.validate_api_key(provided_key);
}

}  // namespace

AppState::~AppState() {
    if (auth_db_conn) {
        PQfinish(auth_db_conn);
        auth_db_conn = nullptr;
    }
}

void setSecurityHeaders(crow::response& res) {
    res.set_header("Content-Type", "application/json");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Content-Security-Policy", "default-src 'none'");
    res.set_header("Cache-Control", "no-store");
}

crow::response unauthorizedResponse() {
    auto response = crow::response(401, R"({"error":"Unauthorized. Invalid or missing API key."})");
    setSecurityHeaders(response);
    return response;
}

crow::response handleHealth() {
    auto response = crow::response(200, R"({"status":"ok"})");
    response.set_header("Content-Type", "application/json");
    return response;
}

crow::response handleSearchStops(const crow::request& req, AppState& state, const AppDependencies& dependencies) {
    if (!isAuthenticated(req, state, dependencies)) return unauthorizedResponse();

    auto query = req.url_params.get("q");
    auto city = req.url_params.get("city");
    auto location_param = req.url_params.get("location");

    bool include_location =
        location_param && (std::string(location_param) == "true" || std::string(location_param) == "1");

    if (!query) {
        auto response = crow::response(400, R"({"error":"Missing 'q' parameter"})");
        setSecurityHeaders(response);
        return response;
    }

    std::string query_str(query);
    if (!isValidSearchQuery(query_str)) {
        auto response = crow::response(400, R"({"error":"Invalid search query"})");
        setSecurityHeaders(response);
        return response;
    }

    std::string city_str = city ? std::string(city) : "";
    if (!city_str.empty() && !isValidSearchQuery(city_str)) {
        auto response = crow::response(400, R"({"error":"Invalid city parameter"})");
        setSecurityHeaders(response);
        return response;
    }

    json search_result = dependencies.search_stops(query_str, city_str, include_location);
    bool has_error = search_result.is_object() && search_result.contains("error");
    if (!has_error) {
        dependencies.ensure_stops_in_database(search_result, query_str);
    }

    auto response = crow::response(search_result.dump());
    setSecurityHeaders(response);
    return response;
}

crow::response handleDepartures(
    const crow::request& req,
    const std::string& stop_id,
    AppState& state,
    const AppDependencies& dependencies) {
    if (!isAuthenticated(req, state, dependencies)) return unauthorizedResponse();

    if (!isValidStopId(stop_id)) {
        auto response = crow::response(400, R"({"error":"Invalid stop ID"})");
        setSecurityHeaders(response);
        return response;
    }

    const char* detailed_param = req.url_params.get("detailed");
    const char* delay_param = req.url_params.get("delay");

    bool detailed = detailed_param && (std::string(detailed_param) == "true" || std::string(detailed_param) == "1");
    bool include_delay = delay_param && (std::string(delay_param) == "true" || std::string(delay_param) == "1");

    json all_departures;
    bool cache_hit = false;
    std::string cache_key = buildCacheKey(stop_id, detailed, include_delay);

    {
        std::lock_guard<std::mutex> lock(state.cache_mutex);
        auto it = state.stop_cache.find(cache_key);
        if (it != state.stop_cache.end()) {
            auto now = dependencies.steady_now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count() <
                kCacheTtlSeconds) {
                all_departures = it->second.data;
                cache_hit = true;
            }
        }
    }

    if (!cache_hit) {
        json raw_data = dependencies.fetch_departures(stop_id);
        if (raw_data.contains("error")) {
            auto response = crow::response(502, raw_data.dump());
            setSecurityHeaders(response);
            return response;
        }

        all_departures = normalizeResponse(raw_data, detailed, include_delay);

        std::lock_guard<std::mutex> lock(state.cache_mutex);
        auto now = dependencies.steady_now();
        evictExpiredCacheEntries(state.stop_cache, now);
        if (state.stop_cache.size() < kMaxCacheEntries) {
            state.stop_cache[cache_key] = {all_departures, now};
        }
    }

    const char* requested_track = req.url_params.get("track");
    if (requested_track) {
        auto response = crow::response(filterDeparturesByTrack(all_departures, requested_track).dump());
        setSecurityHeaders(response);
        return response;
    }

    auto response = crow::response(all_departures.dump());
    setSecurityHeaders(response);
    return response;
}

crow::response handleCurrentNotifications(
    const crow::request& req,
    AppState& state,
    const AppDependencies& dependencies) {
    if (!isAuthenticated(req, state, dependencies)) return unauthorizedResponse();

    auto stop_id_param = req.url_params.get("stopID");
    if (!stop_id_param) {
        auto response = crow::response(400, R"({"error":"Missing 'stopID' parameter"})");
        setSecurityHeaders(response);
        return response;
    }

    std::string stop_id(stop_id_param);
    if (!isValidStopId(stop_id)) {
        auto response = crow::response(400, R"({"error":"Invalid stop ID"})");
        setSecurityHeaders(response);
        return response;
    }

    std::vector<json> responses;
    responses.reserve(kNotificationApiProviders.size());
    for (const auto& provider_url : kNotificationApiProviders) {
        responses.push_back(dependencies.fetch_notifications_from_provider(provider_url, stop_id));
    }

    auto response = crow::response(extractNotificationsFromResponses(stop_id, responses).dump());
    setSecurityHeaders(response);
    return response;
}

std::unique_ptr<TransitApp> createApp(const std::shared_ptr<AppState>& state, AppDependencies dependencies) {
    dependencies = withDefaults(std::move(dependencies));
    auto app = std::make_unique<TransitApp>();

    CROW_ROUTE((*app), "/health")
    ([] {
        return handleHealth();
    });

    CROW_ROUTE((*app), "/api/stops/search")
    ([state, dependencies](const crow::request& req) {
        return handleSearchStops(req, *state, dependencies);
    });

    CROW_ROUTE((*app), "/api/stops/<string>")
    ([state, dependencies](const crow::request& req, std::string stop_id) {
        return handleDepartures(req, stop_id, *state, dependencies);
    });

    CROW_ROUTE((*app), "/api/current_notifs")
    ([state, dependencies](const crow::request& req) {
        return handleCurrentNotifications(req, *state, dependencies);
    });

    app->validate();
    return app;
}

}  // namespace transit_tracker
