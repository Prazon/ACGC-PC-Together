# Dedicated town deployment

## Windows x86-64 release

Install the 64-bit MSYS2 packages listed in the root README, then run:

```bat
package_release.bat -Version local
```

The command builds with native CMake plus Ninja, verifies that both executables
are PE32+ x86-64, stages only runtime files, rejects ROM/save/server-state
files, writes SHA-256 hashes, and creates `dist/ACGC-PC-Port-local-Windows-x86_64.zip`.

The package contains empty `rom`, `save`, and `towns/default` directories. Each
player supplies a legitimate USA Rev 0 disc image in `rom`. No disc image or
Nintendo asset may be distributed with the client or server.

## Running a town

The invitation key is the town's only door lock, and it is optional. Leave
`invite_key` blank and the server starts open: it demands no proof from a
joining client and derives no session keys, so anyone who can reach the port may
join as any account and the traffic is unencrypted. The server prints a warning
at every such start. That is a reasonable choice on a LAN or behind a firewall
and a poor one for a port-forwarded host. Set a randomly generated private
`invite_key` in the host-side `server.ini` to close it, and expose only the UDP
game port (24680 by default):

```bat
AnimalCrossingServer.exe --config server.ini
```

Each player edits the client-side `network.ini` and launches normally. Every
player needs a different stable, nonzero account number:

```ini
[connection]
enabled = true
server = example.net:24680
town_id = 1
account_id = 1001
invite_key = "the-hosts-private-key"
```

Allow inbound UDP 24680 in the server firewall and forward that UDP port when
hosting behind NAT. Do not expose the town storage directory through a web or
file-sharing service.

On first boot the server writes `server.ini` beside the executable. On later
boots it loads that file before applying command-line overrides. Older
`towns/default/config.toml` files are read once and migrated automatically.
Supported settings are
`town_id`, `town_name`, `port`, `capacity` (1–16), `tick_rate`, `snapshot_rate`,
`connection_timeout_ms`, `build_id`, `timezone`, `utc_offset_minutes`,
`clock_mode`, `clock_scale`, `sync_to_system_clock`, `starting_datetime`, `allow_time_travel`,
`empty_town_simulation`, `data_directory`, `dashboard`, `invite_required`, and
`invite_key`, and `town_seed`. Keep the name at eight printable characters or
fewer and never change the seed after residents have created the canonical
town. Set an `invite_key` for any host reachable from the Internet and protect
the file because it may contain that shared secret. `ACGC_INVITE_KEY` or
`--invite-key` can override the stored key. A different file may be selected
with `--config FILE`.

`starting_datetime = "YYYY-MM-DD HH:MM:SS"` is the local civil time used only
when creating a town with no persisted clock. In `realtime` mode it then
advances at normal speed; `scaled` applies `clock_scale`, and `fixed` freezes
it. Restarting an existing town always resumes its stored authoritative time.

`sync_to_system_clock = true` slaves town time to the host system clock (plus
the configured timezone offset) on every tick, so the town always reads the
host's local date and time. It overrides `clock_mode`, `clock_scale`,
`starting_datetime`, and any drift left by an administrative time change,
including the stored time of an existing town — the server prints a notice at
startup when it overrides one of those. The default `realtime` mode with no
`starting_datetime` already tracks the system clock; the flag matters for towns
that were seeded with a custom start, run scaled or frozen, or were nudged with
a manual time set. Leave it `false` to keep a town's own persisted timeline. It
is orthogonal to `allow_time_travel`, which still governs whether a backwards
host clock is followed.

## Operations

On Windows the server creates a visible operator console when its launcher does
not provide one. It refreshes four times per second with the town identity,
authoritative date/time/weather, canonical-world status, four resident slots,
visitors, traffic/errors/jobs, and timestamped connection/bootstrap activity.
It writes structured JSON lifecycle events to standard output at the same time,
so redirecting logs does not blank the operator display. Stop it with Ctrl+C or
SIGTERM so it checkpoints and writes the clean-shutdown marker.

