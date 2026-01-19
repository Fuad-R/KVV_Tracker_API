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

using json = nlohmann::json;

// --- Cache Structure ---
struct CacheEntry {
    json data;
    std::chrono::steady_clock::time_point timestamp;
};

std::mutex cache_mutex;
std::map<std::string, CacheEntry> stop_cache;
const int CACHE_TTL_SECONDS = 30;

// --- KVV Upstream Config ---
const std::string KVV_DM_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
const std::string KVV_SEARCH_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";

// --- Helper: String Utilities ---
std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

// --- Helper: Search Stops (Refactored for Consistency) ---
json searchStopsKVV(const std::string& query, const std::string& city = "") {
    // 1. Prepare Query
    // We append '*' to allow partial matching (e.g., "Synag" -> "Synagoge")
    std::string wildCardQuery = query;
    if (wildCardQuery.empty()) return json::array();
    if (wildCardQuery.back() != '*') {
        wildCardQuery += "*";
    }

    // 2. Configure Parameters
    // We deliberately DO NOT use 'place_sf' (city filter) upstream because it
    // often results in [] when combined with wildcards on this EFA version.
    // Instead, we fetch a larger list (limit 100) and sort client-side.
    cpr::Parameters params{
        {"outputFormat", "JSON"},
        {"type_sf", "stop"},
        {"name_sf", wildCardQuery},
        {"anyObjFilter_sf", "2"},      // Filter for stops only
        {"anyMaxSizeHitList", "100"}   // Fetch more results to ensure our city is included
    };

    // 3. Perform Request
    cpr::Response r = cpr::Get(
        cpr::Url{KVV_SEARCH_URL},
        params
    );

    if (r.status_code != 200) return {{"error", "Upstream Error"}};

    try {
        json raw = json::parse(r.text);
        json result = json::array();

        if (raw.contains("stopFinder") && raw["stopFinder"].contains("points")) {
            auto& points = raw["stopFinder"]["points"];

            // Helper lambda to process a single point entry
            auto processPoint = [&](const json& p) {
                if (p.contains("stateless")) {
                    json item = {
                        {"id", p.value("stateless", "")},
                        {"name", p.value("name", "Unknown")}
                    };

                    std::string itemCity = "";
                    if (p.contains("place")) {
                        itemCity = p.value("place", "");
                        item["city"] = itemCity;
                    }

                    // Add to results
                    result.push_back(item);
                }
            };

            // EFA API can return 'points' as an array OR a single object
            if (points.is_array()) {
                for (const auto& p : points) processPoint(p);
            } else if (points.is_object()) {
                processPoint(points);
            }
        }

        // 4. Client-Side Sorting / Filtering
        // If the user requested a specific city, we sort those results to the top.
        if (!city.empty()) {
            std::string targetCity = toLower(city);

            // Stable sort preserves original EFA order among equal elements
            std::stable_sort(result.begin(), result.end(),
                [&](const json& a, const json& b) {
                    std::string cityA = a.contains("city") ? toLower(a["city"]) : "";
                    std::string cityB = b.contains("city") ? toLower(b["city"]) : "";

                    bool aMatches = (cityA.find(targetCity) != std::string::npos);
                    bool bMatches = (cityB.find(targetCity) != std::string::npos);

                    // If A matches and B doesn't, A comes first (return true)
                    if (aMatches && !bMatches) return true;
                    // If B matches and A doesn't, B comes first (return false)
                    if (!aMatches && bMatches) return false;
                    // Otherwise keep original order
                    return false;
                });
        }

        return result;

    } catch (...) {
        return {{"error", "Invalid JSON from KVV Search"}};
    }
}

// --- Helper: Fetch Departures ---
json fetchDeparturesKVV(const std::string& stopId) {
    cpr::Response r = cpr::Get(
        cpr::Url{KVV_DM_URL},
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

    if (r.status_code != 200) return {{"error", "Upstream KVV error"}, {"code", r.status_code}};

    try {
        return json::parse(r.text);
    } catch (...) {
        return {{"error", "Invalid JSON from KVV"}};
    }
}

// --- Helper: Normalize Data (Detailed Mode) ---
json normalizeResponse(const json& kvvData, bool detailed = false) {
    json result = json::array();

    if (!kvvData.contains("departureList")) return result;

    for (const auto& dep : kvvData["departureList"]) {
        json item;

        // Line Info
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            // DETAILED MODE: Extract barrier-free/accessibility info
            if (detailed && dep["servingLine"].contains("hints")) {
                bool hasWheelchairAccess = false;
                bool hasLowFloor = false;

                for (const auto& hint : dep["servingLine"]["hints"]) {
                    std::string hintText = hint.value("hint", "");

                    if (hintText.find("Niederflur") != std::string::npos ||
                        hintText.find("low floor") != std::string::npos ||
                        hintText.find("lowFloor") != std::string::npos) {
                        hasLowFloor = true;
                    }

                    if (hintText.find("Rollstuhl") != std::string::npos ||
                        hintText.find("wheelchair") != std::string::npos ||
                        hintText.find("barrierefrei") != std::string::npos ||
                        hintText.find("barrier-free") != std::string::npos) {
                        hasWheelchairAccess = true;
                    }
                }
                item["wheelchair_accessible"] = hasWheelchairAccess || hasLowFloor;
                item["low_floor"] = hasLowFloor;
            }

            // DETAILED MODE: Extract train composition/length
            if (detailed) {
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

        // Platform / Track Info
        if (dep.contains("platform")) {
            item["platform"] = dep.value("platform", "");
        } else if (dep.contains("platformName")) {
            item["platform"] = dep.value("platformName", "");
        } else {
            item["platform"] = "Unknown";
        }

        // Time / Countdown
        if (dep.contains("countdown")) {
            item["minutes_remaining"] = std::stoi(dep.value("countdown", "0"));
        } else {
            item["minutes_remaining"] = 0;
        }

        // Realtime
