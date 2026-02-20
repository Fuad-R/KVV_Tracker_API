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
#include <libpq-fe.h>

using json = nlohmann::json;

// --- Cache Structure ---
struct CacheEntry {
    json data;
    std::chrono::steady_clock::time_point timestamp;
};

std::mutex cache_mutex;
std::map<std::string, CacheEntry> stop_cache;
const int CACHE_TTL_SECONDS = 30;

// --- Configuration ---
const std::string Provider_DM_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
const std::string Provider_SEARCH_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";
const std::string DB_CONFIG_PATH = "db_connection.txt";
const std::string DB_CONFIG_CONTAINER_PATH = "/config/db_connection.txt";

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

bool extractCoordinates(const json& stop, double& lat, double& lon) {
    auto extractFromObject = [&](const json& coord) -> bool {
        if (coord.contains("x") && coord.contains("y")) {
            // Provider returns x/y in latitude/longitude order.
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

struct StopRecord {
    std::string stop_id;
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

    double latitude = 0.0;
    double longitude = 0.0;
    if (!extractCoordinates(stop, latitude, longitude)) return std::nullopt;

    auto city = getJsonString(stop, {"city", "place", "locality", "town"});
    if (!city && stop.contains("parent") && stop.at("parent").is_object()) {
        city = getJsonString(stop.at("parent"), {"name", "city", "place", "locality", "town"});
    }
    return StopRecord{
        *stopId,
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

void ensureStopsInDatabase(const json& searchResult, const std::string& originalSearch) {
    if (!db_config) return;

    auto records = extractStopRecords(searchResult);
    if (records.empty()) return;

    PGconn* conn = connectToDatabase(*db_config);
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "Database connection failed: " << PQerrorMessage(conn) << std::endl;
        PQfinish(conn);
        return;
    }

    const char* insertSql =
        "INSERT INTO stops (stop_id, stop_name, city, mot, location, original_search) "
        "VALUES ($1, $2, $3, $4, ST_SetSRID(ST_MakePoint($5, $6), 4326)::geography, $7) "
        "ON CONFLICT (stop_id) DO UPDATE SET "
        "stop_name = EXCLUDED.stop_name, "
        "city = COALESCE(EXCLUDED.city, stops.city), "
        "mot = COALESCE(EXCLUDED.mot, stops.mot), "
        "location = EXCLUDED.location, "
        "original_search = COALESCE(EXCLUDED.original_search, stops.original_search), "
        "last_updated = NOW();";

    for (const auto& record : records) {
        std::string longitudeText = formatDouble(record.longitude);
        std::string latitudeText = formatDouble(record.latitude);

        const char* values[7];
        values[0] = record.stop_id.c_str();
        values[1] = record.stop_name.c_str();
        values[2] = record.city.empty() ? nullptr : record.city.c_str();
        values[3] = record.mot_array ? record.mot_array->c_str() : nullptr;
        values[4] = longitudeText.c_str();
        values[5] = latitudeText.c_str();
        values[6] = originalSearch.empty() ? nullptr : originalSearch.c_str();

        PGresult* res = PQexecParams(conn, insertSql, 7, nullptr, values, nullptr, nullptr, 0);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "Failed to insert stop " << record.stop_id << ": "
                      << PQerrorMessage(conn) << std::endl;
        }
        PQclear(res);
    }

    PQfinish(conn);
}

// --- Helper: Search Stops ---
// Accepts 'city' parameter but ignores it (no-op)
json searchStopsProvider(const std::string& query, const std::string& city = "", bool includeLocation = false) {
    if (query.empty()) return json::array();

    // Configuration based on the user's sample and PDF
    cpr::Parameters params{
            {"outputFormat", "rapidJSON"},
            {"type_sf", "any"},
            {"name_sf", query},
            {"anyObjFilter_sf", "2"}, // Filter for stops/stations
            {"coordOutputFormat", "WGS84[dd.ddddd]"} // Request decimal coordinates
    };

    cpr::Response r = cpr::Get(
        cpr::Url{Provider_SEARCH_URL},
        params
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
        }
    );

    if (r.status_code != 200) return {{"error", "Upstream Provider error"}, {"code", r.status_code}};

    try {
        return json::parse(r.text);
    } catch (...) {
        return {{"error", "Invalid JSON from Provider"}};
    }
}

// --- Helper: Normalize Data ---
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

        // Line Info
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            // ADD THIS: Extract MOT (Mode of Transport)
            if (dep["servingLine"].contains("motType")) {
                item["mot"] = std::stoi(dep["servingLine"].value("motType", "-1"));
            } else {
                item["mot"] = -1; // Unknown/not provided
            }

            // Delay field (from servingLine.delay)
            if (includeDelay && dep["servingLine"].contains("delay")) {
                try {
                    int delayMinutes = std::stoi(dep["servingLine"].value("delay", "0"));
                    item["delay_minutes"] = delayMinutes;
                } catch (...) {
                    item["delay_minutes"] = 0;
                }
            }

            if (detailed) {
                // ... (rest of your detailed code stays the same)
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

        // Platform
        if (dep.contains("platform")) item["platform"] = dep.value("platform", "");
        else if (dep.contains("platformName")) item["platform"] = dep.value("platformName", "");
        else item["platform"] = "Unknown";

        // Countdown
        if (dep.contains("countdown")) item["minutes_remaining"] = std::stoi(dep.value("countdown", "0"));
        else item["minutes_remaining"] = 0;

        // Realtime
        item["is_realtime"] = dep.contains("realDateTime");

        // Hints
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

int main() {
    crow::SimpleApp app;
    db_config = loadDbConfig(DB_CONFIG_PATH);
    if (!db_config) {
        db_config = loadDbConfig(DB_CONFIG_CONTAINER_PATH);
    }
    if (!db_config) {
        std::cerr << "Database config unavailable. Stop persistence disabled." << std::endl;
    }

    // Search Endpoint
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req){
        auto query = req.url_params.get("q");
        auto city = req.url_params.get("city");
        auto locationParam = req.url_params.get("location");

        bool includeLocation = (locationParam && (std::string(locationParam) == "true" || std::string(locationParam) == "1"));

        if (!query) return crow::response(400, "Missing 'q' parameter");

        json searchResult = searchStopsProvider(std::string(query), city ? std::string(city) : "", includeLocation);
        bool hasError = searchResult.is_object() && searchResult.contains("error");
        if (!hasError) {
            ensureStopsInDatabase(searchResult, std::string(query));
        }
        auto response = crow::response(searchResult.dump());
        response.set_header("Content-Type", "application/json");
        return response;

    });

    // Departures Endpoint
    CROW_ROUTE(app, "/api/stops/<string>")
    ([](const crow::request& req, std::string stopId){
        const char* detailedParam = req.url_params.get("detailed");
        const char* delayParam = req.url_params.get("delay");

        bool detailed = (detailedParam && (std::string(detailedParam) == "true" || std::string(detailedParam) == "1"));
        bool includeDelay = (delayParam && (std::string(delayParam) == "true" || std::string(delayParam) == "1"));

        // Cache Check - include delay in cache key
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
            if (rawData.contains("error")) return crow::response(502, rawData.dump());

            allDepartures = normalizeResponse(rawData, detailed, includeDelay);

            std::lock_guard<std::mutex> lock(cache_mutex);
            stop_cache[cacheKey] = {allDepartures, std::chrono::steady_clock::now()};
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
            return crow::response(filteredDepartures.dump());
        }

        return crow::response(allDepartures.dump());
});

    app.port(8080).multithreaded().run();
}
