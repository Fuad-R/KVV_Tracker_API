-- Schema initialization for the test database.
-- Creates the stops and api_keys tables used by the Transit Tracker API.

CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE IF NOT EXISTS stops (
    stop_id      TEXT PRIMARY KEY,
    local_id     TEXT,
    stop_name    TEXT NOT NULL,
    city         TEXT,
    mot          INTEGER[],
    location     GEOGRAPHY(Point, 4326)
);

CREATE TABLE IF NOT EXISTS api_keys (
    id           SERIAL PRIMARY KEY,
    key_hash     TEXT NOT NULL UNIQUE,
    description  TEXT,
    revoked      BOOLEAN DEFAULT FALSE,
    expires_at   TIMESTAMPTZ,
    created_at   TIMESTAMPTZ DEFAULT NOW(),
    last_used_at TIMESTAMPTZ
);

-- Seed data: sample stops for integration tests
INSERT INTO stops (stop_id, stop_name, city, location)
VALUES
    ('7000107', 'Knielingen', 'Karlsruhe',
     ST_SetSRID(ST_MakePoint(8.3447, 49.0245), 4326)::GEOGRAPHY),
    ('7000001', 'Marktplatz', 'Karlsruhe',
     ST_SetSRID(ST_MakePoint(8.4037, 49.0069), 4326)::GEOGRAPHY),
    ('7000002', 'Kronenplatz', 'Karlsruhe',
     ST_SetSRID(ST_MakePoint(8.4100, 49.0094), 4326)::GEOGRAPHY)
ON CONFLICT (stop_id) DO NOTHING;
