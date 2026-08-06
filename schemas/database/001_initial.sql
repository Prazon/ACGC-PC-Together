CREATE TABLE accounts(account_id INTEGER PRIMARY KEY, created_at INTEGER NOT NULL, last_seen_at INTEGER NOT NULL, banned INTEGER NOT NULL DEFAULT 0);
CREATE TABLE sessions(session_id TEXT PRIMARY KEY, account_id INTEGER NOT NULL REFERENCES accounts(account_id), connected INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE TABLE transactions(journal_sequence INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, operation_type INTEGER NOT NULL, result_code INTEGER NOT NULL, committed_at INTEGER NOT NULL);
CREATE TABLE audit_log(id INTEGER PRIMARY KEY AUTOINCREMENT, actor_account_id INTEGER NOT NULL DEFAULT 0, action TEXT NOT NULL, details TEXT NOT NULL, created_at INTEGER NOT NULL);
