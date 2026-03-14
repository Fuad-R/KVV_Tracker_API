#pragma once

#include "crow.h"
#include "../db/database.h"

void registerNotificationsRoutes(crow::SimpleApp& app, Database& db);
