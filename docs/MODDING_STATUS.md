# Modding — implementation status

Branch `modding`. Tracks what is actually built against the phase map in
`docs/MODDING_IMPLEMENTATION.md` §1.

| Phase | State |
|-|-|
| **P0** table `static_assert`s | **done** |
| **P1** Lua vendored, host, sandbox | **done** |
| **P2** calendar API, holidays, runtime wiring, persistence | **done** |
| **P3** `ModCalendar` replication | **server + wire done**; client presentation outstanding |
| **P4** T1 asset override | not started |
| **P5** arena, `.pcasset`, model compiler | not started |
| **P6** table growth, registry, placeholder | not started |
| **P7** delivery, server side | not started |
| **P8** client cache, fetch, loading UI | not started |
| **P9** custom songs and discs | not started |

Gate: `make check` exits 0, 75/75 tests, `client_link` pass at 20 objects, fuzz 50k pass.

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

**A player cannot see any of this yet.** The client-side registry and the three decomp call sites
are the remaining half of P3.

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

- **Protocol is now v23** and `kTownStateVersion` is 13. Both move fast — re-check before
  assuming a number.
- **`music_box` is 64 bits with 55 used** — 9 spare song slots before P9 forces a save and wire
  change. Master already replicates it (v22), so P9 builds on that rather than inventing it.
- **P8 is blocked on fuzzing the `.pcasset` parser.** It is the first point where the client
  parses bytes an arbitrary server chose.
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