The first connected resident creates the canonical foreground using the stable
`town_seed`; later clients join that existing town and cannot overwrite it.
Online character creation skips Rover's local clock/town-name questions and
Tom Nook's onboarding, assigns the server resident slot and house immediately,
and uses a stable randomized starter face/shirt.

Administrative commands are one-shot processes and should be run while the
normal server is stopped:

```text
AnimalCrossingServer --data towns/default --checkpoint-now
AnimalCrossingServer --data towns/default --ban 1001
AnimalCrossingServer --data towns/default --unban 1001
AnimalCrossingServer --data towns/default --import-gci player-town.gci
AnimalCrossingServer --data towns/default --export-gci backup.gci
```

### Gifting bells and mail

Gifts address an account, and a town records an account the first time that
player connects, so start with the roster:

```text
AnimalCrossingServer --data towns/default --list-accounts
```

```text
  ACCOUNT   ROLE       NAME                  WALLET       BANK        DEBT  MAIL
  4242      resident1  Rosie                   1000      50000           0  1/10
```

Bells go straight into the bank account, never into the pocket, so the gift is
safe whether or not the player is mid-session:

```text
AnimalCrossingServer --data towns/default --grant-bells 4242=50000
```

Nook's upgrade level is derived from the town's lifetime sales, which accrue
only from committed `Buy` and `Sell` transactions -- granting bells does not
move it, because bells sitting in a bank have not been spent. To reach a tier
without shopping through it, set the total directly. Nookington's additionally
needs an outside shopper, which `--shop-visitor` records:

```text
AnimalCrossingServer --data towns/default --set-shop-sales 25000    # Nook 'n' Go
AnimalCrossingServer --data towns/default --set-shop-sales 90000    # Nookway
AnimalCrossingServer --data towns/default --set-shop-sales 240000 --shop-visitor
```

The shelf is rerolled at the same time, because its size is a function of the
tier and leaving Nookington's showing a five-row Cranny shelf would be a worse
lie than not upgrading. The current level is on the console and in the startup
banner.

A K.K. song goes into one house's stereo. `SLOT` is the original resident slot
0-3 and `SONG` is a bit index 0-63 into `mHm_hs_c::music_box`. The house must
already have been claimed by a player:

```text
AnimalCrossingServer --data towns/default --grant-song 0=5
```

A letter carries an optional item and an optional body of up to 192 bytes. The
item may be decimal or `0x`-prefixed; at least one of the two is required:

```text
AnimalCrossingServer --data towns/default --send-mail 4242 --mail-item 0x2203 \
    --mail-text "Thanks for keeping the town tidy."
AnimalCrossingServer --data towns/default --send-mail 4242 --mail-text "No attachment"
```

The letter waits in the recipient's mailbox until they take it out and then take
the present out of it -- the same two steps as the original game, and the second
is what moves the item into their pocket. A mailbox holds ten letters; an eleventh is
refused with a non-zero exit status rather than dropping the item quietly. Both
commands go through the same authority, journal, and revision rules a player
transaction uses, so a gift survives a restart, and `audit_log` in `town.db`
records it as `grant-bells` or `send-mail`.

For a complete backup, stop the server and copy the whole `towns/default`
directory. To restore, keep the failed directory for diagnosis, replace it with
the backup as one directory, and start the server. Startup validates checkpoint
checksums, replays the journal, repairs a torn final record, and runs elapsed
calendar jobs before accepting play.

## Source builds and validation

On Linux, `make server` builds the headless server. `make check` runs unit,
integration, parser fuzz, encrypted load/chaos, and server smoke tests.
`make month-soak` covers an accelerated 31-day calendar with clean and abrupt
restarts. `make sanitize` runs the unit/integration suite under ASan and UBSan.
With a legitimate disc in `pc/build64/bin/rom`,
`scripts/smoke_online_windows.ps1` launches the real Windows server and client,
verifies an authenticated online boot, then shuts both down automatically.
`scripts/smoke_two_clients_windows.ps1` launches two real clients concurrently,
checks both authenticated handshakes and the server's 2/16 dashboard state, and
uses no focus activation or synthetic input.

The long-running capacity soak is opt-in:

```sh
make long-soak
```
