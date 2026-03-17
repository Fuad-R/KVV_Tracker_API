#include "stops_routes.h"
#include "../middleware/api_key_auth.h"
#include "../services/stops_service.h"
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace {
std::optional<double> parseDoubleParam(const char* value) {
    if (!value) return std::nullopt;
    try {
        std::string raw(value);
        size_t parsed = 0;
        double number = std::stod(raw, &parsed);
        if (parsed != raw.size() || !std::isfinite(number)) return std::nullopt;
        return number;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> parseIntParam(const char* value) {
    if (!value) return std::nullopt;
    try {
        std::string raw(value);
        size_t parsed = 0;
        long number = std::stol(raw, &parsed);
        if (parsed != raw.size() || number > std::numeric_limits<int>::max() || number < std::numeric_limits<int>::min()) {
            return std::nullopt;
        }
        return static_cast<int>(number);
    } catch (...) {
        return std::nullopt;
    }
}
}

void registerStopsRoutes(crow::SimpleApp& app, Database& db) {
    // --- Route: Search Stops ---
    CROW_ROUTE(app, "/api/stops/search")
    ([&db](const crow::request& req){
        if (!isAuthenticated(req, db)) return unauthorizedResponse();
        auto query = req.url_params.get("q");
        auto city = req.url_params.get("city");
        auto locationParam = req.url_params.get("location");

        bool includeLocation = (locationParam && (std::string(locationParam) == "true" || std::string(locationParam) == "1"));

        if (!query) {
            auto response = crow::response(400, R"({"error":"Missing 'q' parameter"})");
            setSecurityHeaders(response);
            return response;
        }

        std::string queryStr(query);
        if (!isValidSearchQuery(queryStr)) {
            auto response = crow::response(400, R"({"error":"Invalid search query"})");
            setSecurityHeaders(response);
            return response;
        }

        std::string cityStr = city ? std::string(city) : "";
        if (!cityStr.empty() && !isValidSearchQuery(cityStr)) {
            auto response = crow::response(400, R"({"error":"Invalid city parameter"})");
            setSecurityHeaders(response);
            return response;
        }

        json searchResult = searchStopsProvider(queryStr, cityStr, includeLocation);
        bool hasError = searchResult.is_object() && searchResult.contains("error");
        if (!hasError) {
            ensureStopsInDatabase(searchResult, queryStr, db);
        }
        auto response = crow::response(searchResult.dump());
        setSecurityHeaders(response);
        return response;

    });

    // --- Route: Nearby Stops ---
    CROW_ROUTE(app, "/api/stops/nearby")
    ([&db](const crow::request& req){
        if (!isAuthenticated(req, db)) return unauthorizedResponse();

        const char* latParam = req.url_params.get("lat");
        const char* longParam = req.url_params.get("long");

        if (!latParam || !longParam) {
            auto response = crow::response(400, R"({"error":"Missing required 'lat' and/or 'long' parameter"})");
            setSecurityHeaders(response);
            return response;
        }

        auto latitude = parseDoubleParam(latParam);
        auto longitude = parseDoubleParam(longParam);

        if (!latitude || !longitude || *latitude < -90.0 || *latitude > 90.0 || *longitude < -180.0 || *longitude > 180.0) {
            auto response = crow::response(400, R"({"error":"Invalid 'lat' or 'long' parameter"})");
            setSecurityHeaders(response);
            return response;
        }

        int distance = 1000;
        if (const char* distanceParam = req.url_params.get("distance")) {
            auto parsedDistance = parseIntParam(distanceParam);
            if (!parsedDistance || *parsedDistance <= 0 || *parsedDistance > 50000) {
                auto response = crow::response(400, R"({"error":"Invalid 'distance' parameter"})");
                setSecurityHeaders(response);
                return response;
            }
            distance = *parsedDistance;
        }

        int limit = 20;
        if (const char* limitParam = req.url_params.get("limit")) {
            auto parsedLimit = parseIntParam(limitParam);
            if (!parsedLimit || *parsedLimit <= 0 || *parsedLimit > 100) {
                auto response = crow::response(400, R"({"error":"Invalid 'limit' parameter"})");
                setSecurityHeaders(response);
                return response;
            }
            limit = *parsedLimit;
        }

        json result = getNearbyStops(*latitude, *longitude, distance, limit, db);
        if (result.is_object() && result.contains("error")) {
            int status = result.value("error", "") == "Database not configured" ? 503 : 500;
            auto response = crow::response(status, result.dump());
            setSecurityHeaders(response);
            return response;
        }

        auto response = crow::response(result.dump());
        setSecurityHeaders(response);
        return response;
    });
}
