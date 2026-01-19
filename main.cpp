#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <mutex>
#include <chrono>

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

// --- Helper: Search Stops (Updated with City Filter) ---
json searchStopsKVV(const std::string& query, const std::string& city = "") {
    // Start with base parameters
    cpr::Parameters params{
            {"outputFormat", "JSON"},
            {"type_sf", "stop"},      // Ensure we only search for stops (from previous fix)
            {"name_sf", query},
            {"anyObjFilter_sf", "2"}
    };

    // If a city is provided, add it to the request
    if (!city.empty()) {
        params.Add({"place_sf", city});
    }

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

            // Helper lambda to process a single point
            auto processPoint = [&](const json& p) {
                if (p.contains("stateless")) {
                    json item = {
                        {"id", p.value("stateless", "")},
                        {"name", p.value("name", "Unknown")}
                    };
                    // Optional: Include city/place name in response if available
                    if (p.contains("place")) {
                        item["city"] = p.value("place", "");
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
            {"limit", "40"} // Increased limit to ensure we get data for all tracks
        }
    );

    if (r.status_code != 200) return {{"error", "Upstream KVV error"}, {"code", r.status_code}};

    try {
        return json::parse(r.text);
    } catch (...) {
        return {{"error", "Invalid JSON from KVV"}};
    }
}

// --- Helper: Normalize Data (Updated with Detailed Mode) ---
json normalizeResponse(const json& kvvData, bool detailed = false) {
    json result = json::array();
    if (!kvvData.contains("departureList")) return result;

    for (const auto& dep : kvvData["departureList"]) {
        json item;

        // Line Info
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");

            // DETAILED MODE: Extract barrier-free/accessibility info from servingLine
            if (detailed && dep["servingLine"].contains("hints")) {
                bool hasWheelchairAccess = false;
                bool hasLowFloor = false;

                for (const auto& hint : dep["servingLine"]["hints"]) {
                    std::string hintText = hint.value("hint", "");
                    // Check for wheelchair/barrier-free indicators
                    // Common hint texts: "Niederflur", "Rollstuhlgerecht", "wheelchairAccessible"
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

            // DETAILED MODE: Extract train composition/length if available
            if (detailed) {
                // Some EFA systems provide "trainType" or "motType" (means of transport type)
                if (dep["servingLine"].contains("trainType")) {
                    item["train_type"] = dep["servingLine"].value("trainType", "");
                }

                // Train length/composition might be in properties or servingLine
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

        // Realtime Flag
        if (dep.contains("realDateTime")) {
            item["is_realtime"] = true;
        } else {
            item["is_realtime"] = false;
        }

        // DETAILED MODE: Extract additional hints at the departure level
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

    // Endpoint 1: Search (Updated to accept 'city' param)
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req){
        auto query = req.url_params.get("q");
        auto city = req.url_params.get("city"); // Get optional city parameter

        if (!query) return crow::response(400, "Missing 'q' parameter");

        // Pass the city (or empty string if null) to the helper
        return crow::response(searchStopsKVV(std::string(query), city ? std::string(city) : "").dump());
    });

    // Endpoint 2: Get Departures (Updated with Track Filter and Detailed Mode)
    CROW_ROUTE(app, "/api/stops/<string>")
    ([](const crow::request& req, const std::string& stopId){

        // Check if detailed mode is requested
        const char* detailedParam = req.url_params.get("detailed");
        bool detailed = (detailedParam && (std::string(detailedParam) == "true" || std::string(detailedParam) == "1"));

        // 1. Get/Refresh Cache (Always fetch ALL tracks for the cache)
        json allDepartures;
        bool cacheHit = false;

        // Create cache key that includes detailed flag
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

        if (!cacheHit) {
            json rawData = fetchDeparturesKVV(stopId);
            if (rawData.contains("error")) return crow::response(502, rawData.dump());

            allDepartures = normalizeResponse(rawData, detailed);

            // Update Cache
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                stop_cache[cacheKey] = {allDepartures, std::chrono::steady_clock::now()};
            }
        }

        // 2. Handle Track Filter
        const char* requestedTrack = req.url_params.get("track");

        if (requestedTrack) {
            json filteredDepartures = json::array();
            std::string reqTrackStr = std::string(requestedTrack);

            for (const auto& dep : allDepartures) {
                if (dep.value("platform", "") == reqTrackStr) {
                    filteredDepartures.push_back(dep);
                }
            }
            return crow::response(filteredDepartures.dump());
        }

        // If no filter, return everything
        return crow::response(allDepartures.dump());
    });

    app.port(8080).multithreaded().run();
}
