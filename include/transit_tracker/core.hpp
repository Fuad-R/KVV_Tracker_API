#pragma once

#include <chrono>
#include <ctime>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace transit_tracker {

using json = nlohmann::json;

struct CacheEntry {
    json data;
    std::chrono::steady_clock::time_point timestamp;
};

struct DbConfig {
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
    std::string sslmode;
};

struct StopRecord {
    std::string stop_id;
    std::optional<std::string> local_id;
    std::string stop_name;
    std::string city;
    std::optional<std::string> mot_array;
    double latitude;
    double longitude;
};

using RequestParameters = std::vector<std::pair<std::string, std::string>>;

inline constexpr int kCacheTtlSeconds = 30;
inline constexpr size_t kMaxCacheEntries = 10000;
inline constexpr long kUpstreamTimeoutSeconds = 15;
inline constexpr size_t kMaxQueryLength = 200;
inline constexpr size_t kMaxStopIdLength = 100;

extern const std::string kProviderDmUrl;
extern const std::string kProviderSearchUrl;
extern const std::string kDbConfigPath;
extern const std::string kDbConfigContainerPath;
extern const std::vector<std::string> kNotificationApiProviders;

bool isValidStopId(const std::string& stop_id);
bool isValidSearchQuery(const std::string& query);

std::string toLower(const std::string& input);
std::string trim(const std::string& input);

std::optional<std::string> jsonToString(const json& value);
std::optional<double> jsonToDouble(const json& value);
std::optional<std::string> getJsonString(const json& obj, std::initializer_list<const char*> keys);

std::string formatDouble(double value);
std::string sha256Hex(const std::string& input);
bool constantTimeEquals(const std::string& a, const std::string& b);

std::optional<DbConfig> loadDbConfig(const std::string& path);

RequestParameters buildSearchStopRequestParameters(const std::string& query);
RequestParameters buildDepartureRequestParameters(const std::string& stop_id);
RequestParameters buildNotificationRequestParameters(const std::string& stop_id);

std::string buildCacheKey(const std::string& stop_id, bool detailed, bool include_delay);
bool matchesTrackFilter(const std::string& platform, const std::string& requested_track);
json filterDeparturesByTrack(const json& departures, const std::string& requested_track);
void evictExpiredCacheEntries(
    std::map<std::string, CacheEntry>& cache,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

std::optional<std::string> buildMotArray(const json& stop);
bool extractCoordinates(const json& stop, double& lat, double& lon);
std::optional<StopRecord> parseStopRecord(const json& stop);
std::vector<StopRecord> extractStopRecords(const json& search_result);

json normalizeResponse(const json& provider_data, bool detailed = false, bool include_delay = false);

std::optional<std::time_t> parseISO8601(const std::string& timestamp);
bool isCurrentlyValid(const json& validity, std::time_t now);
bool isCurrentlyValid(const json& validity);
bool isStopAffected(const json& info, const std::string& stop_id);
json extractNotificationsFromResponses(const std::string& stop_id, const std::vector<json>& responses);

}  // namespace transit_tracker
