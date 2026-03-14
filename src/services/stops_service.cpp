#include "stops_service.h"
#include <cpr/cpr.h>

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

void ensureStopsInDatabase(const json& searchResult, const std::string& originalSearch, const Database& db) {
    if (!db.hasConfig()) return;

    auto records = extractStopRecords(searchResult);
    if (records.empty()) return;

    PGconn* conn = db.connect();
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

json searchStopsProvider(const std::string& query, const std::string& /*city*/, bool /*includeLocation*/) {
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
