#pragma once

#include "../config/config.h"

json fetchDeparturesProvider(const std::string& stopId);
json normalizeResponse(const json& ProviderData, bool detailed = false, bool includeDelay = false);
json getDepartures(const std::string& stopId, bool detailed, bool includeDelay, const char* track);
json extractValidNotifications(const std::string& stopId);
