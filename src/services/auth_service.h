#pragma once

#include <string>
#include "../db/database.h"

std::string sha256Hex(const std::string& input);
bool constantTimeEquals(const std::string& a, const std::string& b);
bool validateKeyViaDatabase(const std::string& providedKey, const Database& db);
