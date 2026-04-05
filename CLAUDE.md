# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Transit Tracker API — a C++17 HTTP service that aggregates real-time public transit departure data from EFA-compatible upstream providers (default: Karlsruhe/KVV). Built with Crow, cpr, nlohmann/json, backed by PostgreSQL + PostGIS for stop persistence and API key auth.

## Code Architecture

All source lives under `src/`:

```
src/
  main.cpp                  — Entry point: initializes auth, DB, registers routes, starts Crow
  config/config.h           — Central constants, cache struct, DB config, input validation, JSON utils
  db/database.h/.cpp        — PostgreSQL connection, stop persistence, nearby-stop queries, API key lookup
  middleware/api_key_auth.h/.cpp  — Crow middleware that hashes + validates X-API-Key header
  models/api_key.h/.cpp     — API key data model
  services/
    auth_service.h/.cpp          — Auth initialization, key management
    stops_service.h/.cpp         — Upstream stop search, DB persistence, nearby stops
    departures_service.h/.cpp    — Upstream departure fetch with in-memory caching
    notifications_service.h/.cpp — Multi-provider notification aggregation
  routes/
    auth_routes.h/.cpp           — /health, /api/key (create API keys)
    stops_routes.h/.cpp          — /api/stops/search, /api/stops/nearby
    departures_routes.h/.cpp     — /api/stops/{stopId}
    notifications_routes.h/.cpp  — /api/current_notifs
```

**Source Priority**: When docs and code disagree, trust `CMakeLists.txt`, `Dockerfile`, and `src/*` over prose docs. `AGENTS.md` contains valuable gotchas — read it before making changes.

## Build & Run

### Prerequisites (Ubuntu/Debian)
```
cmake build-essential git libssl-dev libpq-dev postgresql-client libcurl4-openssl-dev
```

### Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`CMakeLists.txt` fetches Asio, Crow, nlohmann/json, and cpr via `FetchContent` on first configure (requires network).

### Run
```bash
./build/kvv_aggregator
```

Server starts on port **8080** by default. Override with `APP_PORT` env var.

### Docker
```bash
docker build -t transit-tracker .
docker run -p 8080:8080 --env-file .env transit-tracker
```

### Verify
```bash
curl http://localhost:8080/health
# Expected: {"status":"ok"}
```

## Runtime Configuration

| Variable | Description |
|---|---|
| `APP_PORT` | HTTP port (default 8080) |
| `AUTH` | Set to `True`/`true`/`1`/`yes` to enable API key auth |
| `API_KEY` | Fallback API key when no DB is configured |
| `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, `DB_PASSWORD`, `DB_SSLMODE` | PostgreSQL connection |
| Upstream provider URLs | Hardcoded in `src/config/config.h` (`Provider_DM_URL`, `Provider_SEARCH_URL`) |

**DB config load order**: env vars → `./db_connection.txt` → `/config/db_connection.txt`.

## API Endpoints

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/health` | No | Health check |
| GET | `/api/stops/search?q=<query>` | When AUTH enabled | Search stops by name |
| GET | `/api/stops/nearby?lat=&long=` | When AUTH enabled | Nearby stops (requires PostGIS + DB) |
| GET | `/api/stops/{stopId}` | When AUTH enabled | Real-time departures |
| GET | `/api/current_notifs?stopID=` | When AUTH enabled | Service notifications |

See `docs/API_DOCUMENTATION.md` for full endpoint details.

## Important Behavior Notes

- **Stop search** `city` and `location` query params are accepted by the route but currently **ignored** by `stops_service.cpp` when building the upstream request.
- **Nearby stops** requires PostGIS (`GEOGRAPHY(Point, 4326)`, `ST_DWithin`, `ST_Distance`). Returns `503` without DB config.
- **Departure cache** is in-memory only, 30s TTL, max 10,000 entries. Cache key = `(stopId, detailed, delay)`. The `track` filter runs post-cache.
- **Notifications endpoint** parameter is `stopID` (case-sensitive, not `stopid`). Not cached.
- **No test framework** exists in the repo. Meaningful verification = successful build + health check.

## External Dependencies

- **Crow v1.2.0** — HTTP/API framework (header-only, no Boost)
- **Asio standalone v1.30.2** — Networking (required by Crow)
- **nlohmann/json v3.11.3** — JSON parsing
- **cpr v1.10.5** — HTTP client for upstream requests (wraps libcurl)
- **PostgreSQL (libpq)** — Database connectivity
- **OpenSSL** — SHA-256 hashing for API key storage
