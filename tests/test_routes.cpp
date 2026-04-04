#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "transit_tracker/app.hpp"

namespace {

crow::request makeGetRequest(
    const std::string& raw_url,
    const std::vector<std::pair<std::string, std::string>>& headers = {}) {
    crow::request req;
    req.method = crow::HTTPMethod::Get;
    req.raw_url = raw_url;
    auto query_pos = raw_url.find('?');
    req.url = query_pos == std::string::npos ? raw_url : raw_url.substr(0, query_pos);
    req.url_params = crow::query_string(raw_url);
    req.http_ver_major = 1;
    req.http_ver_minor = 1;
    req.keep_alive = false;
    req.close_connection = false;
    req.upgrade = false;
    for (const auto& [key, value] : headers) {
        req.add_header(key, value);
    }
    return req;
}

crow::response perform(transit_tracker::TransitApp& app, crow::request req) {
    crow::response res;
    app.handle_full(req, res);
    return res;
}

transit_tracker::AppDependencies makeDependencies(
    int& departures_calls,
    int& search_calls,
    int& persist_calls,
    int& notification_calls,
    std::shared_ptr<std::chrono::steady_clock::time_point> now,
    bool auth_result = true) {
    using transit_tracker::json;

    transit_tracker::AppDependencies deps;
    deps.search_stops = [&search_calls](const std::string& query, const std::string& city, bool include_location) {
        ++search_calls;
        return json{
            {"echo", {
                {"query", query},
                {"city", city},
                {"location", include_location}
            }}
        };
    };
    deps.fetch_departures = [&departures_calls](const std::string& stop_id) {
        ++departures_calls;
        if (stop_id == "bad-upstream") {
            return json{{"error", "Upstream Provider error"}};
        }
        return json{
            {"departureList", json::array({
                {
                    {"servingLine", {
                        {"number", "2"},
                        {"direction", "Wolfartsweier"},
                        {"motType", "4"}
                    }},
                    {"platform", "1"},
                    {"countdown", "0"},
                    {"realDateTime", {{"year", 2026}}}
                },
                {
                    {"servingLine", {
                        {"number", "4"},
                        {"direction", "Oberreut"},
                        {"motType", "4"}
                    }},
                    {"platform", "10"},
                    {"countdown", "3"}
                }
            })}
        };
    };
    deps.fetch_notifications_from_provider = [&notification_calls](const std::string& provider, const std::string& stop_id) {
        ++notification_calls;
        return json{
            {"infos", {
                {"current", json::array({
                    {
                        {"id", provider.find("efa-bw") != std::string::npos ? "dup" : "dup"},
                        {"priority", "normal"},
                        {"affected", {
                            {"stops", json::array({
                                {{"properties", {{"stopId", stop_id}}}}
                            })}
                        }},
                        {"properties", {{"providerCode", provider.find("efa-bw") != std::string::npos ? "BW" : "VRR"}}},
                        {"infoLinks", json::array({
                            {{"urlText", "Notice"}, {"content", ""}, {"subtitle", "Heads up"}}
                        })}
                    }
                })}
            }}
        };
    };
    deps.ensure_stops_in_database = [&persist_calls](const transit_tracker::json&, const std::string&) {
        ++persist_calls;
    };
    deps.validate_api_key = [auth_result](const std::string& provided_key) {
        return auth_result && provided_key == "good-key";
    };
    deps.steady_now = [now] { return *now; };
    deps.system_now = [] { return std::time(nullptr); };
    return deps;
}

void checkSecurityHeaders(crow::response& response) {
    CHECK(response.get_header_value("Content-Type") == "application/json");
    CHECK(response.get_header_value("X-Content-Type-Options") == "nosniff");
    CHECK(response.get_header_value("X-Frame-Options") == "DENY");
    CHECK(response.get_header_value("Content-Security-Policy") == "default-src 'none'");
    CHECK(response.get_header_value("Cache-Control") == "no-store");
}

}  // namespace

TEST_CASE("health route stays public and returns JSON") {
    auto state = std::make_shared<transit_tracker::AppState>();
    int departures_calls = 0;
    int search_calls = 0;
    int persist_calls = 0;
    int notification_calls = 0;
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto app = transit_tracker::createApp(
        state,
        makeDependencies(departures_calls, search_calls, persist_calls, notification_calls, now));

    auto response = perform(*app, makeGetRequest("/health"));
    CHECK(response.code == 200);
    CHECK(response.body == R"({"status":"ok"})");
    CHECK(response.get_header_value("Content-Type") == "application/json");
}

