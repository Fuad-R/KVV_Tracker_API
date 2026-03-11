#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <iomanip>
#include <set>
#include <ctime>
#include <regex>
#include <libpq-fe.h>
#include <cstdlib>
#include <openssl/evp.h>

using json = nlohmann::json;

// --- Cache Structure ---
struct CacheEntry {
    json data;
    std::chrono::steady_clock::time_point timestamp;
};

std::mutex cache_mutex;
std::map<std::string, CacheEntry> stop_cache;
const int CACHE_TTL_SECONDS = 30;
const size_t MAX_CACHE_ENTRIES = 10000;

// --- Configuration ---
const std::string Provider_DM_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
const std::string Provider_SEARCH_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";
const std::string DB_CONFIG_PATH = "db_connection.txt";

// --- Notification API Providers (extensible) ---
const std::vector<std::string> NOTIFICATION_API_PROVIDERS = {
    "https://www.efa-bw.de/nvbw/",
    "https://efa.vrr.de/standard/"
};
const std::string DB_CONFIG_CONTAINER_PATH = "/config/db_connection.txt";
const long UPSTREAM_TIMEOUT_SECONDS = 15;
const size_t MAX_QUERY_LENGTH = 200;
const size_t MAX_STOPID_LENGTH = 100;

// --- Input Validation ---
bool isValidStopId(const std::string& stopId) {
    if (stopId.empty() || stopId.size() > MAX_STOPID_LENGTH) return false;
    static const std::regex stopIdPattern("^[a-zA-Z0-9:_. -]+$");
    return std::regex_match(stopId, stopIdPattern);
}

bool isValidSearchQuery(const std::string& query) {
    if (query.empty() || query.size() > MAX_QUERY_LENGTH) return false;
    for (unsigned char c : query) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// --- Cache Eviction ---
void evictExpiredCacheEntries() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = stop_cache.begin(); it != stop_cache.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count() >= CACHE_TTL_SECONDS) {
            it = stop_cache.erase(it);
        } else {
            ++it;
        }
    }
}

// --- Security: Set common security headers on a response ---
void setSecurityHeaders(crow::response& res) {
    res.set_header("Content-Type", "application/json");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Content-Security-Policy", "default-src 'none'");
    res.set_header("Cache-Control", "no-store");
}

// --- Authentication ---
// These are initialized once in main() before app.run(), so they are
// effectively immutable during request handling and safe to read concurrently.
bool auth_enabled = false;
std::string api_key;

// Persistent auth DB connection and throttle state (protected by auth_db_mutex)
std::mutex auth_db_mutex;
PGconn* auth_db_conn = nullptr;
std::map<std::string, std::chrono::steady_clock::time_point> last_used_updates;
constexpr int LAST_USED_UPDATE_INTERVAL_SECONDS = 300; // 5 minutes

std::string sha256Hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1
           && EVP_DigestUpdate(ctx, input.c_str(), input.size()) == 1
           && EVP_DigestFinal_ex(ctx, hash, &hashLen) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || hashLen == 0) return "";
    std::ostringstream ss;
    for (unsigned int i = 0; i < hashLen; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
}

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

// Constant-time string comparison to prevent timing attacks
bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return result == 0;
}

crow::response unauthorizedResponse() {
    auto response = crow::response(401, R"({"error":"Unauthorized. Invalid or missing API key."})");
    setSecurityHeaders(response);
    return response;
}

struct DbConfig {
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
    std::string sslmode;
};

std::optional<DbConfig> db_config;

// --- Helper: String Utilities ---
std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string trim(const std::string& input) {
    auto start = std::find_if_not(input.begin(), input.end(),
                                  [](unsigned char c){ return std::isspace(c); });
    auto end = std::find_if_not(input.rbegin(), input.rend(),
                                [](unsigned char c){ return std::isspace(c); }).base();
    if (start >= end) return "";
    return std::string(start, end);
}

// --- Helper: Database Configuration ---

std::optional<DbConfig> loadDbConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << "Database config not found: " << path << std::endl;
        return std::nullopt;
    }

    DbConfig config;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = toLower(trim(line.substr(0, pos)));
        std::string value = trim(line.substr(pos + 1));
        if (key == "host") config.host = value;
        else if (key == "port") config.port = value;
        else if (key == "dbname") config.dbname = value;
        else if (key == "user") config.user = value;
        else if (key == "password") config.password = value;
        else if (key == "sslmode") config.sslmode = value;
    }

    if (config.host.empty() || config.port.empty() || config.dbname.empty() ||
        config.user.empty() || config.password.empty()) {
        std::cerr << "Database config missing required fields in: " << path << std::endl;
        return std::nullopt;
    }

    if (config.sslmode.empty()) {
        config.sslmode = "require";
    }

    return config;
}

