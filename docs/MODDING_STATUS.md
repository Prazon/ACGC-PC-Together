# Modding — implementation status

Branch `modding`. Tracks what is actually built against the phase map in
`docs/MODDING_IMPLEMENTATION.md` §1.

| Phase | State |
|-|-|
| **P0** table `static_assert`s | **done** |
| **P1** Lua vendored, host, sandbox | **done** |
| **P2** calendar API, holidays, runtime wiring, persistence | **done** |
| **P3** `ModCalendar` replication | **done** (server, wire and client) |
| **P4** T1 asset override | **done** |
| **P5** arena, `.pcasset`, model compiler | **container, parser and arena done**; model compiler outstanding |
| **P6** table growth, registry, placeholder | **blocked** — attempted and reverted, see below |
| **P7** delivery, server side | **done** (wire format, pack store, chunk service) |
| **P8** client cache, fetch, loading UI | **done** (cache, fetch loop, loading screen) |
| **P9** custom songs and discs | **registry and grant done**; audio playback outstanding |

Gate: `make check` exits 0 — 82/82 tests, `client_link` pass at 20 objects, protocol fuzz 50k,
`.pcasset` fuzz 50k, the content-cache check with its SHA-256 vectors, and the fetch-loop check.

---

## What works end to end today

Drop a mod into a town directory and start the server:

```
towns/default/mods/lantern-night/
  mod.toml     id / version / api_version / entry
  init.lua
```

```lua
calendar.register {
  id = 'lantern_night', name = 'holiday_name',
  date  = { month = 10, day = 7 },
  hours = { from = 18, to = 22 },
  rumor = { days_before = 6 },
}

calendar.on('holiday_begin', function(id)
  calendar.set_weather('clear')
  calendar.announce('banner')
end)

calendar.on('holiday_end', function(id)
  if calendar.load('awarded_year') == calendar.today().year then return end
  for _, account in ipairs(calendar.players_online()) do
    calendar.grant(account, 0x30A1)
  end
  calendar.store('awarded_year', calendar.today().year)
end)
```

The server discovers it, sandboxes it, resolves its holidays against the authoritative clock,
fires begin/end hooks on window edges, commits grants through the mail authority (journaled,
auditable), persists `store`/`load` in SQLite, and replicates the resolved calendar to clients.

The client stores the replicated calendar, and two decomp call sites draw from it: mod holidays
are marked on the in-game calendar and named from the wire.

**Not visually verified.** This environment has no SDL2, cmake or ninja, so the graphical client
cannot be built here. Every changed decomp TU compiles clean under the real build's flag set with
`NETCODE_ENABLED` both on and off, but nobody has seen a marker render. That is the first thing to
check on a machine with a disc image.

### Recurrence forms

```lua
date = { month = 10, day = 7 }                                  -- fixed
date = { month = 11, week = 4, weekday = 'thursday' }           -- Nth weekday
date = { month = 11, week = 4, weekday = 'thursday', days_after = 1 }
date = { month = 11, weekday = 'sunday', last = true }          -- last in month
date = { month = 6,  every = 'sunday' }                         -- every occurrence
date = { month = 3,  computed = 'vernal_equinox' }              -- or autumn_equinox
```

### Lua API (api_version 1)

`calendar.register` and `calendar.on` are load-time only. The rest are callable from hooks:
`today`, `weather`, `is_active`, `players_online`, `set_weather`, `announce`, `grant`,
`store`, `load`.

## Client-side asset override (P4)

Independent of the Lua framework in both directions:

```
mods/lantern-set/overrides/con_kaiwa2_w1_tex.bin
```

Any file whose name matches an entry in the generated asset table replaces the disc's copy. Sizes
must match exactly — the destination is a fixed-size array — and a mismatch is reported and
refused rather than truncated. Mods are scanned in id order so a conflict between two mods
resolves identically on every machine.

This is the override tier: it replaces existing assets and cannot add new ones. Adding is P5/P6.

## Content delivery (P5/P7/P8)

A town serves what its mods put under `mods/<id>/content/`. Everything else — `init.lua`,
`mod.toml`, `overrides/` — stays private.

```
AssetManifest      S->C, alongside the first baseline, only if the town has content
AssetChunkRequest  C->S, a bounded window; the server never sends unrequested data
AssetChunk         S->C, one chunk per packet, so delivery never fragments
```

Assets are named by the hash of their bytes. That gives cross-town dedup, makes verification and
identification the same operation (so no signature scheme is needed), and means a manifest never
supplies a filename — path traversal is structurally impossible rather than sanitised.

Verified end to end against a live server: a client joins over UDP, receives the manifest, pulls a
multi-chunk asset, reassembles it, and confirms it hashes back to what was advertised.

**The `.pcasset` parser never allocates.** Parsing yields borrowed views over the caller's buffer,
so a hostile file cannot make it allocate on its behalf. It is fuzzed in `make check` and
separately survived 200k iterations under ASan + UBSan.

**Still missing for a player to see content:** the client fetch loop that drives the cache from a
manifest, the mod arena and model compiler (P5), and the registry and table growth (P6).

## P6 is blocked, and why it matters

