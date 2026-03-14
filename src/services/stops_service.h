#pragma once

#include "../config/config.h"
#include "../db/database.h"
#include <vector>

struct StopRecord {
    std::string stop_id;
    std::optional<std::string> local_id;
    std::string stop_name;
    std::string city;
    std::optional<std::string> mot_array;
    double latitude;
    double longitude;
};

std::optional<std::string> buildMotArray(const json& stop);
bool extractCoordinates(const json& stop, double& lat, double& lon);
std::optional<StopRecord> parseStopRecord(const json& stop);
std::vector<StopRecord> extractStopRecords(const json& searchResult);
void ensureStopsInDatabase(const json& searchResult, const std::string& originalSearch, const Database& db);
json searchStopsProvider(const std::string& query, const std::string& city = "", bool includeLocation = false);