PGconn* connectToDatabase(const DbConfig& config) {
    const char* keywords[] = {"host", "port", "dbname", "user", "password", "sslmode", nullptr};
    const char* values[] = {
        config.host.c_str(),
        config.port.c_str(),
        config.dbname.c_str(),
        config.user.c_str(),
        config.password.c_str(),
        config.sslmode.empty() ? nullptr : config.sslmode.c_str(),
        nullptr
    };
    return PQconnectdbParams(keywords, values, 0);
}

// --- Authentication: Persistent auth DB connection management ---

PGconn* getAuthDbConnection() {
    // Must be called with auth_db_mutex held
    if (auth_db_conn && PQstatus(auth_db_conn) == CONNECTION_OK) {
        return auth_db_conn;
    }
    // Clean up stale connection
    if (auth_db_conn) {
        PQfinish(auth_db_conn);
        auth_db_conn = nullptr;
    }
    if (!db_config) return nullptr;
    auth_db_conn = connectToDatabase(*db_config);
    if (!auth_db_conn) return nullptr;
    if (PQstatus(auth_db_conn) != CONNECTION_OK) {
        std::cerr << "Auth DB connection failed: " << PQerrorMessage(auth_db_conn) << std::endl;
        PQfinish(auth_db_conn);
        auth_db_conn = nullptr;
        return nullptr;
    }
    return auth_db_conn;
}

// --- Authentication: Database-backed API key validation ---

bool validateKeyViaDatabase(const std::string& providedKey) {
    if (!db_config) return false;

    std::string keyHash = sha256Hex(providedKey);
    if (keyHash.empty()) return false;

    std::lock_guard<std::mutex> lock(auth_db_mutex);

    // Purge stale entries from the throttle map to prevent unbounded growth
    auto now = std::chrono::steady_clock::now();
    for (auto it = last_used_updates.begin(); it != last_used_updates.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= LAST_USED_UPDATE_INTERVAL_SECONDS) {
            it = last_used_updates.erase(it);
        } else {
            ++it;
        }
    }

    PGconn* conn = getAuthDbConnection();
    if (!conn) return false;

    const char* querySql =
        "SELECT id FROM api_keys "
        "WHERE key_hash = $1 AND revoked = FALSE "
        "AND (expires_at IS NULL OR expires_at > NOW())";
    const char* queryValues[1] = { keyHash.c_str() };

    PGresult* res = PQexecParams(conn, querySql, 1, nullptr, queryValues, nullptr, nullptr, 0);
    if (!res) {
        std::cerr << "Auth query returned null result" << std::endl;
        return false;
    }
    bool valid = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);

    if (valid) {
        std::string keyId = PQgetvalue(res, 0, 0);
        PQclear(res);

        // Throttle last_used_at updates: only once per LAST_USED_UPDATE_INTERVAL_SECONDS per key
        auto now = std::chrono::steady_clock::now();
        auto it = last_used_updates.find(keyId);
        bool shouldUpdate = (it == last_used_updates.end()) ||
            (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >= LAST_USED_UPDATE_INTERVAL_SECONDS);

        if (shouldUpdate) {
            const char* updateSql = "UPDATE api_keys SET last_used_at = NOW() WHERE id = $1";
            const char* updateValues[1] = { keyId.c_str() };
            PGresult* updateRes = PQexecParams(conn, updateSql, 1, nullptr, updateValues, nullptr, nullptr, 0);
            if (!updateRes || PQresultStatus(updateRes) != PGRES_COMMAND_OK) {
                std::cerr << "Failed to update last_used_at: " << PQerrorMessage(conn) << std::endl;
            } else {
                last_used_updates[keyId] = now;
            }
            if (updateRes) PQclear(updateRes);
        }
    } else {
        PQclear(res);
    }

    return valid;
}

bool isAuthenticated(const crow::request& req) {
    if (!auth_enabled) return true;
    std::string providedKey = req.get_header_value("X-API-Key");
    if (providedKey.empty()) return false;

    // Try database validation first when DB is configured
    if (db_config) {
        return validateKeyViaDatabase(providedKey);
    }

    // Fall back to env-var comparison if no database is configured
    if (!api_key.empty()) {
        return constantTimeEquals(providedKey, api_key);
    }

    return false;
}

// --- Helper: JSON Conversion Utilities ---

std::optional<std::string> jsonToString(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_float()) {
        std::ostringstream stream;
        stream << std::setprecision(15) << value.get<double>();
        return stream.str();
    }
    return std::nullopt;
}

