#pragma once

#include "crow.h"
#include "../db/database.h"

void initAuth();
bool isAuthenticated(const crow::request& req, const Database& db);
crow::response unauthorizedResponse();
void setSecurityHeaders(crow::response& res);
