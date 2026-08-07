# Town persistence and recovery

One server process owns one town directory:

```text
towns/default/
  town.gci
  town.db
  config.toml
  clean.shutdown
  journal/operations.log
  snapshots/checkpoint-<sequence>.bin
```

`operations.log` is append-only. Records have a storage version, monotonically
increasing sequence, semantic record type, bounded payload, and CRC32. Each
accepted persistent operation serializes the authoritative town state and is
flushed to durable storage before success is reported. Startup replays records
newer than the newest valid checkpoint. A torn final record is detected and
physically truncated before any new append; corruption in the middle of a
journal remains a hard error.

Checkpoints use atomic temporary-file replacement, sequence and payload bounds,
and CRC32. Seven checkpoints are retained by default. If the newest checkpoint
is corrupt, startup falls back to the newest earlier valid checkpoint.

`town.db` uses SQLite WAL mode, full synchronous writes, foreign keys, and
transactional migrations. It stores accounts, sessions, bans, transaction
metadata, audit records, mail metadata, and housing metadata. A database backup
is created before schema migration.

The town state stores the authoritative clock/weather, canonical-world initialized flag, residents and visitors,
appearance, transforms, inventories, ledgers, foreground tiles, shop, museum,
mail, NPC state, and the four original houses. State schema v4 adds full
three-floor house layout/facing, furniture switches, lights, music, and the
house-initialized marker. State schema v5 adds each account's mail revision
and each letter's 96-byte body; v6 replaces that body with the whole letter --
location, font, mail and paper type, sender name, header, body, and footer --
so a letter round-trips into the original UI unchanged. Both mail lists are
rebuilt from the records themselves, which are replayed in identifier order, so
only the revision is stored per account. It reads v1-v5, mapping a v5 note onto
the head of the body field and treating every v5 letter as waiting in a mailbox,
supplies deterministic legacy appearance defaults, defaults a pre-v5 mailbox
revision to 1 with an empty letter body, and treats valid pre-v4 houses with
furniture as initialized so a newly connecting client cannot overwrite existing
rooms.

State schema v8 moves the held item out of the appearance block and into the
account's inventory, immediately after the fifteen pocket slots, because the
hand is inventory state rather than presentation. A v7-or-older checkpoint still
decodes: the held item is read from its old position inside the appearance and
routed into the inventory, so an existing town keeps whatever each resident was
holding. `gci.cpp` writes `Private_c::equipment` from `InventoryState::equipped`
at the same GCI offset as before, sourced from the inventory instead.

Note that a v8 server rewrites the town at its first checkpoint, and older
builds cannot read v8. Keep a backup of a town directory before upgrading it if
you intend to be able to roll the server back.

An operator gift (bank bells or a letter) is committed the same way a player
transaction is: the whole town state is appended to the journal and flushed
before the command reports success, so a gift cannot be acknowledged and then
lost to a crash.

GCI conversion is semantic and preserves the native original layout. Import
validates the GAFE01/GAFU01 header, exact block length, native checksum/layout,
four original resident slots, foreground/deposit state, economy, museum,
weather, and structures. Export updates only mapped fields, repairs checksums,
and uses atomic replacement. Visitors remain SQLite-only; fixed `Save_t` arrays
are never enlarged.

The accelerated month soak repeatedly boots the same directory, connects four
clients, advances 31 days, runs hourly/daily jobs, alternates clean shutdowns
and simulated crashes, injects a torn journal write, and performs a final
independent recovery boot.
