#include "departures_service.h"
#include <cpr/cpr.h>
#include <ctime>
#include <set>
#include <mutex>

// --- Departure Cache ---
static std::mutex cache_mutex;
static std::map<std::string, CacheEntry> stop_cache;

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
json normalizeResponse(const json& ProviderData, bool detailed, bool includeDelay) {
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

// --- Departure Cache + Fetch + Filter ---
json getDepartures(const std::string& stopId, bool detailed, bool includeDelay, const char* track) {
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
            return rawData;  // Caller checks for "error" key to return 502
        }

        allDepartures = normalizeResponse(rawData, detailed, includeDelay);

        std::lock_guard<std::mutex> lock(cache_mutex);
        evictExpiredCacheEntries(stop_cache);
        if (stop_cache.size() < MAX_CACHE_ENTRIES) {
            stop_cache[cacheKey] = {allDepartures, std::chrono::steady_clock::now()};
        }
    }

    if (track) {
        json filteredDepartures = json::array();
        std::string reqTrackStr = std::string(track);

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
        return filteredDepartures;
    }

    return allDepartures;
}

// --- Helper: Parse ISO 8601 Timestamp ---

static std::optional<std::time_t> parseISO8601(const std::string& timestamp) {
    std::tm tm = {};
    std::istringstream ss(timestamp);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return std::nullopt;
    return timegm(&tm); // Interpret as UTC
}

// --- Helper: Validity Time Range Check ---
static bool isCurrentlyValid(const json& validity) {
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

static bool isStopAffected(const json& info, const std::string& stopId) {
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
static json fetchNotificationsFromProvider(const std::string& baseUrl, const std::string& stopId) {
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
