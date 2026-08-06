CREATE TABLE mail_metadata(mail_id INTEGER PRIMARY KEY, sender_account_id INTEGER NOT NULL, recipient_account_id INTEGER NOT NULL, delivered_at INTEGER, revision INTEGER NOT NULL);
CREATE INDEX mail_recipient_idx ON mail_metadata(recipient_account_id, delivered_at);
