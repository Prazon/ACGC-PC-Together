-- Per-mod key/value that survives a restart (calendar.store / calendar.load).
-- Keyed by mod id so one mod cannot read or overwrite another's state.
-- Values are short tagged scalars written by the host, not mod-controlled blobs.
CREATE TABLE IF NOT EXISTS mod_state(mod_id TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,updated_at INTEGER NOT NULL,PRIMARY KEY(mod_id,key)) WITHOUT ROWID;