std::optional<double> jsonToDouble(const json& value) {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) {
        try {
            return std::stod(value.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> getJsonString(const json& obj, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        if (obj.contains(key)) {
            auto value = jsonToString(obj.at(key));
            if (value && !value->empty()) return value;
        }
    }
    return std::nullopt;
}

std::string formatDouble(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(8) << value;
    return stream.str();
}

// --- Helper: MOT (Mode of Transport) Parsing ---

std::optional<std::string> buildMotArray(const json& stop) {
    std::vector<int> motValues;

    auto addValue = [&](const json& value) {
        if (value.is_number_integer()) {
            motValues.push_back(value.get<int>());
        } else if (value.is_string()) {
            try {
                motValues.push_back(std::stoi(value.get<std::string>()));
            } catch (...) {
                return;
            }
        } else if (value.is_object()) {
            auto nested = getJsonString(value, {"motType", "type", "mode"});
            if (nested) {
                try {
                    motValues.push_back(std::stoi(*nested));
                } catch (...) {
                    return;
                }
            }
        }
    };

    if (stop.contains("modes")) {
        const auto& modes = stop.at("modes");
        if (modes.is_array()) {
            for (const auto& item : modes) addValue(item);
        } else {
            addValue(modes);
        }
    } else if (stop.contains("mot")) {
        const auto& mot = stop.at("mot");
        if (mot.is_array()) {
            for (const auto& item : mot) addValue(item);
        } else {
            addValue(mot);
        }
    } else if (stop.contains("productClasses")) {
        const auto& classes = stop.at("productClasses");
        if (classes.is_array()) {
            for (const auto& item : classes) addValue(item);
        } else {
            addValue(classes);
        }
    } else if (stop.contains("motType")) {
        addValue(stop.at("motType"));
    }

    if (motValues.empty()) return std::nullopt;

    std::ostringstream stream;
    stream << "{";
    for (size_t i = 0; i < motValues.size(); ++i) {
        if (i > 0) stream << ",";
        stream << motValues[i];
    }
    stream << "}";
    return stream.str();
}

// --- Helper: Coordinate Extraction ---

bool extractCoordinates(const json& stop, double& lat, double& lon) {
    auto extractFromObject = [&](const json& coord) -> bool {
        if (coord.contains("x") && coord.contains("y")) {
            // Stopfinder returns coordinates where x=latitude and y=longitude.
            auto latValue = jsonToDouble(coord.at("x"));
            auto lonValue = jsonToDouble(coord.at("y"));
            if (latValue && lonValue) {
                lat = *latValue;
                lon = *lonValue;
                return true;
            }
        }
        if (coord.contains("lon") && coord.contains("lat")) {
            auto x = jsonToDouble(coord.at("lon"));
            auto y = jsonToDouble(coord.at("lat"));
            if (x && y) {
                lon = *x;
                lat = *y;
                return true;
            }
        }
        if (coord.contains("longitude") && coord.contains("latitude")) {
            auto x = jsonToDouble(coord.at("longitude"));
            auto y = jsonToDouble(coord.at("latitude"));
            if (x && y) {
                lon = *x;
                lat = *y;
                return true;
            }
        }
        return false;
    };

    if (stop.contains("coord")) {
        const auto& coord = stop.at("coord");
        if (coord.is_object() && extractFromObject(coord)) return true;
        if (coord.is_array() && coord.size() >= 2) {
            // Stopfinder arrays use [latitude, longitude] ordering.
            auto latValue = jsonToDouble(coord.at(0));
            auto lonValue = jsonToDouble(coord.at(1));
            if (latValue && lonValue) {
                lat = *latValue;
                lon = *lonValue;
                return true;
            }
        }
    }

    if (extractFromObject(stop)) return true;

    if (stop.contains("latitude") && stop.contains("longitude")) {
        auto y = jsonToDouble(stop.at("latitude"));
        auto x = jsonToDouble(stop.at("longitude"));
        if (x && y) {
            lon = *x;
            lat = *y;
            return true;
        }
    }

    return false;
}

// --- Stop Record Parsing ---

struct StopRecord {
    std::string stop_id;
    std::optional<std::string> local_id;
    std::string stop_name;
    std::string city;
    std::optional<std::string> mot_array;
    double latitude;
    double longitude;
};

std::optional<StopRecord> parseStopRecord(const json& stop) {
    auto stopId = getJsonString(stop, {"id", "stopId", "stopID", "gid"});
    auto stopName = getJsonString(stop, {"name", "stopName", "stop_name"});
    if (!stopId || !stopName) return std::nullopt;

    std::optional<std::string> localId;
    if (stop.contains("properties") && stop.at("properties").is_object()) {
        localId = getJsonString(stop.at("properties"),
                                {"stopId", "stopID", "stopid", "localId", "local_id"});
    }
    if (!localId) {
        localId = getJsonString(stop, {"localId", "local_id"});
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (!extractCoordinates(stop, latitude, longitude)) return std::nullopt;

    auto city = getJsonString(stop, {"city", "place", "locality", "town"});
    if (!city && stop.contains("parent") && stop.at("parent").is_object()) {
        city = getJsonString(stop.at("parent"), {"name", "city", "place", "locality", "town"});
    }
    return StopRecord{
        *stopId,
        localId,
        *stopName,
        city.value_or(""),
        buildMotArray(stop),
        latitude,
        longitude
    };
}

std::vector<StopRecord> extractStopRecords(const json& searchResult) {
    const json* stopArray = nullptr;

    if (searchResult.is_array()) {
        stopArray = &searchResult;
    } else if (searchResult.is_object()) {
        if (searchResult.contains("stopFinder")) {
            const auto& finder = searchResult.at("stopFinder");
            if (finder.contains("points") && finder.at("points").is_array()) {
                stopArray = &finder.at("points");
            } else if (finder.contains("locations") && finder.at("locations").is_array()) {
                stopArray = &finder.at("locations");
            } else if (finder.contains("points") && finder.at("points").contains("point") &&
                       finder.at("points").at("point").is_array()) {
                stopArray = &finder.at("points").at("point");
            }
        }

        if (!stopArray && searchResult.contains("locations") && searchResult.at("locations").is_array()) {
            stopArray = &searchResult.at("locations");
        }
        if (!stopArray && searchResult.contains("points") && searchResult.at("points").is_array()) {
            stopArray = &searchResult.at("points");
        }
    }

    std::vector<StopRecord> records;
    if (!stopArray) return records;

    for (const auto& stop : *stopArray) {
        if (!stop.is_object()) continue;
        auto record = parseStopRecord(stop);
        if (record) records.push_back(*record);
    }

    return records;
}

// --- Helper: Database Persistence ---

void ensureStopsInDatabase(const json& searchResult, const std::string& originalSearch) {
    if (!db_config) return;

    auto records = extractStopRecords(searchResult);
    if (records.empty()) return;

    PGconn* conn = connectToDatabase(*db_config);
    if (!conn) {
        std::cerr << "Database connection returned null" << std::endl;
        return;
    }
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "Database connection failed: " << PQerrorMessage(conn) << std::endl;
        PQfinish(conn);
        return;
    }

    const char* insertSql =
        "INSERT INTO stops (stop_id, local_id, stop_name, city, mot, location, original_search) "
        "VALUES ($1, $2, $3, $4, $5, ST_SetSRID(ST_MakePoint($6, $7), 4326)::geography, $8) "
        "ON CONFLICT (stop_id) DO UPDATE SET "
        "local_id = COALESCE(EXCLUDED.local_id, stops.local_id), "
        "stop_name = EXCLUDED.stop_name, "
        "city = COALESCE(EXCLUDED.city, stops.city), "
        "mot = COALESCE(EXCLUDED.mot, stops.mot), "
        "location = EXCLUDED.location, "
        "original_search = COALESCE(EXCLUDED.original_search, stops.original_search), "
        "last_updated = NOW();";

    for (const auto& record : records) {
        std::string longitudeText = formatDouble(record.longitude);
        std::string latitudeText = formatDouble(record.latitude);

        const char* values[8];
        values[0] = record.stop_id.c_str();
        values[1] = record.local_id ? record.local_id->c_str() : nullptr;
        values[2] = record.stop_name.c_str();
        values[3] = record.city.empty() ? nullptr : record.city.c_str();
        values[4] = record.mot_array ? record.mot_array->c_str() : nullptr;
        values[5] = longitudeText.c_str();
        values[6] = latitudeText.c_str();
        values[7] = originalSearch.empty() ? nullptr : originalSearch.c_str();

        PGresult* res = PQexecParams(conn, insertSql, 8, nullptr, values, nullptr, nullptr, 0);
        if (!res) {
            std::cerr << "Failed to execute insert for stop " << record.stop_id
                      << ": PQexecParams returned null" << std::endl;
            continue;
        }
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "Failed to insert stop " << record.stop_id << ": "
                      << PQerrorMessage(conn) << std::endl;
        }
        PQclear(res);
    }

    PQfinish(conn);
}

// --- Helper: Search Stops ---

json searchStopsProvider(const std::string& query, const std::string& /*city*/ = "", bool includeLocation = false) {
    if (query.empty()) return json::array();

    cpr::Parameters params{
            {"outputFormat", "rapidJSON"},
            {"type_sf", "any"},
            {"name_sf", query},
            {"anyObjFilter_sf", "2"},                  // Stops/stations only
            {"coordOutputFormat", "WGS84[dd.ddddd]"}   // Decimal degree coordinates
    };

    cpr::Response r = cpr::Get(
        cpr::Url{Provider_SEARCH_URL},
        params,
        cpr::Timeout{UPSTREAM_TIMEOUT_SECONDS * 1000}
    );

    if (r.status_code != 200) return {{"error", "Upstream Error"}};

    try {
        return json::parse(r.text);
    } catch (...) {
        return {{"error", "Invalid JSON from Provider Search"}};
    }
}

// --- Helper: Fetch Departures ---
json fetchDeparturesProvider(const std::string& stopId) {
    cpr::Response r = cpr::Get(
        cpr::Url{Provider_DM_URL},
        cpr::Parameters{
            {"outputFormat", "JSON"},
            {"depType", "stopEvents"},
            {"mode", "direct"},
            {"type_dm", "stop"},
            {"name_dm", stopId},
            {"useRealtime", "1"},
            {"limit", "40"}
        },
        cpr::Timeout{UPSTREAM_TIMEOUT_SECONDS * 1000}
    );

    if (r.status_code != 200) return {{"error", "Upstream Provider error"}, {"code", r.status_code}};

    try {
        return json::parse(r.text);
    } catch (...) {
        return {{"error", "Invalid JSON from Provider"}};
    }
}

// --- Helper: Normalize Departure Data ---
json normalizeResponse(const json& ProviderData, bool detailed = false, bool includeDelay = false) {
    json result = json::array();
    if (!ProviderData.contains("departureList")) return result;

    auto strToBool = [](const std::string& v) {
        std::string s = v;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return (s == "1" || s == "true" || s == "yes");
    };

    for (const auto& dep : ProviderData["departureList"]) {
        json item;

        // Line info (number, direction, MOT)
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            // Mode of Transport type
            if (dep["servingLine"].contains("motType")) {
                try {
                    item["mot"] = std::stoi(dep["servingLine"].value("motType", "-1"));
                } catch (...) {
                    item["mot"] = -1;
                }
            } else {
                item["mot"] = -1;
            }

            // Delay in minutes
            if (includeDelay && dep["servingLine"].contains("delay")) {
                try {
                    int delayMinutes = std::stoi(dep["servingLine"].value("delay", "0"));
                    item["delay_minutes"] = delayMinutes;
                } catch (...) {
                    item["delay_minutes"] = 0;
                }
            }

            if (detailed) {
                // Accessibility flags from departure attributes
                bool hasPlanLowFloor = false;
                bool hasPlanWheelchair = false;
                bool planLowFloor = false;
                bool planWheelchair = false;

                if (dep.contains("attrs") && dep["attrs"].is_array()) {
                    for (const auto& a : dep["attrs"]) {
                        std::string name = a.value("name", "");
                        std::string value = a.value("value", "");

                        std::string lname = toLower(name);
                        if (lname == "planlowfloorvehicle") {
                            hasPlanLowFloor = true;
                            planLowFloor = strToBool(value);
                        } else if (lname == "planwheelchairaccess") {
                            hasPlanWheelchair = true;
                            planWheelchair = strToBool(value);
                        }
                    }
                }

                bool hintLowFloor = false;
                bool hintWheelchair = false;

                if (dep["servingLine"].contains("hints") && dep["servingLine"]["hints"].is_array()) {
                    for (const auto& h : dep["servingLine"]["hints"]) {
                        std::string txt = h.value("hint", h.value("content", ""));
                        if (txt.find("Niederflur") != std::string::npos ||
                            txt.find("low floor") != std::string::npos ||
                            txt.find("lowFloor") != std::string::npos) {
                            hintLowFloor = true;
                        }
                        if (txt.find("Rollstuhl") != std::string::npos ||
                            txt.find("wheelchair") != std::string::npos ||
                            txt.find("barrierefrei") != std::string::npos ||
                            txt.find("barrier-free") != std::string::npos) {
                            hintWheelchair = true;
                        }
                    }
                }

                bool lowFloor = hasPlanLowFloor ? planLowFloor : hintLowFloor;
                bool wheelchair = hasPlanWheelchair ? planWheelchair : hintWheelchair;

                item["low_floor"] = lowFloor;
                item["wheelchair_accessible"] = wheelchair || lowFloor;

                if (dep["servingLine"].contains("trainType"))
                    item["train_type"] = dep["servingLine"].value("trainType", "");
                if (dep["servingLine"].contains("trainLength"))
                    item["train_length"] = dep["servingLine"].value("trainLength", "");
                else if (dep["servingLine"].contains("trainComposition"))
                    item["train_composition"] = dep["servingLine"].value("trainComposition", "");
            }
        }

        // Platform name
        if (dep.contains("platform")) item["platform"] = dep.value("platform", "");
        else if (dep.contains("platformName")) item["platform"] = dep.value("platformName", "");
        else item["platform"] = "Unknown";

        // Minutes until departure
        if (dep.contains("countdown")) {
            try {
                item["minutes_remaining"] = std::stoi(dep.value("countdown", "0"));
            } catch (...) {
                item["minutes_remaining"] = 0;
            }
        }
        else item["minutes_remaining"] = 0;

        // Whether real-time data is available
        item["is_realtime"] = dep.contains("realDateTime");

        // Additional hints (detailed mode only)
        if (detailed && dep.contains("hints") && dep["hints"].is_array()) {
            json hintsArray = json::array();
            for (const auto& h : dep["hints"]) {
                std::string txt = h.value("hint", h.value("content", ""));
                if (!txt.empty()) hintsArray.push_back(txt);
            }
            if (!hintsArray.empty()) item["hints"] = hintsArray;
        }

        result.push_back(item);
    }

    return result;
}

