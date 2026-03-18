# KVV Tracker — Auth & User Data Schema

This file documents the database schema for the Transit Tracker API authentication, authorization, user preferences and security audit log. It contains the DBML definition followed by human-readable table-by-table documentation, relationships, indexes, and notes about security and usage.

---

## DBML

```dbml
// KVV Tracker Auth + User Data Schema

Table users {
  id uuid [pk, default: `gen_random_uuid()`]
  email citext [not null, unique]
  username text [unique]
  password_hash text
  is_active boolean [not null, default: `true`]
  is_verified boolean [not null, default: `false`]
  failed_login_attempts int [not null, default: `0`]
  locked_until timestamptz
  created_at timestamptz [not null, default: `now()`]
  updated_at timestamptz [not null, default: `now()`]
  last_login_at timestamptz
  locale text
  timezone text
  avatar_url text
  bio text
  metadata jsonb [not null, default: `'{}'::jsonb`]
}

Table roles {
  id smallserial [pk]
  name text [not null, unique]
  description text
}

Table user_roles {
  user_id uuid [not null, ref: > users.id, pk]
  role_id smallint [not null, ref: > roles.id, pk]
  assigned_at timestamptz [not null, default: `now()`]
}

Table sessions {
  id uuid [pk, default: `gen_random_uuid()`]
  user_id uuid [not null, ref: > users.id]
  refresh_token_hash text [not null]
  created_at timestamptz [not null, default: `now()`]
  expires_at timestamptz [not null]
  revoked_at timestamptz
  ip inet
  user_agent text
  device_info text
}

Table user_tokens {
  id uuid [pk, default: `gen_random_uuid()`]
  user_id uuid [not null, ref: > users.id]
  token_hash text [not null]
  type text [not null]
  created_at timestamptz [not null, default: `now()`]
  expires_at timestamptz [not null]
  consumed_at timestamptz
  meta jsonb [default: `'{}'::jsonb`]
}

Table oauth_accounts {
  id uuid [pk, default: `gen_random_uuid()`]
  user_id uuid [not null, ref: > users.id]
  provider text [not null]
  provider_user_id text [not null]
  provider_email citext
  access_token text
  refresh_token_encrypted text
  expires_at timestamptz
  created_at timestamptz [not null, default: `now()`]

  Indexes {
    (provider, provider_user_id) [unique]
  }
}

Table password_history {
  id uuid [pk, default: `gen_random_uuid()`]
  user_id uuid [not null, ref: > users.id]
  password_hash text [not null]
  changed_at timestamptz [not null, default: `now()`]
}

Table api_keys {
  id serial [pk]
  user_id uuid [not null, ref: > users.id]
  key_hash text [not null]
  key_prefix text [not null]
  name text [not null]
  scopes text[] [default: `'{}'`]
  created_at timestamptz [default: `now()`]
  expires_at timestamptz
  revoked boolean [default: `false`]
  last_used_at timestamptz
}

////////////////////////////////////////////////////
// USER DATA (preferences, saved routes, favorites)
////////////////////////////////////////////////////

Table user_data {
  user_id uuid [pk, ref: > users.id]

  home_stop_id text
  favorite_stops text[] [default: `'{}'`]

  saved_routes jsonb [default: `'[]'::jsonb`]
  notification_settings jsonb [default: `'{}'::jsonb`]
  ui_preferences jsonb [default: `'{}'::jsonb`]

  created_at timestamptz [default: `now()`]
  updated_at timestamptz [default: `now()`]
}

////////////////////////////////////////////////////
// SECURITY AUDIT LOG
////////////////////////////////////////////////////

Table security_events {
  id uuid [pk, default: `gen_random_uuid()`]

  user_id uuid [ref: > users.id]

  event_type text [not null]
  event_description text

  ip inet
  user_agent text

  related_session uuid [ref: > sessions.id]

  metadata jsonb [default: `'{}'::jsonb`]

  created_at timestamptz [not null, default: `now()`]
}
```

