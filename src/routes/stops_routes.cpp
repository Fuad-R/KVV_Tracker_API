#include "stops_routes.h"
#include "../middleware/api_key_auth.h"
#include "../services/stops_service.h"

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
}
