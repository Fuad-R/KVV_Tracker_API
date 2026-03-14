#pragma once

#include "crow.h"
#include "../db/database.h"

void registerVehiclesRoutes(crow::SimpleApp& app, Database& db);