---

## Overview

This schema covers:

- Authentication: `users`, `password_history`, `oauth_accounts`, `user_tokens`, `sessions`, `api_keys`.
- Authorization: `roles` and `user_roles` join table.
- User profile & preferences: `user_data` (home stop, saved routes, UI pref, favorites).
- Security auditing: `security_events`.

The schema aims to separate concerns (auth vs preferences) and support common features: OAuth sign-ins, API keys, refreshable sessions, token-based flows (password resets, email verification), and an append-only audit log.

---

## Tables & column explanations

### `users`
Stores primary user account data.

| Column | Type | Constraints / Default | Purpose |
|---|---:|---|---|
| id | uuid | PK, `gen_random_uuid()` | Primary identifier for a user |
| email | citext | not null, unique | Contact and login identifier (case-insensitive)
| username | text | unique | Optional display name |
| password_hash | text |  | bcrypt/argon2 hashed password (nullable for OAuth-only accounts)
| is_active | boolean | not null, default true | Soft-delete / enable flag
| is_verified | boolean | not null, default false | Email verified flag
| failed_login_attempts | int | not null, default 0 | For lockout policies
| locked_until | timestamptz |  | If set, account blocked until timestamp
| created_at / updated_at | timestamptz | not null, default now() | Timestamps
| last_login_at | timestamptz |  | Last successful login
| locale / timezone | text |  | User locale/timezone preferences
| avatar_url / bio | text |  | Profile fields
| metadata | jsonb | not null, default '{}' | Free-form metadata (features, flags)


### `roles`
Simple RBAC role registry.

| Column | Type | Purpose |
|---|---:|---|
| id | smallserial | PK |
| name | text | unique role key (eg. `admin`, `moderator`) |
| description | text | human readable description |


### `user_roles`
Many-to-many between users and roles. Composite primary key `(user_id, role_id)` prevents duplicate assignments.

| Column | Type | Purpose |
|---|---:|---|
| user_id | uuid | FK → users.id |
| role_id | smallint | FK → roles.id |
| assigned_at | timestamptz | when the role was granted |


### `sessions`
Server-side refresh sessions. Store a hash of the refresh token (never store raw token). Track IP and UA for security.

| Column | Type | Purpose |
|---|---:|---|
| id | uuid | PK: session id / refresh token id |
| user_id | uuid | FK → users.id |
| refresh_token_hash | text | hashed refresh token |
| created_at / expires_at | timestamptz | validity window |
| revoked_at | timestamptz | revocation time (soft revoke) |
| ip | inet | recorded IP address |
| user_agent / device_info | text | client info |


### `user_tokens`
One-off tokens for flows (password reset, email verify, magic link). Store hash and consumed_at.

| Column | Type | Purpose |
|---|---:|---|
| id | uuid | PK |
| user_id | uuid | FK → users.id |
| token_hash | text | hashed token |
| type | text | token kind (eg. `password_reset`, `email_verify`) |
| created_at / expires_at | timestamptz | validity window |
| consumed_at | timestamptz | when token used |
| meta | jsonb | extra data if needed |


### `oauth_accounts`
External provider linkage (Google, Apple, etc.). `provider + provider_user_id` is unique.

| Column | Type | Purpose |
|---|---:|---|
| id | uuid | PK |
| user_id | uuid | FK → users.id |
| provider | text | provider name (eg. `google`) |
| provider_user_id | text | provider's user id |
| provider_email | citext | email from provider |
| access_token | text | optional short-lived token (if needed)
| refresh_token_encrypted | text | encrypted refresh token for provider
| expires_at | timestamptz | provider token expiry
| created_at | timestamptz | linkage creation time

Index: unique on `(provider, provider_user_id)`.


### `password_history`
Store previous password hashes to prevent reuse.

