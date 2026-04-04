#pragma once

#include <memory>
#include <string>

#include "transit_tracker/app.hpp"

namespace transit_tracker {

void initializeAuthFromEnvironment(AppState& state);
std::shared_ptr<AppState> createProductionState();

json searchStopsProvider(const std::string& query, const std::string& city = "", bool include_location = false);
json fetchDeparturesProvider(const std::string& stop_id);
json fetchNotificationsFromProvider(const std::string& base_url, const std::string& stop_id);

void ensureStopsInDatabase(AppState& state, const json& search_result, const std::string& original_search);
bool validateKeyViaDatabase(AppState& state, const std::string& provided_key);

AppDependencies createProductionDependencies(const std::shared_ptr<AppState>& state);

}  // namespace transit_tracker