// --- Helper: Parse ISO 8601 Timestamp ---

std::optional<std::time_t> parseISO8601(const std::string& timestamp) {
    std::tm tm = {};
    std::istringstream ss(timestamp);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return std::nullopt;
    return timegm(&tm); // Interpret as UTC
}

// --- Helper: Validity Time Range Check ---
bool isCurrentlyValid(const json& validity) {
    if (!validity.is_array() || validity.empty()) return false;

    auto now = std::time(nullptr);

    for (const auto& range : validity) {
        if (!range.contains("from") || !range.contains("to")) continue;
        if (!range["from"].is_string() || !range["to"].is_string()) continue;

        auto fromTime = parseISO8601(range["from"].get<std::string>());
        auto toTime = parseISO8601(range["to"].get<std::string>());

        if (!fromTime || !toTime) continue;

        if (now >= *fromTime && now <= *toTime) {
            return true;
        }
    }
    return false;
}

// --- Helper: Stop Notification Matching ---

bool isStopAffected(const json& info, const std::string& stopId) {
    // Match against affected.stops array
    if (info.contains("affected") && info["affected"].is_object()) {
        if (info["affected"].contains("stops") && info["affected"]["stops"].is_array()) {
            for (const auto& stop : info["affected"]["stops"]) {
                if (stop.contains("properties") && stop["properties"].is_object()) {
                    if (stop["properties"].contains("stopId")) {
                        std::string affectedStopId = stop["properties"]["stopId"].get<std::string>();
                        if (affectedStopId == stopId) return true;
                    }
                }
                // Match by global ID (e.g., "de:08212:107")
                if (stop.contains("id") && stop["id"].is_string()) {
                    std::string affectedId = stop["id"].get<std::string>();
                    if (affectedId == stopId) return true;
                }
            }
        }
    }

    // Match against concernedStop properties (concernedStop0, concernedStop1, ...)
    if (info.contains("properties") && info["properties"].is_object()) {
        const auto& props = info["properties"];
        for (auto it = props.begin(); it != props.end(); ++it) {
            const std::string& key = it.key();
            if (key.rfind("concernedStop", 0) == 0 && it->is_string()) {
                if (it->get<std::string>() == stopId) return true;
            }
        }
    }

    return false;
}

