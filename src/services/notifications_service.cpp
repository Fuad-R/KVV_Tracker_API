#include "notifications_service.h"
#include <cpr/cpr.h>
#include <ctime>
#include <set>

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
