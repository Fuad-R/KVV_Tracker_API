#include "departures_routes.h"
#include "../middleware/api_key_auth.h"
#include "../services/departures_service.h"

void registerDeparturesRoutes(crow::SimpleApp& app, Database& db) {
    // --- Route: Departures ---
    CROW_ROUTE(app, "/api/stops/<string>")
    ([&db](const crow::request& req, std::string stopId){
        if (!isAuthenticated(req, db)) return unauthorizedResponse();
        if (!isValidStopId(stopId)) {
            auto response = crow::response(400, R"({"error":"Invalid stop ID"})");
            setSecurityHeaders(response);
            return response;
        }

        const char* detailedParam = req.url_params.get("detailed");
        const char* delayParam = req.url_params.get("delay");

        bool detailed = (detailedParam && (std::string(detailedParam) == "true" || std::string(detailedParam) == "1"));
        bool includeDelay = (delayParam && (std::string(delayParam) == "true" || std::string(delayParam) == "1"));

        const char* requestedTrack = req.url_params.get("track");

        json result = getDepartures(stopId, detailed, includeDelay, requestedTrack);

        if (result.is_object() && result.contains("error")) {
            auto response = crow::response(502, result.dump());
            setSecurityHeaders(response);
            return response;
        }

        auto response = crow::response(result.dump());
        setSecurityHeaders(response);
        return response;
    });

    // --- Route: Current Notifications ---
    CROW_ROUTE(app, "/api/current_notifs")
    ([&db](const crow::request& req){
        if (!isAuthenticated(req, db)) return unauthorizedResponse();
        auto stopIdParam = req.url_params.get("stopID");

        if (!stopIdParam) {
            auto response = crow::response(400, R"({"error":"Missing 'stopID' parameter"})");
            setSecurityHeaders(response);
            return response;
        }

        std::string stopId = std::string(stopIdParam);
        if (!isValidStopId(stopId)) {
            auto response = crow::response(400, R"({"error":"Invalid stop ID"})");
            setSecurityHeaders(response);
            return response;
        }

        json notifications = extractValidNotifications(stopId);

        auto response = crow::response(notifications.dump());
        setSecurityHeaders(response);
        return response;
    });
}