| Column | Type | Purpose |
|---|---:|---|
| id | uuid | PK |
| user_id | uuid | FK → users.id |
| password_hash | text | previous password hash |
| changed_at | timestamptz | change timestamp |


### `api_keys`
Application-level API keys issued by users. Store only a hash of the key (`key_hash`) and keep a short human-readable prefix to identify keys in logs or the UI.

| Column | Type | Purpose |
|---|---:|---|
| id | uuid | PK |
| user_id | uuid | FK → users.id |
| key_hash | text | hashed secret part of the key |
| key_prefix | text | first N chars shown in UI |
| name | text | user supplied name for key |
| scopes | text[] | allowed scopes for the key |
| created_at / expires_at | timestamptz | lifecycle fields |
| revoked | boolean | soft-revocation flag |
| last_used_at | timestamptz | telemetry for rotation |


### `user_data`
User-specific preferences and saved objects. This table is one-row-per-user and can hold JSON blobs for flexibility.

| Column | Type | Purpose |
|---|---:|---|
| user_id | uuid | PK, FK → users.id |
| home_stop_id | text | user's saved home stop identifier |
| favorite_stops | text[] | array of stops the user favorited |
| saved_routes | jsonb | array/object describing stored routes |
| notification_settings | jsonb | per-channel notification preferences |
| ui_preferences | jsonb | theme, compact/expanded lists, etc. |
| created_at / updated_at | timestamptz | timestamps |


### `security_events`
Append-only audit log for security-related events: logins, failed logins, password changes, API key usage, suspicious activity, role changes, etc.

| Column | Type | Purpose |
|---|---:|---|
| id | uuid | PK |
| user_id | uuid | FK → users.id (nullable for unauthenticated events) |
| event_type | text | categorical type (eg. `login.success`, `login.fail`, `api_key.revoked`) |
| event_description | text | human-readable details |
| ip | inet | source IP |
| user_agent | text | UA string |
| related_session | uuid | FK → sessions.id (optional) |
| metadata | jsonb | extra structured data |
| created_at | timestamptz | event time (default now()) |


---

## Relationships

- `user_roles.user_id` → `users.id`
- `user_roles.role_id` → `roles.id`
- `sessions.user_id` → `users.id`
- `user_tokens.user_id` → `users.id`
- `oauth_accounts.user_id` → `users.id`
- `password_history.user_id` → `users.id`
- `api_keys.user_id` → `users.id`
- `user_data.user_id` → `users.id`
- `security_events.user_id` → `users.id`
- `security_events.related_session` → `sessions.id`

---

## Indexes & Uniqueness

- `users.email` — unique (case-insensitive)
- `users.username` — unique
- `roles.name` — unique
- `oauth_accounts` — unique `(provider, provider_user_id)`
- `user_roles` has composite PK `(user_id, role_id)` to prevent duplicates

Consider adding additional indexes for common query patterns, e.g. `sessions(user_id)`, `api_keys(key_prefix)`, and `security_events(created_at)` for time-based queries.

---

## Example queries (Postgres)

**Insert a new user**

```sql
INSERT INTO users (email, username, password_hash)
VALUES ('alice@example.com', 'alice', '<argon2-hash>')
RETURNING id;
```

**Create a session (store hash of refresh token)**

```sql
INSERT INTO sessions (user_id, refresh_token_hash, expires_at, ip, user_agent)
VALUES ('<user-id>', '<sha256-hash>', now() + interval '30 days', '203.0.113.5', 'kvv-web/1.0')
RETURNING id;
```

**Add/update user preferences (upsert pattern)**

```sql
INSERT INTO user_data (user_id, home_stop_id, favorite_stops)
VALUES ('<user-id>', 'stop:1234', ARRAY['stop:1234','stop:4567'])
ON CONFLICT (user_id) DO UPDATE
  SET favorite_stops = EXCLUDED.favorite_stops,
      updated_at = now();
```
- export this markdown as a downloadable file.

