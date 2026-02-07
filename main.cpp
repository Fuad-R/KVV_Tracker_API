#include "crow.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

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
const std::string KVV_DM_URL     = "https://projekte.kvv-efa.de/sl3-alone/XSLT_DM_REQUEST";
const std::string KVV_SEARCH_URL = "https://projekte.kvv-efa.de/sl3-alone/XSLT_STOPFINDER_REQUEST";

// --- Helpers ---
std::string toLower(const std::string& str) {
    std::string s = str;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool parseBoolParam(const char* v) {
    if (!v) return false;
    std::string s = toLower(std::string(v));
    return (s == "true" || s == "1" || s == "yes");
}

static int parseIntLoose(const json& v, int fallback = -1) {
    try {
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_number()) return static_cast<int>(v.get<double>());
        if (v.is_string()) return std::stoi(v.get<std::string>());
    } catch (...) {
    }
    return fallback;
}

static int getIntFieldLoose(const json& obj, const std::initializer_list<const char*>& keys, int fallback = -1) {
    for (const char* k : keys) {
        if (obj.contains(k)) return parseIntLoose(obj.at(k), fallback);
    }
    return fallback;
}

static bool getBoolFieldLoose(const json& obj, const std::initializer_list<const char*>& keys, bool fallback = false) {
    for (const char* k : keys) {
        if (!obj.contains(k)) continue;
        const auto& v = obj.at(k);
        try {
            if (v.is_boolean()) return v.get<bool>();
            if (v.is_number_integer()) return v.get<int>() != 0;
            if (v.is_string()) {
                std::string s = toLower(v.get<std::string>());
                return (s == "true" || s == "1" || s == "yes");
            }
        } catch (...) {
        }
    }
    return fallback;
}

// --- Stop search ---
// - city is a preference: passed upstream as anyResSort_sf=city (per PDF).
// - returns match_quality + is_best.
// - stable-sort by match_quality (desc) so upstream ordering (incl. anyResSort_sf) remains within ties.
json searchStopsKVV(const std::string& query, const std::string& city = "", bool includeLocation = false) {
    std::string wildCardQuery = query;
    if (wildCardQuery.empty()) return json::array();
    if (wildCardQuery.back() != '*') wildCardQuery += "*";

    cpr::Response r;
    if (city.empty()) {
        r = cpr::Get(
            cpr::Url{KVV_SEARCH_URL},
            cpr::Parameters{
                {"outputFormat", "JSON"},
                {"coordOutputFormat", "WGS84[dd.ddddd]"},
                {"locationServerActive", "1"},
                {"type_sf", "any"},
                {"name_sf", wildCardQuery},
                {"anyObjFilter_sf", "2"},
                {"anyMaxSizeHitList", "100"}
            }
        );
    } else {
        r = cpr::Get(
            cpr::Url{KVV_SEARCH_URL},
            cpr::Parameters{
                {"outputFormat", "JSON"},
                {"coordOutputFormat", "WGS84[dd.ddddd]"},
                {"locationServerActive", "1"},
                {"type_sf", "any"},
                {"name_sf", wildCardQuery},
                {"anyObjFilter_sf", "2"},
                {"anyMaxSizeHitList", "100"},
                {"anyResSort_sf", city}
            }
        );
    }

    if (r.status_code != 200) return {{"error", "Upstream Error"}};

    try {
        json raw = json::parse(r.text);
        json result = json::array();

        if (raw.contains("stopFinder") && raw["stopFinder"].contains("points")) {
            const auto& points = raw["stopFinder"]["points"];

            auto processPoint = [&](const json& p) {
                if (!p.contains("stateless")) return;

                int matchQ = getIntFieldLoose(p, {"matchQuality", "matchquality", "quality"}, -1);
                bool isBest = getBoolFieldLoose(p, {"isBest", "isbest"}, false);

                json item = {
                    {"id", p.value("stateless", "")},
                    {"name", p.value("name", "Unknown")},
                    {"match_quality", matchQ},
                    {"is_best", isBest}
                };

                if (p.contains("place")) item["city"] = p.value("place", "");

                if (includeLocation && p.contains("ref") && p["ref"].contains("coords")) {
                    item["coordinates"] = p["ref"].value("coords", "");
                }

                result.push_back(std::move(item));
            };

            if (points.is_array()) {
                for (const auto& p : points) processPoint(p);
            } else if (points.is_object()) {
                processPoint(points);
            }
        }

        std::stable_sort(result.begin(), result.end(),
            [&](const json& a, const json& b) {
                int qa = a.value("match_quality", -1);
                int qb = b.value("match_quality", -1);
                if (qa != qb) return qa > qb;
                return false; // preserve upstream order for ties
            }
        );

        // Infer is_best for top match_quality group if upstream didn't mark any.
        if (!result.empty()) {
            bool anyMarked = false;
            for (const auto& item : result) {
                if (item.value("is_best", false)) { anyMarked = true; break; }
            }

            int topQ = result[0].value("match_quality", -1);
            if (!anyMarked && topQ >= 0) {
                for (auto& item : result) {
                    item["is_best"] = (item.value("match_quality", -1) == topQ);
                }
            }
        }

        return result;
    } catch (...) {
        return {{"error", "Invalid JSON from KVV Search"}};
    }
}

// --- Departures fetch ---
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

