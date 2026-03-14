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

    // Initialize database
    Database db;
    if (!db.loadConfig(DB_CONFIG_PATH)) {
        db.loadConfig(DB_CONFIG_CONTAINER_PATH);
    }
    if (!db.hasConfig()) {
        std::cerr << "Database config unavailable. Stop persistence disabled." << std::endl;
    }

    // Register routes
    registerAuthRoutes(app, db);
    registerStopsRoutes(app, db);
    registerDeparturesRoutes(app, db);
    registerNotificationsRoutes(app, db);

    app.port(8080).multithreaded().run();
}
