#include "transit_tracker/core.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <regex>
#include <set>
#include <sstream>

namespace transit_tracker {

const std::string kProviderDmUrl = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
const std::string kProviderSearchUrl = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";
const std::string kDbConfigPath = "db_connection.txt";
const std::string kDbConfigContainerPath = "/config/db_connection.txt";
const std::vector<std::string> kNotificationApiProviders = {
    "https://www.efa-bw.de/nvbw/",
    "https://efa.vrr.de/standard/"
};

namespace {

std::time_t portableTimegm(std::tm* tm) {
#if defined(_WIN32)
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

}  // namespace

bool isValidStopId(const std::string& stop_id) {
    if (stop_id.empty() || stop_id.size() > kMaxStopIdLength) return false;
    static const std::regex stop_id_pattern("^[a-zA-Z0-9:_. -]+$");
    return std::regex_match(stop_id, stop_id_pattern);
}

bool isValidSearchQuery(const std::string& query) {
    if (query.empty() || query.size() > kMaxQueryLength) return false;
    for (unsigned char c : query) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

std::string toLower(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string trim(const std::string& input) {
    auto start = std::find_if_not(input.begin(), input.end(),
                                  [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(input.rbegin(), input.rend(),
                                [](unsigned char c) { return std::isspace(c); }).base();
    if (start >= end) return "";
    return std::string(start, end);
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

std::string sha256Hex(const std::string& input) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(ctx, input.c_str(), input.size()) == 1 &&
              EVP_DigestFinal_ex(ctx, hash, &hash_len) == 1;
    EVP_MD_CTX_free(ctx);

    if (!ok || hash_len == 0) return "";

    std::ostringstream stream;
    for (unsigned int i = 0; i < hash_len; ++i) {
        stream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return stream.str();
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return result == 0;
}

std::optional<DbConfig> loadDbConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;

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
        return std::nullopt;
    }

    if (config.sslmode.empty()) config.sslmode = "require";
    return config;
}

RequestParameters buildSearchStopRequestParameters(const std::string& query) {
    return {
        {"outputFormat", "rapidJSON"},
        {"type_sf", "any"},
        {"name_sf", query},
        {"anyObjFilter_sf", "2"},
        {"coordOutputFormat", "WGS84[dd.ddddd]"}
    };
}

RequestParameters buildDepartureRequestParameters(const std::string& stop_id) {
    return {
        {"outputFormat", "JSON"},
        {"depType", "stopEvents"},
        {"mode", "direct"},
        {"type_dm", "stop"},
        {"name_dm", stop_id},
        {"useRealtime", "1"},
        {"limit", "40"}
    };
}

RequestParameters buildNotificationRequestParameters(const std::string& stop_id) {
    return {
        {"commonMacro", "addinfo"},
        {"outputFormat", "rapidJSON"},
        {"filterPublished", "1"},
        {"filterValid", "1"},
        {"filterShowLineList", "0"},
        {"filterShowPlaceList", "0"},
        {"itdLPxx_selStop", stop_id}
    };
}

std::string buildCacheKey(const std::string& stop_id, bool detailed, bool include_delay) {
    return stop_id + (detailed ? "_detailed" : "") + (include_delay ? "_delay" : "");
}

bool matchesTrackFilter(const std::string& platform, const std::string& requested_track) {
    if (requested_track.empty()) return false;
    if (platform == requested_track) return true;

    if (platform.size() > requested_track.size() &&
        platform.substr(0, requested_track.size()) == requested_track) {
        return !std::isdigit(static_cast<unsigned char>(platform[requested_track.size()]));
    }

    return platform.find(" " + requested_track) != std::string::npos ||
           platform.find("Gleis " + requested_track) != std::string::npos;
}

json filterDeparturesByTrack(const json& departures, const std::string& requested_track) {
    json filtered = json::array();
    for (const auto& dep : departures) {
        if (matchesTrackFilter(dep.value("platform", ""), requested_track)) {
            filtered.push_back(dep);
        }
    }
    return filtered;
}

void evictExpiredCacheEntries(
    std::map<std::string, CacheEntry>& cache,
    std::chrono::steady_clock::time_point now) {
    for (auto it = cache.begin(); it != cache.end();) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count() >=
            kCacheTtlSeconds) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<std::string> buildMotArray(const json& stop) {
    std::vector<int> mot_values;

    auto add_value = [&](const json& value) {
        if (value.is_number_integer()) {
            mot_values.push_back(value.get<int>());
        } else if (value.is_string()) {
            try {
                mot_values.push_back(std::stoi(value.get<std::string>()));
            } catch (...) {
                return;
            }
        } else if (value.is_object()) {
            auto nested = getJsonString(value, {"motType", "type", "mode"});
            if (nested) {
                try {
                    mot_values.push_back(std::stoi(*nested));
                } catch (...) {
                    return;
                }
            }
        }
    };

    if (stop.contains("modes")) {
        const auto& modes = stop.at("modes");
        if (modes.is_array()) {
            for (const auto& item : modes) add_value(item);
        } else {
            add_value(modes);
        }
    } else if (stop.contains("mot")) {
        const auto& mot = stop.at("mot");
        if (mot.is_array()) {
            for (const auto& item : mot) add_value(item);
        } else {
            add_value(mot);
        }
    } else if (stop.contains("productClasses")) {
        const auto& classes = stop.at("productClasses");
        if (classes.is_array()) {
            for (const auto& item : classes) add_value(item);
        } else {
            add_value(classes);
        }
    } else if (stop.contains("motType")) {
        add_value(stop.at("motType"));
    }

    if (mot_values.empty()) return std::nullopt;

    std::ostringstream stream;
    stream << "{";
    for (size_t i = 0; i < mot_values.size(); ++i) {
        if (i > 0) stream << ",";
        stream << mot_values[i];
    }
    stream << "}";
    return stream.str();
}

bool extractCoordinates(const json& stop, double& lat, double& lon) {
    auto extract_from_object = [&](const json& coord) -> bool {
        if (coord.contains("x") && coord.contains("y")) {
            auto lat_value = jsonToDouble(coord.at("x"));
            auto lon_value = jsonToDouble(coord.at("y"));
            if (lat_value && lon_value) {
                lat = *lat_value;
                lon = *lon_value;
                return true;
            }
        }

        if (coord.contains("lon") && coord.contains("lat")) {
            auto lon_value = jsonToDouble(coord.at("lon"));
            auto lat_value = jsonToDouble(coord.at("lat"));
            if (lat_value && lon_value) {
                lat = *lat_value;
                lon = *lon_value;
                return true;
            }
        }

        if (coord.contains("longitude") && coord.contains("latitude")) {
            auto lon_value = jsonToDouble(coord.at("longitude"));
            auto lat_value = jsonToDouble(coord.at("latitude"));
            if (lat_value && lon_value) {
                lat = *lat_value;
                lon = *lon_value;
                return true;
            }
        }

        return false;
    };

    if (stop.contains("coord")) {
        const auto& coord = stop.at("coord");
        if (coord.is_object() && extract_from_object(coord)) return true;
        if (coord.is_array() && coord.size() >= 2) {
            auto lat_value = jsonToDouble(coord.at(0));
            auto lon_value = jsonToDouble(coord.at(1));
            if (lat_value && lon_value) {
                lat = *lat_value;
                lon = *lon_value;
                return true;
            }
        }
    }

    if (extract_from_object(stop)) return true;

    if (stop.contains("latitude") && stop.contains("longitude")) {
        auto lat_value = jsonToDouble(stop.at("latitude"));
        auto lon_value = jsonToDouble(stop.at("longitude"));
        if (lat_value && lon_value) {
            lat = *lat_value;
            lon = *lon_value;
            return true;
        }
    }

    return false;
}

std::optional<StopRecord> parseStopRecord(const json& stop) {
    auto stop_id = getJsonString(stop, {"id", "stopId", "stopID", "gid"});
    auto stop_name = getJsonString(stop, {"name", "stopName", "stop_name"});
    if (!stop_id || !stop_name) return std::nullopt;

    std::optional<std::string> local_id;
    if (stop.contains("properties") && stop.at("properties").is_object()) {
        local_id = getJsonString(stop.at("properties"),
                                 {"stopId", "stopID", "stopid", "localId", "local_id"});
    }
    if (!local_id) {
        local_id = getJsonString(stop, {"localId", "local_id"});
    }

    double latitude = 0.0;
    double longitude = 0.0;
    if (!extractCoordinates(stop, latitude, longitude)) return std::nullopt;

    auto city = getJsonString(stop, {"city", "place", "locality", "town"});
    if (!city && stop.contains("parent") && stop.at("parent").is_object()) {
        city = getJsonString(stop.at("parent"), {"name", "city", "place", "locality", "town"});
    }

    return StopRecord{
        *stop_id,
        local_id,
        *stop_name,
        city.value_or(""),
        buildMotArray(stop),
        latitude,
        longitude
    };
}

std::vector<StopRecord> extractStopRecords(const json& search_result) {
    const json* stop_array = nullptr;

    if (search_result.is_array()) {
        stop_array = &search_result;
    } else if (search_result.is_object()) {
        if (search_result.contains("stopFinder")) {
            const auto& finder = search_result.at("stopFinder");
            if (finder.contains("points") && finder.at("points").is_array()) {
                stop_array = &finder.at("points");
            } else if (finder.contains("locations") && finder.at("locations").is_array()) {
                stop_array = &finder.at("locations");
            } else if (finder.contains("points") && finder.at("points").contains("point") &&
                       finder.at("points").at("point").is_array()) {
                stop_array = &finder.at("points").at("point");
            }
        }

        if (!stop_array && search_result.contains("locations") && search_result.at("locations").is_array()) {
            stop_array = &search_result.at("locations");
        }
        if (!stop_array && search_result.contains("points") && search_result.at("points").is_array()) {
            stop_array = &search_result.at("points");
        }
    }

    std::vector<StopRecord> records;
    if (!stop_array) return records;

    for (const auto& stop : *stop_array) {
        if (!stop.is_object()) continue;
        auto record = parseStopRecord(stop);
        if (record) records.push_back(*record);
    }

    return records;
}

json normalizeResponse(const json& provider_data, bool detailed, bool include_delay) {
    json result = json::array();
    if (!provider_data.contains("departureList")) return result;

    auto str_to_bool = [](const std::string& value) {
        std::string normalized = value;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return normalized == "1" || normalized == "true" || normalized == "yes";
    };

    for (const auto& dep : provider_data["departureList"]) {
        json item;

        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            if (dep["servingLine"].contains("motType")) {
                try {
                    item["mot"] = std::stoi(dep["servingLine"].value("motType", "-1"));
                } catch (...) {
                    item["mot"] = -1;
                }
            } else {
                item["mot"] = -1;
            }

            if (include_delay && dep["servingLine"].contains("delay")) {
                try {
                    item["delay_minutes"] = std::stoi(dep["servingLine"].value("delay", "0"));
                } catch (...) {
                    item["delay_minutes"] = 0;
                }
            }

            if (detailed) {
                bool has_plan_low_floor = false;
                bool has_plan_wheelchair = false;
                bool plan_low_floor = false;
                bool plan_wheelchair = false;

                if (dep.contains("attrs") && dep["attrs"].is_array()) {
                    for (const auto& attr : dep["attrs"]) {
                        std::string name = toLower(attr.value("name", ""));
                        std::string value = attr.value("value", "");
                        if (name == "planlowfloorvehicle") {
                            has_plan_low_floor = true;
                            plan_low_floor = str_to_bool(value);
                        } else if (name == "planwheelchairaccess") {
                            has_plan_wheelchair = true;
                            plan_wheelchair = str_to_bool(value);
                        }
                    }
                }

                bool hint_low_floor = false;
                bool hint_wheelchair = false;
                if (dep["servingLine"].contains("hints") && dep["servingLine"]["hints"].is_array()) {
                    for (const auto& hint : dep["servingLine"]["hints"]) {
                        std::string text = hint.value("hint", hint.value("content", ""));
                        if (text.find("Niederflur") != std::string::npos ||
                            text.find("low floor") != std::string::npos ||
                            text.find("lowFloor") != std::string::npos) {
                            hint_low_floor = true;
                        }
                        if (text.find("Rollstuhl") != std::string::npos ||
                            text.find("wheelchair") != std::string::npos ||
                            text.find("barrierefrei") != std::string::npos ||
                            text.find("barrier-free") != std::string::npos) {
                            hint_wheelchair = true;
                        }
                    }
                }

                bool low_floor = has_plan_low_floor ? plan_low_floor : hint_low_floor;
                bool wheelchair = has_plan_wheelchair ? plan_wheelchair : hint_wheelchair;

                item["low_floor"] = low_floor;
                item["wheelchair_accessible"] = wheelchair || low_floor;

                if (dep["servingLine"].contains("trainType")) {
                    item["train_type"] = dep["servingLine"].value("trainType", "");
                }
                if (dep["servingLine"].contains("trainLength")) {
                    item["train_length"] = dep["servingLine"].value("trainLength", "");
                } else if (dep["servingLine"].contains("trainComposition")) {
                    item["train_composition"] = dep["servingLine"].value("trainComposition", "");
                }
            }
        }

        if (dep.contains("platform")) item["platform"] = dep.value("platform", "");
        else if (dep.contains("platformName")) item["platform"] = dep.value("platformName", "");
        else item["platform"] = "Unknown";

        if (dep.contains("countdown")) {
            try {
                item["minutes_remaining"] = std::stoi(dep.value("countdown", "0"));
            } catch (...) {
                item["minutes_remaining"] = 0;
            }
        } else {
            item["minutes_remaining"] = 0;
        }

        item["is_realtime"] = dep.contains("realDateTime");

        if (detailed && dep.contains("hints") && dep["hints"].is_array()) {
            json hints = json::array();
            for (const auto& hint : dep["hints"]) {
                std::string text = hint.value("hint", hint.value("content", ""));
                if (!text.empty()) hints.push_back(text);
            }
            if (!hints.empty()) item["hints"] = hints;
        }

        result.push_back(item);
    }

    return result;
}

std::optional<std::time_t> parseISO8601(const std::string& timestamp) {
    std::tm tm = {};
    std::istringstream stream(timestamp);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) return std::nullopt;
    return portableTimegm(&tm);
}

bool isCurrentlyValid(const json& validity, std::time_t now) {
    if (!validity.is_array() || validity.empty()) return false;

    for (const auto& range : validity) {
        if (!range.contains("from") || !range.contains("to")) continue;
        if (!range["from"].is_string() || !range["to"].is_string()) continue;

        auto from_time = parseISO8601(range["from"].get<std::string>());
        auto to_time = parseISO8601(range["to"].get<std::string>());
        if (!from_time || !to_time) continue;

        if (now >= *from_time && now <= *to_time) return true;
    }

    return false;
}

bool isCurrentlyValid(const json& validity) {
    return isCurrentlyValid(validity, std::time(nullptr));
}

bool isStopAffected(const json& info, const std::string& stop_id) {
    if (info.contains("affected") && info["affected"].is_object()) {
        if (info["affected"].contains("stops") && info["affected"]["stops"].is_array()) {
            for (const auto& stop : info["affected"]["stops"]) {
                if (stop.contains("properties") && stop["properties"].is_object() &&
                    stop["properties"].contains("stopId")) {
                    if (stop["properties"]["stopId"].get<std::string>() == stop_id) return true;
                }

                if (stop.contains("id") && stop["id"].is_string()) {
                    if (stop["id"].get<std::string>() == stop_id) return true;
                }
            }
        }
    }

    if (info.contains("properties") && info["properties"].is_object()) {
        const auto& properties = info["properties"];
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (it.key().rfind("concernedStop", 0) == 0 && it->is_string()) {
                if (it->get<std::string>() == stop_id) return true;
            }
        }
    }

    return false;
}

json extractNotificationsFromResponses(const std::string& stop_id, const std::vector<json>& responses) {
    json notifications = json::array();
    std::set<std::string> seen_ids;

    for (const auto& response : responses) {
        if (!response.contains("infos") || !response["infos"].is_object()) continue;
        if (!response["infos"].contains("current") || !response["infos"]["current"].is_array()) continue;

        for (const auto& info : response["infos"]["current"]) {
            if (!isStopAffected(info, stop_id)) continue;

            std::string alert_id = info.contains("id") && info["id"].is_string()
                ? info["id"].get<std::string>()
                : "";

            if (!alert_id.empty() && seen_ids.find(alert_id) != seen_ids.end()) continue;
            if (!alert_id.empty()) seen_ids.insert(alert_id);

            std::string priority = info.contains("priority") && info["priority"].is_string()
                ? info["priority"].get<std::string>()
                : "";

            std::string provider_code;
            if (info.contains("properties") && info["properties"].is_object() &&
                info["properties"].contains("providerCode") && info["properties"]["providerCode"].is_string()) {
                provider_code = info["properties"]["providerCode"].get<std::string>();
            }

            if (!info.contains("infoLinks") || !info["infoLinks"].is_array()) continue;

            for (const auto& link : info["infoLinks"]) {
                json notification;
                notification["id"] = alert_id;
                notification["urlText"] = link.contains("urlText") && link["urlText"].is_string()
                    ? link["urlText"].get<std::string>()
                    : "";
                notification["content"] = link.contains("content") && link["content"].is_string()
                    ? link["content"].get<std::string>()
                    : "";
                notification["subtitle"] = link.contains("subtitle") && link["subtitle"].is_string()
                    ? link["subtitle"].get<std::string>()
                    : "";
                notification["providerCode"] = provider_code;
                notification["priority"] = priority;
                notifications.push_back(notification);
            }
        }
    }

    return notifications;
}

}  // namespace transit_tracker