Growing the furniture tables at load (`MODLOADER_PLAN.md` §7.5 Option A) was attempted and
reverted. `ac_furniture_profile_data.c_inc` is included into **two** translation units, so each
currently gets a private `static` copy. Making the symbol a non-static pointer — the whole
mechanism Option A depends on — produces two definitions of one global, which is a
**duplicate-symbol link error**.

It nearly slipped through: `-fsyntax-only` passes, and both read sites compile unchanged, which is
exactly what Option A promises. The failure is only visible at link, and there is no linkable
client build in this environment.

**Consequence:** P6 needs each table made single-definition first (extern in a header, defined in
one TU). That is a larger decomp change than the plan assumed, and everything downstream —
runtime-added items, and therefore the model compiler and the client fetch loop having anything to
bind to — sits behind it.

**Verify with a link, not a syntax check**, and count the parent TUs of every other `.c_inc` table
before growing it.

## Custom songs (P9)

```lua
music.define { id = 'lantern_waltz', name = 'song_name', audio = 'waltz.ogg' }
music.grant(0, 55)   -- house slot 0, song id 55
```

**A town gets exactly nine custom songs.** A stereo's `music_box` is 64 bits and the original game
uses 55 (`MINIDISK_NUM`), so bits 55–63 are all that is free. The cap is the bitfield, not the
code, and the error message says so.

Slots are handed out in registration order — deterministic, because mod load order is — so the
same mod set yields the same song ids every start. That matters: the id is what a saved
`music_box` holds. Slots are **never reused**; the counter is monotonic, so a song defined after a
quarantine cannot land on a bit some stereo already holds. Renumbering would silently make every
such stereo play something else.

`music.grant` routes through `grant_house_song`, the same entry point the `--grant-song` operator
command uses, so it is journaled and bumps the same house revision a player action would.

**Audio playback is not implemented.** Mixing decoded PCM into `pc_audio.c`'s ring buffer needs
SDL, unavailable here. The registry and grant path work; the sound does not yet.

## Corrections made while building

Each came from measuring rather than trusting a prior description.

1. **The lockstep-table survey was wrong.** `FTR_NUM` is 1266, and only **two** furniture tables
   are declared `[]`. The other five are `[FTR_NUM]` and self-enforcing. P0 is 2 assertions, not
   the ~11 predicted.
2. **`packaging/server.ini` still said `capacity = 16`** while the code default had moved to 4,
   so the shipped template handed operators a sixteen-player town.
3. **`kMaxModStrings` was incoherent** at 128 against 64 holidays — each entry names exactly one
   string, so it can never exceed the holiday count.
4. **The calendar resource exceeds `kMaxPayloadBytes`** at full occupancy. That is by design
   (it rides fragmentation), but the default `ByteWriter` limit refused to encode it, so the
   writer now takes an explicit `kMaxModCalendarBytes`.

## Bugs caught by tests, worth knowing about

- A **dangling pointer**: the registration context was a stack local in `load_all`, but the Lua
  closure holding it outlives that frame. Both contexts now live in the `Mod`.
- A **malformed UTF-8 bounds check** that could read past the end of a truncated sequence.
- **February validated against a non-leap year**, which rejected 29 February outright instead of
  skipping it per-year.

## Constraints a future phase must not forget

- **Protocol is now v24** and `kTownStateVersion` is 13. Both move fast — re-check before
  assuming a number.
- **Mod calendar markers use event byte band 64..99** (`mSC_EVENT_MOD_BASE`, 36 slots). The stock
  values are the soncho enum plus 32 and 101-103, so that band is free. A town may declare more
  holidays (64) than it can mark (36); the marker pass stops at the band's end.
- **`music_box` is 64 bits with 55 used** — 9 spare song slots before P9 forces a save and wire
  change. Master already replicates it (v22), so P9 builds on that rather than inventing it.
- **The `.pcasset` fuzz gate is satisfied** and must stay that way: it is the first point where
  the client parses bytes an arbitrary server chose.
- **Bulk messages are session-scoped.** A chunk request built with session 0 is dropped before it
  reaches the service — the manifest arrives, no chunks do, and it looks like a broken service
  rather than a missing session id. This cost real debugging time; the P8 fetch loop must carry
  the session from `ServerHello`.
- **Lua must never enter `NET_SOURCES`.** `client-link` is the guard. Note that
  `town_runtime.cpp` *is* in `NET_SOURCES` and now calls the mod host, so every binary linking
  `NET_OBJECTS` needs `MOD_OBJECTS` and `LUA_OBJECTS` — the client does not, because it links a
  strict subset excluding the server-only TUs.

## Trying it by hand

```sh
make server
mkdir -p /tmp/town/mods/probe
printf 'id = "probe"\nversion = "1"\napi_version = 1\n' > /tmp/town/mods/probe/mod.toml
cat > /tmp/town/mods/probe/init.lua <<'LUA'
if io or os or require then error("sandbox leak") end
calendar.register { id = 'test', name = 'test_day', date = { month = 1, day = 1 } }
LUA
build/netcode/AnimalCrossingServer --smoke --ticks 120 --config packaging/server.ini --data /tmp/town
```

Expect `{"event":"mod_loaded","mod":"probe",...}` on stdout. A sandbox leak, a malformed
manifest, or a bad holiday spec each print a `mod_load_failed` line naming the cause, and the
town still starts.
