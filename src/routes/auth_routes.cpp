#include "auth_routes.h"

void registerAuthRoutes(crow::SimpleApp& app, Database& /*db*/) {
    // --- Route: Health Check (no authentication required) ---
    CROW_ROUTE(app, "/health")
    ([](const crow::request& /*req*/){
        auto response = crow::response(200, R"({"status":"ok"})");
        response.set_header("Content-Type", "application/json");
        return response;
    });
}
