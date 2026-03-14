#include "config/config.h"
#include "db/database.h"
#include "middleware/api_key_auth.h"
#include "routes/auth_routes.h"
#include "routes/stops_routes.h"
#include "routes/departures_routes.h"
#include "routes/notifications_routes.h"

#include <algorithm>
#include <cctype>
#include <string>

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
        std::string portStr(portEnv);

        // Ensure APP_PORT consists only of digits to avoid partial parsing (e.g., "8080abc")
        bool allDigits = !portStr.empty() &&
            std::all_of(portStr.begin(), portStr.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });

        if (allDigits) {
            try {
                int parsedPort = std::stoi(portStr);
                if (parsedPort >= 1 && parsedPort <= 65535) {
                    port = parsedPort;
                } else {
                    std::cerr << "APP_PORT out of range (1-65535), using default 8080." << std::endl;
                }
            } catch (const std::exception&) {
                std::cerr << "Invalid APP_PORT value, using default 8080." << std::endl;
            }
        } else {
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
