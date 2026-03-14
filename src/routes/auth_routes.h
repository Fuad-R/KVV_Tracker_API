#pragma once

#include "crow.h"
#include "../db/database.h"

void registerAuthRoutes(crow::SimpleApp& app, Database& db);
