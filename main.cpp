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
// Accepts 'city' parameter but ignores it (no-op)
json searchStopsKVV(const std::string& query, const std::string& city = "", bool includeLocation = false) {
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
        cpr::Url{KVV_SEARCH_URL},
        params
    );

    if (r.status_code != 200) return {{"error", "Upstream Error"}};

    try {
        return json::parse(r.text);
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