TEST_CASE("search route enforces auth validates params and persists successful results") {
    auto state = std::make_shared<transit_tracker::AppState>();
    state->auth_enabled = true;

    int departures_calls = 0;
    int search_calls = 0;
    int persist_calls = 0;
    int notification_calls = 0;
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto app = transit_tracker::createApp(
        state,
        makeDependencies(departures_calls, search_calls, persist_calls, notification_calls, now));

    auto unauthorized = perform(*app, makeGetRequest("/api/stops/search?q=Marktplatz"));
    CHECK(unauthorized.code == 401);
    checkSecurityHeaders(unauthorized);

    auto missing_q = perform(*app, makeGetRequest("/api/stops/search", {{"X-API-Key", "good-key"}}));
    CHECK(missing_q.code == 400);
    CHECK(missing_q.body == R"({"error":"Missing 'q' parameter"})");

    auto invalid_city = perform(
        *app,
        makeGetRequest("/api/stops/search?q=Marktplatz&city=bad%0Avalue", {{"X-API-Key", "good-key"}}));
    CHECK(invalid_city.code == 400);

    auto success = perform(
        *app,
        makeGetRequest("/api/stops/search?q=Marktplatz&city=Karlsruhe&location=true", {{"X-API-Key", "good-key"}}));
    CHECK(success.code == 200);
    checkSecurityHeaders(success);
    auto body = transit_tracker::json::parse(success.body);
    CHECK(body["echo"]["query"] == "Marktplatz");
    CHECK(body["echo"]["city"] == "Karlsruhe");
    CHECK(body["echo"]["location"] == true);
    CHECK(search_calls == 1);
    CHECK(persist_calls == 1);
}

TEST_CASE("departures route normalizes responses caches results and filters tracks") {
    auto state = std::make_shared<transit_tracker::AppState>();

    int departures_calls = 0;
    int search_calls = 0;
    int persist_calls = 0;
    int notification_calls = 0;
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto app = transit_tracker::createApp(
        state,
        makeDependencies(departures_calls, search_calls, persist_calls, notification_calls, now));

    auto first = perform(*app, makeGetRequest("/api/stops/7000107?track=1"));
    CHECK(first.code == 200);
    checkSecurityHeaders(first);
    auto first_body = transit_tracker::json::parse(first.body);
    REQUIRE(first_body.size() == 1);
    CHECK(first_body[0]["platform"] == "1");
    CHECK(departures_calls == 1);

    auto second = perform(*app, makeGetRequest("/api/stops/7000107?track=1"));
    CHECK(second.code == 200);
    CHECK(departures_calls == 1);

    auto detailed = perform(*app, makeGetRequest("/api/stops/7000107?detailed=true&delay=true"));
    CHECK(detailed.code == 200);
    CHECK(departures_calls == 2);

    *now += std::chrono::seconds(transit_tracker::kCacheTtlSeconds + 1);
    auto after_ttl = perform(*app, makeGetRequest("/api/stops/7000107"));
    CHECK(after_ttl.code == 200);
    CHECK(departures_calls == 3);

    auto invalid_stop = perform(*app, makeGetRequest("/api/stops/bad$stop"));
    CHECK(invalid_stop.code == 400);
    CHECK(invalid_stop.body == R"({"error":"Invalid stop ID"})");
}

TEST_CASE("departures route returns 502 for upstream errors and notifications dedupe providers") {
    auto state = std::make_shared<transit_tracker::AppState>();

    int departures_calls = 0;
    int search_calls = 0;
    int persist_calls = 0;
    int notification_calls = 0;
    auto now = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto app = transit_tracker::createApp(
        state,
        makeDependencies(departures_calls, search_calls, persist_calls, notification_calls, now));

    auto upstream_error = perform(*app, makeGetRequest("/api/stops/bad-upstream"));
    CHECK(upstream_error.code == 502);
    checkSecurityHeaders(upstream_error);

    auto missing_stop = perform(*app, makeGetRequest("/api/current_notifs"));
    CHECK(missing_stop.code == 400);
    CHECK(missing_stop.body == R"({"error":"Missing 'stopID' parameter"})");

    auto notifications = perform(*app, makeGetRequest("/api/current_notifs?stopID=7000107"));
    CHECK(notifications.code == 200);
    checkSecurityHeaders(notifications);
    auto body = transit_tracker::json::parse(notifications.body);
    REQUIRE(body.size() == 1);
    CHECK(body[0]["id"] == "dup");
    CHECK(notification_calls == 2);
}
