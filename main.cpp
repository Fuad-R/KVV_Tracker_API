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

// --- Helper: Search Stops ---
json searchStopsKVV(const std::string& query, const std::string& city = "") {
    std::string wildCardQuery = query;
    if (wildCardQuery.empty()) return json::array();

    // Append wildcard for partial matching if not present
    if (wildCardQuery.back() != '*') {
        wildCardQuery += "*";
    }

    // Configure params (Server-side city filter removed to prevent empty results)
    cpr::Parameters params{
        {"outputFormat", "JSON"},
        {"type_sf", "stop"},
        {"name_sf", wildCardQuery},
        {"anyObjFilter_sf", "2"},      // Stop filter
        {"anyMaxSizeHitList", "100"}   // High limit to catch regional results
    };

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
                    result.push_back(item);
                }
            };

            if (points.is_array()) {
                for (const auto& p : points) processPoint(p);
            } else if (points.is_object()) {
                processPoint(points);
            }
        }

        // Client-Side Sorting: Prioritize requested city
        if (!city.empty()) {
            std::string targetCity = toLower(city);
            std::stable_sort(result.begin(), result.end(),
                [&](const json& a, const json& b) {
                    std::string cityA = a.contains("city") ? toLower(a["city"]) : "";
                    std::string cityB = b.contains("city") ? toLower(b["city"]) : "";

                    bool aMatches = (cityA.find(targetCity) != std::string::npos);
                    bool bMatches = (cityB.find(targetCity) != std::string::npos);

                    if (aMatches && !bMatches) return true;
                    if (!aMatches && bMatches) return false;
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

// --- Helper: Normalize Data ---
json normalizeResponse(const json& kvvData, bool detailed = false) {
    json result = json::array();

    if (!kvvData.contains("departureList")) return result;

    for (const auto& dep : kvvData["departureList"]) {
        json item;

        // Line Info
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            // Hints inside servingLine (Accessibility)
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

            // Train Details
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

        // Platform
        if (dep.contains("platform")) {
            item["platform"] = dep.value("platform", "");
        } else if (dep.contains("platformName")) {
            item["platform"] = dep.value("platformName", "");
        } else {
            item["platform"] = "Unknown";
        }

        // Countdown
        if (dep.contains("countdown")) {
            item["minutes_remaining"] = std::stoi(dep.value("countdown", "0"));
        } else {
            item["minutes_remaining"] = 0;
        }

        // Realtime
        if (dep.contains("realDateTime")) {
            item["is_realtime"] = true;
        } else {
            item["is_realtime"] = false;
        }

        // Top-level hints
        if (detailed && dep.contains("hints")) {
            json hintsArray = json::array();
            for (const auto& hint : dep["hints"]) {
                if (hint.contains("hint")) {
                    hintsArray.push_back(hint.value("hint", ""));
                }
            }
            if (!hintsArray.empty()) {
                item["hints"] = hintsArray;
            }
        }

        result.push_back(item);
    }
    return result;
}

int main() {
    crow::SimpleApp app;

    // Search Endpoint
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req){
        auto query = req.url_params.get("q");
        auto city = req.url_params.get("city");

        if (!query) return crow::response(400, "Missing 'q' parameter");

        return crow::response(searchStopsKVV(std::string(query), city ? std::string(city) : "").dump());
    });

    // Departures Endpoint
    CROW_ROUTE(app, "/api/stops/<string>")
    ([](const crow::request& req, std::string stopId){
        const char* detailedParam = req.url_params.get("detailed");
        bool detailed = (detailedParam && (std::string(detailedParam) == "true" || std::string(detailedParam) == "1"));

        // Cache Check
        json allDepartures;
        bool cacheHit = false;
        std::string cacheKey = stopId + (detailed ? "_detailed" : "");

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

        // Fetch if not cached
        if (!cacheHit) {
            json rawData = fetchDeparturesKVV(stopId);
            if (rawData.contains("error")) return crow::response(502, rawData.dump());

            allDepartures = normalizeResponse(rawData, detailed);

            std::lock_guard<std::mutex> lock(cache_mutex);
            stop_cache[cacheKey] = {allDepartures, std::chrono::steady_clock::now()};
        }

        // Track Filter
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
                    if (!isdigit(platform[reqTrackStr.size()])) {
                        match = true;
                    }
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
