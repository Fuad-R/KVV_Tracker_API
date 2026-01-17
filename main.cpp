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

// --- Helper: Search Stops ---
json searchStopsKVV(const std::string& query) {
    cpr::Response r = cpr::Get(
        cpr::Url{KVV_SEARCH_URL},
        cpr::Parameters{
            {"outputFormat", "JSON"},
            {"type_sf", "any"},
            {"name_sf", query},
            {"anyObjFilter_sf", "2"}
        }
    );

    if (r.status_code != 200) return {{"error", "Upstream Error"}};

    try {
        json raw = json::parse(r.text);
        json result = json::array();

        if (raw.contains("stopFinder") && raw["stopFinder"].contains("points")) {
            auto& points = raw["stopFinder"]["points"];
            if (points.is_array()) {
                for (const auto& p : points) {
                    if (p.contains("stateless")) {
                        result.push_back({
                            {"id", p.value("stateless", "")},
                            {"name", p.value("name", "Unknown")}
                        });
                    }
                }
            } else if (points.is_object()) {
                 result.push_back({
                    {"id", points.value("stateless", "")},
                    {"name", points.value("name", "Unknown")}
                });
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

// --- Helper: Normalize Data ---
json normalizeResponse(const json& kvvData) {
    json result = json::array();
    if (!kvvData.contains("departureList")) return result;

    for (const auto& dep : kvvData["departureList"]) {
        json item;

        // Line Info
        if (dep.contains("servingLine")) {
            item["line"] = dep["servingLine"].value("number", "?");
            item["direction"] = dep["servingLine"].value("direction", "Unknown");
        }

        // Platform / Track Info (NEW)
        // KVV often uses "platform" for the number (e.g. "1") or "platformName" (e.g. "Gleis 1")
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

        result.push_back(item);
    }
    return result;
}

int main() {
    crow::SimpleApp app;

    // Endpoint 1: Search
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req){
        auto query = req.url_params.get("q");
        if (!query) return crow::response(400, "Missing 'q' parameter");
        return crow::response(searchStopsKVV(std::string(query)).dump());
    });

    // Endpoint 2: Get Departures (Updated with Track Filter)
    CROW_ROUTE(app, "/api/stops/<string>")
    ([](const crow::request& req, const std::string& stopId){

        // 1. Get/Refresh Cache (Always fetch ALL tracks for the cache)
        json allDepartures;
        bool cacheHit = false;

        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = stop_cache.find(stopId);
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

            allDepartures = normalizeResponse(rawData);

            // Update Cache
            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                stop_cache[stopId] = {allDepartures, std::chrono::steady_clock::now()};
            }
        }

        // 2. Handle Track Filter
        // Check if user requested specific track via ?track=...
        const char* requestedTrack = req.url_params.get("track");

        if (requestedTrack) {
            json filteredDepartures = json::array();
            std::string reqTrackStr = std::string(requestedTrack);

            for (const auto& dep : allDepartures) {
                // Check if the platform matches the request
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
