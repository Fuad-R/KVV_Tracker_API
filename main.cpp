#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>
#include <map>
#include <mutex>
#include <chrono>
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

// --- Configuration ---
const std::string KVV_DM_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
const std::string KVV_SEARCH_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";
const std::string DEFAULT_PRIORITY_REGION = "karlsruhe"; // Fallback priority if no city is requested

// --- Helper: String Utilities ---
std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

// --- Helper: Search Stops ---
// Updated signature to accept includeLocation
json searchStopsKVV(const std::string& query, const std::string& city = "", bool includeLocation = false) {
    std::string wildCardQuery = query;
    if (wildCardQuery.empty()) return json::array();
    if (wildCardQuery.back() != '*') {
        wildCardQuery += "*";
    }

    cpr::Parameters params{
        {"outputFormat", "rapidJSON"},
        {"type_sf", "stop"},
        {"name_sf", wildCardQuery},
        {"anyObjFilter_sf", "2"},
        {"anyMaxSizeHitList", "100"},
        // Request standard GPS coordinates (Lon/Lat) instead of raw projection
        {"coordOutputFormat", "WGS84[dd.ddddd]"}
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

                    if (p.contains("place")) {
                        item["city"] = p.value("place", "");
                    }

                    // --- NEW: Location Parsing ---
                    if (includeLocation && p.contains("ref") && p["ref"].contains("coords")) {
                        // Returns string "8.401...,49.005..." (Lon,Lat)
                        item["coordinates"] = p["ref"].value("coords", "");
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

        // --- SORTING LOGIC ---
        std::string targetCity = city.empty() ? DEFAULT_PRIORITY_REGION : city;
        targetCity = toLower(targetCity);

        std::stable_sort(result.begin(), result.end(),
            [&](const json& a, const json& b) {
                std::string cityA = a.contains("city") ? toLower(a["city"]) : "";
                std::string nameA = a.contains("name") ? toLower(a["name"]) : "";
                std::string cityB = b.contains("city") ? toLower(b["city"]) : "";
                std::string nameB = b.contains("name") ? toLower(b["name"]) : "";

                bool aMatches = (cityA.find(targetCity) != std::string::npos) ||
                                (nameA.find(targetCity) != std::string::npos);
                bool bMatches = (cityB.find(targetCity) != std::string::npos) ||
                                (nameB.find(targetCity) != std::string::npos);

                if (aMatches && !bMatches) return true;
                if (!aMatches && bMatches) return false;
                return false;
            });

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
            {"outputFormat", "rapidJSON"},
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
json normalizeResponse(const json& kvvData, bool detailed = false, bool includeDelay = false) {
    json result = json::array();
    if (!kvvData.contains("departureList")) return result;

    auto strToBool = [](const std::string& v) {
        std::string s = v;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return (s == "1" || s == "true" || s == "yes");
    };

    for (const auto& dep : kvvData["departureList"]) {
        json item;

        // Line Info
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            // Vehicle Type (ALWAYS included)
            if (dep["servingLine"].contains("product")) {
                if (dep["servingLine"]["product"].contains("class")) {
                    item["vehicle_type_id"] = dep["servingLine"]["product"].value("class", -1);
                }
                if (dep["servingLine"]["product"].contains("name")) {
                    item["vehicle_type_name"] = dep["servingLine"]["product"].value("name", "Unknown");
                }
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
                // Accessibility parsing
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

    // Search Endpoint
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req){
        auto query = req.url_params.get("q");
        auto city = req.url_params.get("city");
        auto locationParam = req.url_params.get("location");

        bool includeLocation = (locationParam && (std::string(locationParam) == "true" || std::string(locationParam) == "1"));

        if (!query) return crow::response(400, "Missing 'q' parameter");

        return crow::response(searchStopsKVV(std::string(query), city ? std::string(city) : "", includeLocation).dump());
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
            json rawData = fetchDeparturesKVV(stopId);
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
                    if (!isdigit(platform[reqTrackStr.size()])) match = true;
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
