# Transit Tracker API — Developer Integration Guide

> Comprehensive API reference designed for building applications that consume real-time public transit data. This document covers every endpoint, parameter, response field, error condition, and integration pattern.

## Table of Contents

- [Base URL](#base-url)
- [Authentication](#authentication)
- [Response Format](#response-format)
- [Endpoints](#endpoints)
  - [Health Check](#1-health-check)
  - [Search Stops](#2-search-stops)
  - [Get Departures](#3-get-departures)
  - [Get Notifications](#4-get-notifications)
  - [Nearby Stops](#5-nearby-stops)
- [Data Types Reference](#data-types-reference)
  - [MOT Codes](#mot-codes-mode-of-transport)
- [Error Handling](#error-handling)
- [Caching Behavior](#caching-behavior)
- [Integration Walkthrough](#integration-walkthrough)
- [Code Examples](#code-examples)
  - [Python](#python)
  - [JavaScript](#javascript-fetch)
  - [cURL](#curl)
- [Common Patterns](#common-patterns)

---

## Base URL

```
http://<host>:8080
```

The API listens on port **8080** by default. Replace `<host>` with the server's IP address or domain name (e.g. `localhost` for local development).

---

## Authentication

Authentication is **optional** and controlled by the `AUTH` environment variable.

### When `AUTH` is not set or is not `True`

All endpoints are publicly accessible without API keys or tokens (default behavior).

### When `AUTH=True`

All endpoints **except `/health`** require a valid API key. Pass the key via the `X-API-Key` HTTP header:

```
X-API-Key: <your-api-key>
```

**Example:**

```bash
curl -H "X-API-Key: my-secret-key" http://localhost:8080/api/stops/search?q=Marktplatz
```

Requests with a missing or invalid API key receive a `401 Unauthorized` response:

```json
{"error": "Unauthorized. Invalid or missing API key."}
```

#### Database-backed keys (primary)

When a database connection is configured, API keys are validated against the `api_keys` table. The server computes the SHA-256 hash of the provided key and looks it up via the `key_hash` column. Keys that are revoked or past their `expires_at` timestamp are rejected. On successful authentication the `last_used_at` column is updated.

See the [Database Configuration](#database-configuration) section in the README for the full `api_keys` schema.

#### Environment variable fallback

If no database connection is available, the server falls back to comparing the provided key against the `API_KEY` environment variable.

**Environment Variables:**

| Variable  | Description                                      |
|-----------|--------------------------------------------------|
| `AUTH`    | Set to `True` to enable API key authentication   |
| `API_KEY` | Fallback API key used when no database is configured |

---

## Response Format

- All responses are **JSON** (`Content-Type: application/json`).
- Successful list responses return a **JSON array** `[...]`.
- Error responses return a **JSON object** with an `"error"` key: `{"error": "..."}`.
- All responses include the following security headers:
  - `X-Content-Type-Options: nosniff`
  - `X-Frame-Options: DENY`
  - `Content-Security-Policy: default-src 'none'`
  - `Cache-Control: no-store`

---

## Endpoints

### 1. Health Check

Check whether the API server is running. This endpoint **does not require authentication** even when `AUTH=True`.

| Property | Value |
|---|---|
| **URL** | `/health` |
| **Method** | `GET` |
| **Auth** | Not required |
| **Parameters** | None |

#### Response

**`200 OK`**
```json
{"status": "ok"}
```

---

### 2. Search Stops

Search for transit stops by name. Returns stop identifiers needed for the departures and notifications endpoints.

| Property | Value |
|---|---|
| **URL** | `/api/stops/search` |
| **Method** | `GET` |

#### Query Parameters

| Parameter | Required | Type | Description |
|---|---|---|---|
| `q` | **Yes** | string | Stop name or partial name to search. Max 200 characters. |
| `city` | No | string | City filter (reserved for future use). |
| `location` | No | string | Set to `true` or `1` to include geographic coordinates in the response. |

#### Response

**`200 OK`** — Returns the upstream EFA search response. The response is a JSON object containing stop data in the `stopFinder.points` array. Each stop object includes the stop's identifier and, when `location=true`, geographic coordinates.

**Example Request:**
```
GET /api/stops/search?q=Marktplatz&location=true
```

**Example Response:**
```json
{
  "stopFinder": {
    "points": [
      {
        "id": "de:08212:1",
        "name": "Karlsruhe, Marktplatz",
        "type": "stop",
        "coord": {
          "x": 49.00937,
          "y": 8.40390
        },
        "parent": {
          "name": "Karlsruhe",
          "type": "locality"
        },
        "properties": {
          "stopId": "7000001"
        },
        "modes": [3, 4, 5]
      }
    ]
  }
}
```

**Key fields in each stop object:**

| Field | Type | Description |
|---|---|---|
| `id` | string | Global stop ID (e.g. `"de:08212:1"`). Use this as `stopId` for departures. |
| `name` | string | Full stop name including city. |
| `properties.stopId` | string | Local numeric stop ID (e.g. `"7000001"`). Can also be used as a stop identifier. |
| `coord` | object | Geographic coordinates when `location=true`. `x` = latitude, `y` = longitude. |
| `parent.name` | string | City or locality name. |
| `modes` | array | Array of MOT (Mode of Transport) codes available at this stop. See [MOT Codes](#mot-codes-mode-of-transport). |

#### Errors

| Status | Body | Cause |
|---|---|---|
| `400` | `{"error": "Missing 'q' parameter"}` | The `q` query parameter was not provided. |
| `400` | `{"error": "Invalid search query"}` | The query is empty, exceeds 200 characters, or contains control characters. |
| `400` | `{"error": "Invalid city parameter"}` | The `city` value is invalid. |

---

### 3. Get Departures

Retrieve real-time departures from a specific stop. This is the primary endpoint for building departure boards.

| Property | Value |
|---|---|
| **URL** | `/api/stops/{stopId}` |
| **Method** | `GET` |

#### Path Parameters

| Parameter | Required | Type | Description |
|---|---|---|---|
| `stopId` | **Yes** | string | Stop identifier obtained from the [Search Stops](#2-search-stops) endpoint. Accepts both global IDs (e.g. `de:08212:1`) and local IDs (e.g. `7000001`). Max 100 characters. Allowed characters: `a-z A-Z 0-9 : _ . - (space)`. |

#### Query Parameters

| Parameter | Required | Type | Default | Description |
|---|---|---|---|---|
| `detailed` | No | string | off | Set to `true` or `1` to include accessibility and vehicle information. |
| `delay` | No | string | off | Set to `true` or `1` to include delay information for each departure. |
| `track` | No | string | (all) | Filter departures by platform/track number. Supports exact match, prefix match (e.g. `"1"` matches `"1a"`), and keyword match (e.g. `"1"` matches `"Gleis 1"`). |

#### Response

**`200 OK`** — JSON array of departure objects.

**Standard response (no optional parameters):**

```json
[
  {
    "line": "2",
    "direction": "Wolfartsweier Über ZKM",
    "mot": 4,
    "platform": "4",
    "minutes_remaining": 0,
    "is_realtime": true
  },
  {
    "line": "S5",
    "direction": "Pforzheim",
    "mot": 1,
    "platform": "3",
    "minutes_remaining": 5,
    "is_realtime": true
  }
]
```

**Detailed response (`detailed=true&delay=true`):**

```json
[
  {
    "line": "2",
    "direction": "Wolfartsweier Über ZKM",
    "mot": 4,
    "platform": "4",
    "minutes_remaining": 0,
    "is_realtime": true,
    "delay_minutes": 2,
    "low_floor": true,
    "wheelchair_accessible": true,
    "hints": ["Niederflurwagen"]
  }
]
```

#### Departure Object Fields

| Field | Type | Always Present | Description |
|---|---|---|---|
| `line` | string | Yes | Line number or name (e.g. `"2"`, `"S5"`, `"SEV"`). |
| `direction` | string | Yes | Destination / direction of travel. |
| `mot` | integer | Yes | Mode of transport code. See [MOT Codes](#mot-codes-mode-of-transport). `-1` if unknown. |
| `platform` | string | Yes | Platform or track designation (e.g. `"4"`, `"Gleis 1"`, `"Unknown"`). |
| `minutes_remaining` | integer | Yes | Minutes until departure. `0` means departing now. |
| `is_realtime` | boolean | Yes | `true` if the time is based on real-time data, `false` if scheduled. |
| `delay_minutes` | integer | Only when `delay=true` | Delay in minutes. `0` means on time. |
| `low_floor` | boolean | Only when `detailed=true` | `true` if the vehicle has a low-floor entry. |
| `wheelchair_accessible` | boolean | Only when `detailed=true` | `true` if wheelchair accessible. Always `true` when `low_floor` is `true`. |
| `train_type` | string | Only when `detailed=true` and available | Type of train (e.g. `"ICE"`, `"RE"`). |
| `train_length` | string | Only when `detailed=true` and available | Length of the train. |
| `train_composition` | string | Only when `detailed=true` and available | Train composition info (returned when `train_length` is not available). |
| `hints` | array of strings | Only when `detailed=true` and available | Additional information about the departure. |

#### Errors

| Status | Body | Cause |
|---|---|---|
| `400` | `{"error": "Invalid stop ID"}` | The stop ID is empty, too long, or contains invalid characters. |
| `502` | `{"error": "Upstream Provider error", "code": <status>}` | The upstream EFA provider returned a non-200 status code. |
| `502` | `{"error": "Invalid JSON from Provider"}` | The upstream provider returned invalid JSON. |

---

### 4. Get Notifications

Retrieve current service alerts and disruptions affecting a specific stop.

| Property | Value |
|---|---|
| **URL** | `/api/current_notifs` |
| **Method** | `GET` |

#### Query Parameters

| Parameter | Required | Type | Description |
|---|---|---|---|
| `stopID` | **Yes** | string | Stop identifier to check for notifications. Accepts both global IDs (e.g. `de:08212:107`) and local IDs (e.g. `7000107`). Note: this parameter is **case-sensitive** — it must be `stopID`, not `stopid`. |

#### Response

**`200 OK`** — JSON array of notification objects.

**Example Request:**
```
GET /api/current_notifs?stopID=7000107
```

**Example Response:**
```json
[
  {
    "id": "100003815_KVV_ICSKVV",
    "urlText": "Knielingen - Wörth: nächtliche Einschränkung wegen Bauarbeiten",
    "content": "An der Haltestelle <b>Kluftern</b> kommt es zu <b>Umleitungen</b>.",
    "subtitle": "Knielingen - Wörth: nächtliche Einschränkung wegen Bauarbeiten",
    "providerCode": "KVV",
    "priority": "normal"
  }
]
```

#### Notification Object Fields

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique alert identifier. Empty string if unavailable. |
| `urlText` | string | Short description suitable for display as a link title. |
| `content` | string | Full notification body. May contain HTML markup. Empty string if unavailable. |
| `subtitle` | string | Notification subtitle / summary line. |
| `providerCode` | string | Transit provider code (e.g. `"KVV"`, `"bodo"`). Empty string if unavailable. |
| `priority` | string | Priority level: `"normal"` or `"high"`. Empty string if unavailable. |

#### Errors

| Status | Body | Cause |
|---|---|---|
| `400` | `{"error": "Missing 'stopID' parameter"}` | The `stopID` query parameter was not provided. |
| `400` | `{"error": "Invalid stop ID"}` | The stop ID is empty, too long, or contains invalid characters. |

#### Notes

- Queries multiple EFA notification providers simultaneously (efa-bw.de, efa.vrr.de).
- Only currently valid notifications are returned (upstream filtering via `filterValid=1`).
- Duplicate alerts (same ID from multiple providers) are automatically deduplicated.
- Returns an empty array `[]` when no active notifications affect the given stop.
- The `content` field may contain **HTML**. Sanitize it before rendering in a browser context.

---

### 5. Nearby Stops

Find transit stops near a geographic location. Returns stops from the database ordered by distance (nearest first). Stops are populated via the [Search Stops](#2-search-stops) endpoint, so only previously searched stops are available.

| Property | Value |
|---|---|
| **URL** | `/api/stops/nearby` |
| **Method** | `GET` |

#### Query Parameters

| Parameter | Required | Type | Default | Description |
|---|---|---|---|---|
| `lat` | **Yes** | number | — | Latitude of the center point. Must be between -90 and 90. |
| `lon` | **Yes** | number | — | Longitude of the center point. Must be between -180 and 180. |
| `radius` | No | number | `1000` | Search radius in meters. Must be between 0 (exclusive) and 50000. |
| `limit` | No | integer | `10` | Maximum number of stops to return. Must be between 1 and 100. |

#### Response

**`200 OK`** — JSON array of stop objects ordered by distance (nearest first).

**Example Request:**
```
GET /api/stops/nearby?lat=49.0094&lon=8.4044&radius=500&limit=5
```

**Example Response:**
```json
[
  {
    "stop_id": "de:08212:1",
    "local_id": "7000001",
    "stop_name": "Karlsruhe, Marktplatz",
    "city": "Karlsruhe",
    "latitude": 49.00937,
    "longitude": 8.40390,
    "distance_meters": 72.34
  },
  {
    "stop_id": "de:08212:2",
    "local_id": "7000002",
    "stop_name": "Karlsruhe, Kronenplatz",
    "city": "Karlsruhe",
    "latitude": 49.00890,
    "longitude": 8.41050,
    "distance_meters": 312.56
  }
]
```

#### Stop Object Fields

| Field | Type | Description |
|---|---|---|
| `stop_id` | string | Global stop ID (e.g. `"de:08212:1"`). Use this with the departures and notifications endpoints. |
| `local_id` | string or null | Local numeric stop ID (e.g. `"7000001"`). `null` if unavailable. |
| `stop_name` | string | Full stop name. |
| `city` | string or null | City or locality name. `null` if unavailable. |
| `latitude` | number | Latitude of the stop. |
| `longitude` | number | Longitude of the stop. |
| `distance_meters` | number | Distance in meters from the requested coordinates to the stop. |

#### Errors

| Status | Body | Cause |
|---|---|---|
| `400` | `{"error": "Missing 'lat' and/or 'lon' parameter"}` | One or both of `lat` / `lon` were not provided. |
| `400` | `{"error": "Invalid 'lat' or 'lon' value"}` | `lat` or `lon` is not a valid number. |
| `400` | `{"error": "'lat' must be between -90 and 90, 'lon' must be between -180 and 180"}` | Coordinates are out of valid range. |
| `400` | `{"error": "Invalid 'radius' value"}` | `radius` is not a valid number. |
| `400` | `{"error": "'radius' must be between 0 and 50000 meters"}` | Radius is out of valid range. |
| `400` | `{"error": "Invalid 'limit' value"}` | `limit` is not a valid integer. |
| `400` | `{"error": "'limit' must be between 1 and 100"}` | Limit is out of valid range. |
| `503` | `{"error": "Database not configured"}` | No database connection is configured. |
| `503` | `{"error": "Database connection failed"}` | Unable to connect to the database. |
| `500` | `{"error": "Database query failed"}` | The spatial query failed (e.g. PostGIS extension not installed). |

#### Notes

- This endpoint queries the local `stops` database using PostGIS spatial functions (`ST_DWithin`, `ST_Distance`).
- Only stops that have been previously discovered via the [Search Stops](#2-search-stops) endpoint are available. If you need comprehensive coverage, search for stops in the area first.
- Distance is calculated on the WGS 84 ellipsoid (geographic distance, not planar).

---

## Data Types Reference

### MOT Codes (Mode of Transport)

The `mot` field in departure objects identifies the vehicle type using these integer codes:

| Code | Mode of Transport |
|---|---|
| 0 | Train |
| 1 | Commuter railway (S-Bahn) |
| 2 | Underground train (U-Bahn) |
| 3 | City rail (Stadtbahn) |
| 4 | Tram (Straßenbahn) |
| 5 | City bus |
| 6 | Regional bus |
| 7 | Coach |
| 8 | Cable car |
| 9 | Boat / Ferry |
| 10 | Transit on demand |
| 11 | Other |
| 12 | Airplane |
| 13 | Regional train |
| 14 | National train |
| 15 | International train |
| 16 | High-speed train (ICE) |
| 17 | Rail replacement service |
| 18 | Shuttle train |
| 19 | Community bus (Bürgerbus) |
| -1 | Unknown / not available |

---

## Error Handling

All error responses follow this structure:

```json
{"error": "<human-readable message>"}
```

### HTTP Status Codes Used

| Code | Meaning | When Returned |
|---|---|---|
| `200` | Success | Request completed successfully. |
| `400` | Bad Request | Missing required parameters, or parameter values are invalid. |
| `401` | Unauthorized | Authentication is enabled and the API key is missing or invalid. |
| `500` | Internal Server Error | An internal error occurred (e.g. a database query failed). |
| `502` | Bad Gateway | The upstream EFA provider is unreachable or returned an error. |
| `503` | Service Unavailable | A required service (e.g. database) is not configured or unreachable. |

### Input Validation Rules

- **Stop IDs** must be 1–100 characters, matching the pattern `^[a-zA-Z0-9:_. -]+$`.
- **Search queries** must be 1–200 characters with no ASCII control characters (0x00–0x1F, 0x7F).
- **Coordinates** (`lat`, `lon`) must be valid numbers within geographic bounds (latitude: -90 to 90, longitude: -180 to 180).
- **Radius** must be a positive number up to 50,000 meters.
- **Limit** must be an integer between 1 and 100.
- Requests with invalid input are rejected with `400` before any upstream call is made.

---

## Caching Behavior

- Departure responses are cached for **30 seconds** per unique combination of `stopId`, `detailed`, and `delay` parameters.
- Subsequent requests within the TTL window return cached data instantly.
- The `track` filter is applied locally after cache retrieval, so different track filters on the same stop share the same cache entry.
- Notification responses are **not cached** — each request queries upstream providers directly.
- Maximum cache size: **10,000 entries**. Expired entries are evicted automatically.

---

## Integration Walkthrough

A typical app integration follows this workflow:

### Step 1 — Find a stop

Search for the user's desired stop by name:

```
GET /api/stops/search?q=Hauptbahnhof&location=true
```

Extract the stop ID from the response. Use either the global ID (`id` field, e.g. `"de:08212:90"`) or the local ID (`properties.stopId`, e.g. `"7000090"`).

### Step 2 — Fetch departures

Request departures using the stop ID:

```
GET /api/stops/de:08212:90
```

Or with all optional data:

```
GET /api/stops/de:08212:90?detailed=true&delay=true
```

Or filtered to a specific platform:

```
GET /api/stops/de:08212:90?track=3
```

### Step 3 — Check for service alerts

Query active notifications for the same stop:

```
GET /api/current_notifs?stopID=de:08212:90
```

Display any returned notifications to the user. If the array is empty, there are no active alerts.

### Step 4 — Refresh

Poll the departures endpoint periodically (every 30+ seconds aligns with the cache TTL) to keep the display current.

---

## Code Examples

### Python

```python
import requests

BASE_URL = "http://localhost:8080"

# Step 1: Search for a stop
response = requests.get(f"{BASE_URL}/api/stops/search", params={
    "q": "Marktplatz",
    "location": "true"
})
search_data = response.json()

# Extract the first stop's ID
stops = search_data.get("stopFinder", {}).get("points", [])
if not stops:
    print("No stops found")
    exit()

stop_id = stops[0]["id"]           # e.g. "de:08212:1"
stop_name = stops[0]["name"]       # e.g. "Karlsruhe, Marktplatz"
print(f"Found stop: {stop_name} ({stop_id})")

# Step 2: Get departures with delay info
response = requests.get(f"{BASE_URL}/api/stops/{stop_id}", params={
    "detailed": "true",
    "delay": "true"
})
departures = response.json()

for dep in departures:
    line = dep["line"]
    direction = dep["direction"]
    minutes = dep["minutes_remaining"]
    delay = dep.get("delay_minutes", 0)
    realtime = "live" if dep["is_realtime"] else "scheduled"
    print(f"  Line {line} → {direction} in {minutes} min ({realtime}, +{delay} min delay)")

# Step 3: Check notifications
response = requests.get(f"{BASE_URL}/api/current_notifs", params={
    "stopID": stop_id
})
notifications = response.json()

if notifications:
    print(f"\n⚠ {len(notifications)} active alert(s):")
    for notif in notifications:
        print(f"  [{notif['priority']}] {notif['subtitle']}")
else:
    print("\nNo active service alerts.")
```

### JavaScript (fetch)

```javascript
const BASE_URL = "http://localhost:8080";

async function getTransitInfo(query) {
  // Step 1: Search for stops
  const searchRes = await fetch(
    `${BASE_URL}/api/stops/search?q=${encodeURIComponent(query)}&location=true`
  );
  const searchData = await searchRes.json();
  const stops = searchData?.stopFinder?.points ?? [];

  if (stops.length === 0) {
    console.log("No stops found");
    return;
  }

  const stopId = stops[0].id;
  console.log(`Found: ${stops[0].name} (${stopId})`);

  // Step 2: Get departures
  const depRes = await fetch(
    `${BASE_URL}/api/stops/${encodeURIComponent(stopId)}?detailed=true&delay=true`
  );
  const departures = await depRes.json();

  departures.forEach((dep) => {
    console.log(
      `  Line ${dep.line} → ${dep.direction} in ${dep.minutes_remaining} min` +
      (dep.delay_minutes ? ` (+${dep.delay_minutes} delay)` : "")
    );
  });

  // Step 3: Check notifications
  const notifRes = await fetch(
    `${BASE_URL}/api/current_notifs?stopID=${encodeURIComponent(stopId)}`
  );
  const notifications = await notifRes.json();

  if (notifications.length > 0) {
    console.log(`\n⚠ ${notifications.length} alert(s):`);
    notifications.forEach((n) => console.log(`  [${n.priority}] ${n.subtitle}`));
  }
}

getTransitInfo("Marktplatz");
```

### cURL

```bash
# Health check
curl http://localhost:8080/health

# Search for stops
curl "http://localhost:8080/api/stops/search?q=Marktplatz&location=true"

# Get departures (standard)
curl "http://localhost:8080/api/stops/de:08212:1"

# Get departures (detailed with delays)
curl "http://localhost:8080/api/stops/de:08212:1?detailed=true&delay=true"

# Get departures filtered by platform
curl "http://localhost:8080/api/stops/de:08212:1?track=3"

# Get notifications for a stop
curl "http://localhost:8080/api/current_notifs?stopID=7000107"

# Find nearby stops (within 500m, max 5 results)
curl "http://localhost:8080/api/stops/nearby?lat=49.0094&lon=8.4044&radius=500&limit=5"

# Find nearby stops (defaults: 1000m radius, 10 results)
curl "http://localhost:8080/api/stops/nearby?lat=49.0094&lon=8.4044"
```

---

## Common Patterns

### Building a Departure Board

1. Search for the stop once, store the `stop_id`.
2. Poll `GET /api/stops/{stopId}?delay=true` every 30 seconds.
3. Sort the response array by `minutes_remaining` (it typically arrives pre-sorted).
4. Display `line`, `direction`, `minutes_remaining`, and optionally `delay_minutes`.

### Showing Service Disruptions

1. Call `GET /api/current_notifs?stopID={stopId}` when loading a stop view.
2. If the response array is non-empty, display a banner with `subtitle` for each alert.
3. Use `content` (may contain HTML) for a detailed view.
4. Use `priority` to style high-priority alerts differently.

### Filtering by Transport Mode

After fetching departures, filter client-side by the `mot` field:

```python
# Show only trams (mot=4) and city buses (mot=5)
trams_and_buses = [d for d in departures if d["mot"] in [4, 5]]
```

### Handling Errors Gracefully

```python
response = requests.get(f"{BASE_URL}/api/stops/{stop_id}")

if response.status_code == 400:
    error = response.json()
    print(f"Bad request: {error['error']}")
elif response.status_code == 502:
    print("Upstream transit provider is temporarily unavailable. Try again shortly.")
elif response.status_code == 200:
    departures = response.json()
    # process departures
```

### Using Both Stop ID Formats

The API accepts two stop ID formats interchangeably:

- **Global ID**: `de:08212:1` — the `id` field from search results.
- **Local ID**: `7000001` — the `properties.stopId` field from search results.

Both work for the departures and notifications endpoints:

```
GET /api/stops/de:08212:1          ← global ID
GET /api/stops/7000001             ← local ID
GET /api/current_notifs?stopID=de:08212:1
GET /api/current_notifs?stopID=7000001
```
