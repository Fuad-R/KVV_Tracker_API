CREATE EXTENSION IF NOT EXISTS postgis;

CREATE TABLE IF NOT EXISTS stops (
   stop_id TEXT PRIMARY KEY,
   local_id TEXT,
   stop_name TEXT NOT NULL,
   city TEXT,
   mot SMALLINT[],
   location GEOGRAPHY(Point, 4326) NOT NULL,
   original_search TEXT,
   created TIMESTAMPTZ DEFAULT NOW(),
   last_updated TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS api_keys (
   id SERIAL PRIMARY KEY,
   user_id INTEGER NOT NULL,
   key_hash TEXT NOT NULL,
   key_prefix TEXT NOT NULL,
   name TEXT NOT NULL,
   scopes TEXT[] DEFAULT '{}',
   created_at TIMESTAMPTZ DEFAULT NOW(),
   expires_at TIMESTAMPTZ,
   revoked BOOLEAN DEFAULT FALSE,
   last_used_at TIMESTAMPTZ
);

INSERT INTO stops (stop_id, local_id, stop_name, city, location, original_search)
VALUES (
  '7000107',
  '7000107',
  'Knielingen',
  'Karlsruhe',
  ST_SetSRID(ST_MakePoint(8.3606, 49.0333), 4326)::geography,
  'Knielingen'
)
ON CONFLICT (stop_id) DO NOTHING;

INSERT INTO api_keys (user_id, key_hash, key_prefix, name, revoked)
VALUES (1, '4c806362b613f7496abf284146efd31da90e4b16169fe001841ca17290f427c4', 'test', 'valid-test-key', FALSE)
ON CONFLICT DO NOTHING;

INSERT INTO api_keys (user_id, key_hash, key_prefix, name, revoked)
VALUES (2, '7c92350e6c89a32c27e4ac7a72c7b518cf8bd66721978211c3f213ad37d0f83a', 'revo', 'revoked-test-key', TRUE)
ON CONFLICT DO NOTHING;

INSERT INTO api_keys (user_id, key_hash, key_prefix, name, revoked, expires_at)
VALUES (3, 'a25552f5c9db0855658ef39cea5666a47d154771fb1a3906cb8c3691e2e04acc', 'expi', 'expired-test-key', FALSE, '2025-01-01 00:00:00+00')
ON CONFLICT DO NOTHING;
