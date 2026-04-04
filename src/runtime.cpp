#include "transit_tracker/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include <cpr/cpr.h>

namespace transit_tracker {

namespace {

constexpr int kLastUsedUpdateIntervalSeconds = 300;

cpr::Parameters toCprParameters(const RequestParameters& parameters) {
    cpr::Parameters result;
    for (const auto& [key, value] : parameters) {
        result.Add({key, value});
    }
    return result;
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

PGconn* getAuthDbConnection(AppState& state) {
    if (state.auth_db_conn && PQstatus(state.auth_db_conn) == CONNECTION_OK) {
        return state.auth_db_conn;
    }

    if (state.auth_db_conn) {
        PQfinish(state.auth_db_conn);
        state.auth_db_conn = nullptr;
    }

    if (!state.db_config) return nullptr;
    state.auth_db_conn = connectToDatabase(*state.db_config);
    if (!state.auth_db_conn) return nullptr;

    if (PQstatus(state.auth_db_conn) != CONNECTION_OK) {
        std::cerr << "Auth DB connection failed: " << PQerrorMessage(state.auth_db_conn) << std::endl;
        PQfinish(state.auth_db_conn);
        state.auth_db_conn = nullptr;
        return nullptr;
    }

    return state.auth_db_conn;
}

}  // namespace

void initializeAuthFromEnvironment(AppState& state) {
    const char* auth_env = std::getenv("AUTH");
    if (!auth_env) return;

    std::string auth_value(auth_env);
    std::transform(auth_value.begin(), auth_value.end(), auth_value.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (auth_value == "true" || auth_value == "1" || auth_value == "yes") {
        state.auth_enabled = true;
        const char* key_env = std::getenv("API_KEY");
        if (key_env && std::string(key_env).length() > 0) {
            state.api_key = key_env;
            std::cout << "API key authentication enabled (env-var fallback available)." << std::endl;
        } else {
            std::cout << "API key authentication enabled (database mode)." << std::endl;
        }
    }
}

std::shared_ptr<AppState> createProductionState() {
    auto state = std::make_shared<AppState>();
    initializeAuthFromEnvironment(*state);

    state->db_config = loadDbConfig(kDbConfigPath);
    if (!state->db_config) {
        state->db_config = loadDbConfig(kDbConfigContainerPath);
    }
    if (!state->db_config) {
        std::cerr << "Database config unavailable. Stop persistence disabled." << std::endl;
    }

    return state;
}

json searchStopsProvider(const std::string& query, const std::string& /*city*/, bool /*include_location*/) {
    if (query.empty()) return json::array();

    cpr::Response response = cpr::Get(
        cpr::Url{kProviderSearchUrl},
        toCprParameters(buildSearchStopRequestParameters(query)),
        cpr::Timeout{kUpstreamTimeoutSeconds * 1000});

    if (response.status_code != 200) return {{"error", "Upstream Error"}};

    try {
        return json::parse(response.text);
    } catch (...) {
        return {{"error", "Invalid JSON from Provider Search"}};
    }
}

json fetchDeparturesProvider(const std::string& stop_id) {
    cpr::Response response = cpr::Get(
        cpr::Url{kProviderDmUrl},
        toCprParameters(buildDepartureRequestParameters(stop_id)),
        cpr::Timeout{kUpstreamTimeoutSeconds * 1000});

    if (response.status_code != 200) {
        return {{"error", "Upstream Provider error"}, {"code", response.status_code}};
    }

    try {
        return json::parse(response.text);
    } catch (...) {
        return {{"error", "Invalid JSON from Provider"}};
    }
}

json fetchNotificationsFromProvider(const std::string& base_url, const std::string& stop_id) {
    cpr::Response response = cpr::Get(
        cpr::Url{base_url + "XML_ADDINFO_REQUEST"},
        toCprParameters(buildNotificationRequestParameters(stop_id)),
        cpr::Timeout{kUpstreamTimeoutSeconds * 1000});

    if (response.status_code != 200) return json::object();

    try {
        return json::parse(response.text);
    } catch (...) {
        return json::object();
    }
}

void ensureStopsInDatabase(AppState& state, const json& search_result, const std::string& original_search) {
    if (!state.db_config) return;

    auto records = extractStopRecords(search_result);
    if (records.empty()) return;

    PGconn* conn = connectToDatabase(*state.db_config);
    if (!conn) {
        std::cerr << "Database connection returned null" << std::endl;
        return;
    }

    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "Database connection failed: " << PQerrorMessage(conn) << std::endl;
        PQfinish(conn);
        return;
    }

    const char* insert_sql =
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
        std::string longitude_text = formatDouble(record.longitude);
        std::string latitude_text = formatDouble(record.latitude);

        const char* values[8];
        values[0] = record.stop_id.c_str();
        values[1] = record.local_id ? record.local_id->c_str() : nullptr;
        values[2] = record.stop_name.c_str();
        values[3] = record.city.empty() ? nullptr : record.city.c_str();
        values[4] = record.mot_array ? record.mot_array->c_str() : nullptr;
        values[5] = longitude_text.c_str();
        values[6] = latitude_text.c_str();
        values[7] = original_search.empty() ? nullptr : original_search.c_str();

        PGresult* result = PQexecParams(conn, insert_sql, 8, nullptr, values, nullptr, nullptr, 0);
        if (!result) {
            std::cerr << "Failed to execute insert for stop " << record.stop_id
                      << ": PQexecParams returned null" << std::endl;
            continue;
        }
        if (PQresultStatus(result) != PGRES_COMMAND_OK) {
            std::cerr << "Failed to insert stop " << record.stop_id << ": "
                      << PQerrorMessage(conn) << std::endl;
        }
        PQclear(result);
    }

