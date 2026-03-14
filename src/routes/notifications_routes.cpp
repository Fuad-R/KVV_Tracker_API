#include "notifications_routes.h"
#include "../middleware/api_key_auth.h"
#include "../services/notifications_service.h"

void registerNotificationsRoutes(crow::SimpleApp& app, Database& db) {
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
