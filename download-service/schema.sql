CREATE TABLE IF NOT EXISTS beta_accounts (
  username TEXT PRIMARY KEY,
  status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','suspended')),
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS beta_keys (
  id TEXT PRIMARY KEY,
  key_hash TEXT UNIQUE NOT NULL,
  username TEXT NOT NULL REFERENCES beta_accounts(username),
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  revoked_at INTEGER
);
CREATE INDEX IF NOT EXISTS beta_keys_account ON beta_keys(username);
CREATE TABLE IF NOT EXISTS removed_beta_keys (
  key_id TEXT PRIMARY KEY REFERENCES beta_keys(id)
);
CREATE TABLE IF NOT EXISTS injection_events (
  event_id TEXT PRIMARY KEY,
  key_id TEXT NOT NULL REFERENCES beta_keys(id),
  reported_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS injection_events_time ON injection_events(reported_at);
CREATE INDEX IF NOT EXISTS injection_events_key ON injection_events(key_id);
CREATE TABLE IF NOT EXISTS client_reports (
  event_id TEXT PRIMARY KEY,
  channel TEXT NOT NULL CHECK(channel IN ('alpha','beta')),
  reported_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS client_reports_time ON client_reports(reported_at);
CREATE TABLE IF NOT EXISTS beta_devices (
  key_id TEXT PRIMARY KEY REFERENCES beta_keys(id),
  device_hash TEXT NOT NULL,
  bound_at INTEGER NOT NULL
);
-- Patreon sign-in (patreon.mjs): which Patreon account owns which Beta key.
CREATE TABLE IF NOT EXISTS patreon_links (
  patreon_id TEXT PRIMARY KEY,
  key_id TEXT UNIQUE NOT NULL REFERENCES beta_keys(id),
  created_at INTEGER NOT NULL
);
-- One-use Patreon sign-in states, kept for 10 minutes.
CREATE TABLE IF NOT EXISTS oauth_states (
  state TEXT PRIMARY KEY,
  new_key INTEGER NOT NULL DEFAULT 0,
  expires_at INTEGER NOT NULL
);