// --- Helper: Fetch Notifications from Provider ---
json fetchNotificationsFromProvider(const std::string& baseUrl, const std::string& stopId) {
    std::string url = baseUrl + "XML_ADDINFO_REQUEST";
    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Parameters{
            {"commonMacro", "addinfo"},
            {"outputFormat", "rapidJSON"},
            {"filterPublished", "1"},
            {"filterValid", "1"},
            {"filterShowLineList", "0"},
            {"filterShowPlaceList", "0"},
            {"itdLPxx_selStop", stopId}
        },
        cpr::Timeout{UPSTREAM_TIMEOUT_SECONDS * 1000}
    );

    if (r.status_code != 200) {
        return json::object();
    }

    try {
        return json::parse(r.text);
    } catch (...) {
        return json::object();
    }
}

// --- Helper: Extract Valid Notifications for a Stop ---

json extractValidNotifications(const std::string& stopId) {
    json notifications = json::array();
    std::set<std::string> seenIds;

    for (const auto& providerUrl : NOTIFICATION_API_PROVIDERS) {
        json response = fetchNotificationsFromProvider(providerUrl, stopId);

        if (!response.contains("infos") || !response["infos"].is_object()) continue;
        if (!response["infos"].contains("current") || !response["infos"]["current"].is_array()) continue;

        for (const auto& info : response["infos"]["current"]) {
            if (!isStopAffected(info, stopId)) continue;

            std::string alertId = info.contains("id") && info["id"].is_string()
                ? info["id"].get<std::string>() : "";

            // Skip duplicate alerts across providers
            if (!alertId.empty() && seenIds.find(alertId) != seenIds.end()) continue;
            if (!alertId.empty()) seenIds.insert(alertId);

            std::string priority = info.contains("priority") && info["priority"].is_string()
                ? info["priority"].get<std::string>() : "";

            std::string providerCode;
            if (info.contains("properties") && info["properties"].is_object() &&
                info["properties"].contains("providerCode") && info["properties"]["providerCode"].is_string()) {
                providerCode = info["properties"]["providerCode"].get<std::string>();
            }

            if (!info.contains("infoLinks") || !info["infoLinks"].is_array()) continue;

            for (const auto& link : info["infoLinks"]) {
                json notif;
                notif["id"] = alertId;
                notif["urlText"] = link.contains("urlText") && link["urlText"].is_string()
                    ? link["urlText"].get<std::string>() : "";
                notif["content"] = link.contains("content") && link["content"].is_string()
                    ? link["content"].get<std::string>() : "";
                notif["subtitle"] = link.contains("subtitle") && link["subtitle"].is_string()
                    ? link["subtitle"].get<std::string>() : "";
                notif["providerCode"] = providerCode;
                notif["priority"] = priority;
                notifications.push_back(notif);
            }
        }
    }

    return notifications;
}

