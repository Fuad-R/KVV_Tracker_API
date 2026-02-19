**Transit tracker API**


# KVV Tracker API - Usage & Functionality Documentation

## Overview
The KVV Tracker API provides real-time transit departure information for the Karlsruhe public transport system. It integrates with the KVV (Karlsruher Verkehrsverbund) data provider to deliver normalized, cached departure data with optional filtering and accessibility information.

## Core Functionality

### 1. **Stop Search**
Search for transit stops by name across the KVV network. Results include stop identification and geographic coordinates.
When configured, each stop search also stores any new stops in the PostgreSQL database.

### 2. **Real-time Departures**
Retrieve live departure information for any stop, including line numbers, directions, platforms, and countdown timers.

### 3. **Data Caching**
30-second TTL caching reduces upstream provider load while maintaining fresh data.

### 4. **Accessibility Information**
Query low-floor vehicle availability and wheelchair accessibility for departures.

### 5. **Platform Filtering**
Filter departures by specific platform/track to refine results.

### 6. **Delay Tracking**
Optional real-time delay information for each departure.

## API Endpoints

### Search Stops
**`GET /api/stops/search`**

Search for transit stops by name.

**Query Parameters:**
- `q` (required) - Stop name or partial name to search
- `city` (optional) - City filter (currently no-op)
- `location` (optional) - Include geographic coordinates (`true`/`1`)

**Response:** JSON array of matching stops with stop identifiers and coordinates

---

## Database Configuration
Stop search persistence reads PostgreSQL connection values from `db_connection.txt` in the repository root.
Copy `db_connection.txt.example` to `db_connection.txt` and update the values (the `sslmode` entry is optional).
If the file is missing or incomplete, stop searches still respond, but no stops are persisted.

The database must include the following schema:
```
CREATE TABLE stops (
   stop_id TEXT PRIMARY KEY,
   stop_name TEXT NOT NULL,
   city TEXT,
   mot SMALLINT[],
   location GEOGRAPHY(Point, 4326) NOT NULL,
   original_search TEXT,
   created TIMESTAMPTZ DEFAULT NOW(),
   last_updated TIMESTAMPTZ DEFAULT NOW()
);
```

### Get Departures
**`GET /api/stops/{stopId}`**

Retrieve all upcoming departures from a specific stop.

**Path Parameters:**
- `{stopId}` - Stop identifier from search results

**Query Parameters:**
- `detailed` (optional) - Include extended information (`true`/`1`)
  - Adds: `low_floor`, `wheelchair_accessible`, `train_type`, `train_length`, `train_composition`, `hints`
- `delay` (optional) - Include delay information (`true`/`1`)
  - Adds: `delay_minutes` field to each departure
- `track` (optional) - Filter by platform/track number
  - Supports exact matches and partial matching (e.g., `"1"` matches `"Gleis 1"`, `"1a"`)

**Response:** JSON array of departures with the following fields:
- `line` - Line number (string)
- `direction` - Destination direction
- `mot` - Mode of transport type code (integer)
- `platform` - Platform/track designation
- `minutes_remaining` - Countdown in minutes until departure
- `is_realtime` - Boolean indicating real-time data availability
- `delay_minutes` - Minutes of delay (when `delay=true`)
- `low_floor` - Low-floor vehicle availability (when `detailed=true`)
- `wheelchair_accessible` - Wheelchair access capability (when `detailed=true`)
- `hints` - Array of additional information (when `detailed=true`)

---

## Usage Examples

### Search for a stop
```
GET /api/stops/search?q=Hauptbahnhof&location=true
```

### Get current departures
```
GET /api/stops/id_1234
```

### Get detailed departures with delays
```
GET /api/stops/id_1234?detailed=true&delay=true
```

### Filter departures by platform
```
GET /api/stops/id_1234?track=1
```

### Combined query
```
GET /api/stops/id_1234?detailed=true&delay=true&track=2
```

---

## Response Format

All endpoints return JSON. Standard departure response:

```json
[
  {
    "direction":"Wolfartsweier Über ZKM",
    "is_realtime":true,
    "line":"2",
    "minutes_remaining":0,
    "mot":4,
    "platform":"4"
  },
  {
    "direction":"Oberreut",
    "is_realtime":true,
    "line":"4",
    "minutes_remaining":0,
    "mot":4,
    "platform":"3"
  }
]
```

---

## Performance Considerations

- Responses are cached for 30 seconds per stop/parameter combination
- Repeated queries within the TTL window return cached results
- Platform filtering is applied post-normalization (locally)
- Upstream provider supports up to 40 departures per request
