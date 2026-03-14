#pragma once

#include "crow.h"
#include "../db/database.h"

void registerDeparturesRoutes(crow::SimpleApp& app, Database& db);
