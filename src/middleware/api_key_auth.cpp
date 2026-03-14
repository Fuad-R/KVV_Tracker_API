#include "api_key_auth.h"
#include "../services/auth_service.h"
#include <cstdlib>
#include <algorithm>
#include <iostream>

// These are initialized once in main() before app.run(), so they are
// effectively immutable during request handling and safe to read concurrently.
static bool auth_enabled = false;
static std::string api_key;

void initAuth() {
    const char* authEnv = std::getenv("AUTH");
    if (authEnv) {
        std::string authVal(authEnv);
        std::transform(authVal.begin(), authVal.end(), authVal.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (authVal == "true" || authVal == "1" || authVal == "yes") {
            auth_enabled = true;
            const char* keyEnv = std::getenv("API_KEY");
            if (keyEnv && std::string(keyEnv).length() > 0) {
                api_key = std::string(keyEnv);
                std::cout << "API key authentication enabled (env-var fallback available)." << std::endl;
            } else {
                std::cout << "API key authentication enabled (database mode)." << std::endl;
            }
        }
    }
}

bool isAuthenticated(const crow::request& req, const Database& db) {
    if (!auth_enabled) return true;
    std::string providedKey = req.get_header_value("X-API-Key");
    if (providedKey.empty()) return false;

    // Try database validation first when DB is configured
    if (db.hasConfig()) {
        return validateKeyViaDatabase(providedKey, db);
    }

    // Fall back to env-var comparison if no database is configured
    if (!api_key.empty()) {
        return constantTimeEquals(providedKey, api_key);
    }

    return false;
}

crow::response unauthorizedResponse() {
    auto response = crow::response(401, R"({"error":"Unauthorized. Invalid or missing API key."})");
    setSecurityHeaders(response);
    return response;
}

void setSecurityHeaders(crow::response& res) {
    res.set_header("Content-Type", "application/json");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Content-Security-Policy", "default-src 'none'");
    res.set_header("Cache-Control", "no-store");
}