int main() {
    crow::SimpleApp app;
    initAuth();
    db_config = loadDbConfig(DB_CONFIG_PATH);
    if (!db_config) {
        db_config = loadDbConfig(DB_CONFIG_CONTAINER_PATH);
    }
    if (!db_config) {
        std::cerr << "Database config unavailable. Stop persistence disabled." << std::endl;
    }

    // --- Route: Health Check (no authentication required) ---
    CROW_ROUTE(app, "/health")
    ([](const crow::request& /*req*/){
        auto response = crow::response(200, R"({"status":"ok"})");
        response.set_header("Content-Type", "application/json");
        return response;
    });

    // --- Route: Search Stops ---
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req){
        if (!isAuthenticated(req)) return unauthorizedResponse();
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
            ensureStopsInDatabase(searchResult, queryStr);
        }
        auto response = crow::response(searchResult.dump());
        setSecurityHeaders(response);
        return response;

    });

    // --- Route: Departures ---
    CROW_ROUTE(app, "/api/stops/<string>")
    ([](const crow::request& req, std::string stopId){
        if (!isAuthenticated(req)) return unauthorizedResponse();
        if (!isValidStopId(stopId)) {
            auto response = crow::response(400, R"({"error":"Invalid stop ID"})");
            setSecurityHeaders(response);
            return response;
        }

        const char* detailedParam = req.url_params.get("detailed");
        const char* delayParam = req.url_params.get("delay");

        bool detailed = (detailedParam && (std::string(detailedParam) == "true" || std::string(detailedParam) == "1"));
        bool includeDelay = (delayParam && (std::string(delayParam) == "true" || std::string(delayParam) == "1"));

        // Build cache key from stop ID and enabled options
        json allDepartures;
        bool cacheHit = false;
        std::string cacheKey = stopId + (detailed ? "_detailed" : "") + (includeDelay ? "_delay" : "");

        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = stop_cache.find(cacheKey);
            if (it != stop_cache.end()) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count() < CACHE_TTL_SECONDS) {
                    allDepartures = it->second.data;
                    cacheHit = true;
                }
            }
        }

        if (!cacheHit) {
            json rawData = fetchDeparturesProvider(stopId);
            if (rawData.contains("error")) {
                auto response = crow::response(502, rawData.dump());
                setSecurityHeaders(response);
                return response;
            }

            allDepartures = normalizeResponse(rawData, detailed, includeDelay);

            std::lock_guard<std::mutex> lock(cache_mutex);
            evictExpiredCacheEntries();
            if (stop_cache.size() < MAX_CACHE_ENTRIES) {
                stop_cache[cacheKey] = {allDepartures, std::chrono::steady_clock::now()};
            }
        }

        const char* requestedTrack = req.url_params.get("track");
        if (requestedTrack) {
            json filteredDepartures = json::array();
            std::string reqTrackStr = std::string(requestedTrack);

            for (const auto& dep : allDepartures) {
                std::string platform = dep.value("platform", "");
                bool match = false;

                if (platform == reqTrackStr) {
                    match = true;
                } else if (platform.size() > reqTrackStr.size() &&
                           platform.substr(0, reqTrackStr.size()) == reqTrackStr) {
                    if (!std::isdigit(static_cast<unsigned char>(platform[reqTrackStr.size()]))) match = true;
                } else if (platform.find(" " + reqTrackStr) != std::string::npos ||
                           platform.find("Gleis " + reqTrackStr) != std::string::npos) {
                    match = true;
                }

                if (match) filteredDepartures.push_back(dep);
            }
            auto response = crow::response(filteredDepartures.dump());
            setSecurityHeaders(response);
            return response;
        }

        auto response = crow::response(allDepartures.dump());
        setSecurityHeaders(response);
        return response;
});

    // --- Route: Current Notifications ---
    CROW_ROUTE(app, "/api/current_notifs")
    ([](const crow::request& req){
        if (!isAuthenticated(req)) return unauthorizedResponse();
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

    // --- Route: Nearby Stops ---
    CROW_ROUTE(app, "/api/stops/nearby")
    ([](const crow::request& req){
        if (!isAuthenticated(req)) return unauthorizedResponse();

        auto latParam = req.url_params.get("lat");
        auto lonParam = req.url_params.get("lon");
        auto radiusParam = req.url_params.get("radius");
        auto limitParam = req.url_params.get("limit");

        if (!latParam || !lonParam) {
            auto response = crow::response(400, R"({"error":"Missing 'lat' and/or 'lon' parameter"})");
            setSecurityHeaders(response);
            return response;
        }

        double lat, lon;
        try {
            lat = std::stod(std::string(latParam));
            lon = std::stod(std::string(lonParam));
        } catch (...) {
            auto response = crow::response(400, R"({"error":"Invalid 'lat' or 'lon' value"})");
            setSecurityHeaders(response);
            return response;
        }

        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            auto response = crow::response(400, R"({"error":"'lat' must be between -90 and 90, 'lon' must be between -180 and 180"})");
            setSecurityHeaders(response);
            return response;
        }

        double radius = 1000.0; // default 1000 meters
        if (radiusParam) {
            try {
                radius = std::stod(std::string(radiusParam));
            } catch (...) {
                auto response = crow::response(400, R"({"error":"Invalid 'radius' value"})");
                setSecurityHeaders(response);
                return response;
            }
            if (radius <= 0 || radius > 50000) {
                auto response = crow::response(400, R"({"error":"'radius' must be greater than 0 and at most 50000 meters"})");
                setSecurityHeaders(response);
                return response;
            }
        }

        int limit = 10; // default 10 results
        if (limitParam) {
            try {
                limit = std::stoi(std::string(limitParam));
            } catch (...) {
                auto response = crow::response(400, R"({"error":"Invalid 'limit' value"})");
                setSecurityHeaders(response);
                return response;
            }
            if (limit < 1 || limit > 100) {
                auto response = crow::response(400, R"({"error":"'limit' must be between 1 and 100"})");
                setSecurityHeaders(response);
                return response;
            }
        }

        if (!db_config) {
            auto response = crow::response(503, R"({"error":"Database not configured"})");
            setSecurityHeaders(response);
            return response;
        }

        PGconn* conn = connectToDatabase(*db_config);
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            if (conn) PQfinish(conn);
            auto response = crow::response(503, R"({"error":"Database connection failed"})");
            setSecurityHeaders(response);
            return response;
        }

        const char* sql =
            "SELECT stop_id, local_id, stop_name, city, "
            "ST_Y(location::geometry) AS latitude, "
            "ST_X(location::geometry) AS longitude, "
            "ST_Distance(location, ST_SetSRID(ST_MakePoint($1, $2), 4326)::geography) AS distance "
            "FROM stops "
            "WHERE ST_DWithin(location, ST_SetSRID(ST_MakePoint($1, $2), 4326)::geography, $3) "
            "ORDER BY distance ASC "
            "LIMIT $4;";

        std::string lonText = formatDouble(lon);
        std::string latText = formatDouble(lat);
        std::string radiusText = formatDouble(radius);
        std::string limitText = std::to_string(limit);

        const char* values[4] = { lonText.c_str(), latText.c_str(), radiusText.c_str(), limitText.c_str() };

        PGresult* res = PQexecParams(conn, sql, 4, nullptr, values, nullptr, nullptr, 0);
        if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
            std::cerr << "Nearby stops query failed: " << PQerrorMessage(conn) << std::endl;
            if (res) PQclear(res);
            PQfinish(conn);
            auto response = crow::response(500, R"({"error":"Database query failed"})");
            setSecurityHeaders(response);
            return response;
        }

        json result = json::array();
        int nRows = PQntuples(res);
        for (int i = 0; i < nRows; ++i) {
            json stop;
            stop["stop_id"] = PQgetisnull(res, i, 0) ? "" : PQgetvalue(res, i, 0);
            stop["local_id"] = PQgetisnull(res, i, 1) ? nullptr : json(PQgetvalue(res, i, 1));
            stop["stop_name"] = PQgetisnull(res, i, 2) ? "" : PQgetvalue(res, i, 2);
            stop["city"] = PQgetisnull(res, i, 3) ? nullptr : json(PQgetvalue(res, i, 3));

            if (!PQgetisnull(res, i, 4) && !PQgetisnull(res, i, 5)) {
                stop["latitude"] = std::stod(PQgetvalue(res, i, 4));
                stop["longitude"] = std::stod(PQgetvalue(res, i, 5));
            }

            if (!PQgetisnull(res, i, 6)) {
                stop["distance_meters"] = std::stod(PQgetvalue(res, i, 6));
            }

            result.push_back(stop);
        }

        PQclear(res);
        PQfinish(conn);

        auto response = crow::response(result.dump());
        setSecurityHeaders(response);
        return response;
    });

    app.port(8080).multithreaded().run();
}