    PQfinish(conn);
}

bool validateKeyViaDatabase(AppState& state, const std::string& provided_key) {
    if (!state.db_config) return false;

    std::string key_hash = sha256Hex(provided_key);
    if (key_hash.empty()) return false;

    std::lock_guard<std::mutex> lock(state.auth_db_mutex);

    auto now = std::chrono::steady_clock::now();
    for (auto it = state.last_used_updates.begin(); it != state.last_used_updates.end();) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >=
            kLastUsedUpdateIntervalSeconds) {
            it = state.last_used_updates.erase(it);
        } else {
            ++it;
        }
    }

    PGconn* conn = getAuthDbConnection(state);
    if (!conn) return false;

    const char* query_sql =
        "SELECT id FROM api_keys "
        "WHERE key_hash = $1 AND revoked = FALSE "
        "AND (expires_at IS NULL OR expires_at > NOW())";
    const char* query_values[1] = {key_hash.c_str()};

    PGresult* result = PQexecParams(conn, query_sql, 1, nullptr, query_values, nullptr, nullptr, 0);
    if (!result) {
        std::cerr << "Auth query returned null result" << std::endl;
        return false;
    }

    bool valid = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
    if (valid) {
        std::string key_id = PQgetvalue(result, 0, 0);
        PQclear(result);

        auto it = state.last_used_updates.find(key_id);
        bool should_update = (it == state.last_used_updates.end()) ||
            (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() >=
             kLastUsedUpdateIntervalSeconds);

        if (should_update) {
            const char* update_sql = "UPDATE api_keys SET last_used_at = NOW() WHERE id = $1";
            const char* update_values[1] = {key_id.c_str()};
            PGresult* update_result = PQexecParams(conn, update_sql, 1, nullptr, update_values, nullptr, nullptr, 0);
            if (!update_result || PQresultStatus(update_result) != PGRES_COMMAND_OK) {
                std::cerr << "Failed to update last_used_at: " << PQerrorMessage(conn) << std::endl;
            } else {
                state.last_used_updates[key_id] = now;
            }
            if (update_result) PQclear(update_result);
        }
    } else {
        PQclear(result);
    }

    return valid;
}

AppDependencies createProductionDependencies(const std::shared_ptr<AppState>& state) {
    AppDependencies dependencies;
    dependencies.search_stops = searchStopsProvider;
    dependencies.fetch_departures = fetchDeparturesProvider;
    dependencies.fetch_notifications_from_provider = fetchNotificationsFromProvider;
    dependencies.ensure_stops_in_database = [state](const json& search_result, const std::string& original_search) {
        ensureStopsInDatabase(*state, search_result, original_search);
    };
    dependencies.validate_api_key = [state](const std::string& provided_key) {
        if (state->db_config) {
            return validateKeyViaDatabase(*state, provided_key);
        }
        if (!state->api_key.empty()) {
            return constantTimeEquals(provided_key, state->api_key);
        }
        return false;
    };
    dependencies.steady_now = [] { return std::chrono::steady_clock::now(); };
    dependencies.system_now = [] { return std::time(nullptr); };
    return dependencies;
}

}  // namespace transit_tracker
