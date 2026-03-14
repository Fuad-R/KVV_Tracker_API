#include "config/config.h"
#include "db/database.h"
#include "middleware/api_key_auth.h"
#include "routes/auth_routes.h"
#include "routes/stops_routes.h"
#include "routes/departures_routes.h"
#include "routes/notifications_routes.h"

int main() {
    crow::SimpleApp app;

    // Initialize authentication
    initAuth();

    // Initialize database (try environment variables first, then config files)
    Database db;
    if (!db.loadConfigFromEnv()) {
        if (!db.loadConfig(DB_CONFIG_PATH)) {
            db.loadConfig(DB_CONFIG_CONTAINER_PATH);
        }
    }
    if (!db.hasConfig()) {
        std::cerr << "Database config unavailable. Stop persistence disabled." << std::endl;
    }

    // Determine server port (default: 8080)
    int port = 8080;
    const char* portEnv = std::getenv("APP_PORT");
    if (portEnv && portEnv[0] != '\0') {
        try {
            port = std::stoi(std::string(portEnv));
        } catch (...) {
            std::cerr << "Invalid APP_PORT value, using default 8080." << std::endl;
        }
    }

    // Register routes
    registerAuthRoutes(app, db);
    registerStopsRoutes(app, db);
    registerDeparturesRoutes(app, db);
    registerNotificationsRoutes(app, db);

    app.port(port).multithreaded().run();
}