// --- Normalize departures ---
json normalizeResponse(const json& kvvData, bool detailed = false, bool includeDelay = false) {
    json result = json::array();
    if (!kvvData.contains("departureList") || !kvvData["departureList"].is_array()) return result;

    auto strToBool = [](const std::string& v) {
        std::string s = toLower(v);
        return (s == "1" || s == "true" || s == "yes");
    };

    auto toIntSafe = [](const json& v, int fallback = 0) -> int {
        try {
            if (v.is_number_integer()) return v.get<int>();
            if (v.is_number()) return static_cast<int>(v.get<double>());
            if (v.is_string()) return std::stoi(v.get<std::string>());
        } catch (...) {}
        return fallback;
    };

    for (const auto& dep : kvvData["departureList"]) {
        json item;

        if (dep.contains("servingLine") && dep["servingLine"].is_object()) {
            const auto& sl = dep["servingLine"];

            item["line"] = sl.value("number", "?");
            item["direction"] = sl.value("direction", "Unknown");

            if (sl.contains("motType")) item["mot"] = toIntSafe(sl["motType"], -1);
            else item["mot"] = -1;

            if (includeDelay && sl.contains("delay")) {
                item["delay_minutes"] = toIntSafe(sl["delay"], 0);
            }

            if (detailed) {
                bool hasPlanLowFloor = false, hasPlanWheelchair = false;
                bool planLowFloor = false, planWheelchair = false;

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

                bool hintLowFloor = false, hintWheelchair = false;
                if (sl.contains("hints") && sl["hints"].is_array()) {
                    for (const auto& h : sl["hints"]) {
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

                if (sl.contains("trainType")) item["train_type"] = sl.value("trainType", "");
                if (sl.contains("trainLength")) item["train_length"] = sl.value("trainLength", "");
                else if (sl.contains("trainComposition")) item["train_composition"] = sl.value("trainComposition", "");
            }
        } else {
            item["line"] = "?";
            item["direction"] = "Unknown";
            item["mot"] = -1;
            if (includeDelay) item["delay_minutes"] = 0;
        }

        if (dep.contains("platform")) item["platform"] = dep.value("platform", "");
        else if (dep.contains("platformName")) item["platform"] = dep.value("platformName", "");
        else item["platform"] = "Unknown";

        if (dep.contains("countdown")) item["minutes_remaining"] = toIntSafe(dep["countdown"], 0);
        else item["minutes_remaining"] = 0;

        item["is_realtime"] = dep.contains("realDateTime");

        if (detailed && dep.contains("hints") && dep["hints"].is_array()) {
            json hintsArray = json::array();
            for (const auto& h : dep["hints"]) {
                std::string txt = h.value("hint", h.value("content", ""));
                if (!txt.empty()) hintsArray.push_back(txt);
            }
            if (!hintsArray.empty()) item["hints"] = hintsArray;
        }

        result.push_back(std::move(item));
    }

    return result;
}

int main() {
    crow::SimpleApp app;

    // Search Endpoint
    CROW_ROUTE(app, "/api/stops/search")
    ([](const crow::request& req) {
        const char* query = req.url_params.get("q");
        const char* city  = req.url_params.get("city");
        const char* locationParam = req.url_params.get("location");

        bool includeLocation = parseBoolParam(locationParam);

        if (!query) return crow::response(400, "Missing 'q' parameter");

        json out = searchStopsKVV(std::string(query),
                                  city ? std::string(city) : "",
                                  includeLocation);

        crow::response res(out.dump());
        res.add_header("Content-Type", "application/json");
        return res;
    });

    // Departures Endpoint
    CROW_ROUTE(app, "/api/stops/<string>")
    ([](const crow::request& req, std::string stopId) {
        bool detailed = parseBoolParam(req.url_params.get("detailed"));
        bool includeDelay = parseBoolParam(req.url_params.get("delay"));

        std::string cacheKey = stopId +
            (detailed ? "_detailed" : "") +
            (includeDelay ? "_delay" : "");

        json allDepartures;
        bool cacheHit = false;

        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = stop_cache.find(cacheKey);
            if (it != stop_cache.end()) {
                auto now = std::chrono::steady_clock::now();
                auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();
                if (age < CACHE_TTL_SECONDS) {
                    allDepartures = it->second.data;
                    cacheHit = true;
                }
            }
        }

        if (!cacheHit) {
            json rawData = fetchDeparturesKVV(stopId);
            if (rawData.contains("error")) {
                crow::response res(502, rawData.dump());
                res.add_header("Content-Type", "application/json");
                return res;
            }

            allDepartures = normalizeResponse(rawData, detailed, includeDelay);

            {
                std::lock_guard<std::mutex> lock(cache_mutex);
                stop_cache[cacheKey] = {allDepartures, std::chrono::steady_clock::now()};
            }
        }

        const char* requestedTrack = req.url_params.get("track");
        if (requestedTrack) {
            std::string reqTrackStr = std::string(requestedTrack);
            json filtered = json::array();

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

                if (match) filtered.push_back(dep);
            }

            crow::response res(filtered.dump());
            res.add_header("Content-Type", "application/json");
            return res;
        }

        crow::response res(allDepartures.dump());
        res.add_header("Content-Type", "application/json");
        return res;
    });

    app.port(8080).multithreaded().run();
    return 0;
}
