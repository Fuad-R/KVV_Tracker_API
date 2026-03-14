#pragma once

#include "crow.h"
#include "../db/database.h"

void registerStopsRoutes(crow::SimpleApp& app, Database& db);
