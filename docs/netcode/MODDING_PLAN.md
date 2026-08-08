# Lua modding framework — plan

A modding framework for the **dedicated-town multiplayer server**. Mods are a property of the
town, not of the player: they run inside the authoritative server process, and clients receive
replicated results plus the strings needed to present them.

**Iteration 1 is custom holidays / calendar dates.** Custom tools (iteration 2) and custom
furniture (iteration 3) are sketched here so iteration 1's API does not paint them into a corner,
but they are explicitly out of scope for the first landing.

Nothing here is implemented. Companion reading: `docs/netcode/MASTER_PLAN.md` (authority model),
`docs/netcode/AUTHORITY_MATRIX.md`, `docs/CUSTOM_CONTENT_SYSTEMS.md` (the event scheduler this
framework drives), `docs/UI_SYSTEMS.md` (how mod strings reach the screen).

---

## 1. The decision that shapes everything

**The Lua VM runs server-side only. The client never executes mod code.**

Everything else in this document follows from that. The reasoning:

| Concern | Client-side VM | Server-side VM |
|-|-|-|
| Two players, different mod versions | disagree about what day it is | impossible — one town, one mod set |
| A mod that grants an item | is a cheat | is a server transaction, journaled and audited |
| Joining a modded town | requires the player to install matching mods | works with a stock client |
| Mod state across a crash | lives in a client save | lives in the journal + checkpoints |
| Holiday dates | derived from a client clock the server overwrites every frame | derived from the clock the server already owns |

That last row is the clincher. `TownClock` (`server/src/town_clock.cpp`) already owns town time,
the day boundary and the weather, and `Net_ApplyAuthoritativeClock` (`src/game/m_net_hooks.c:1079`)
overwrites the client's RTC from it every frame. A holiday is a predicate over town time.
**Server-side is where the input already lives.**

The client's job is presentation: draw the marker, say the line, show the banner. That is
`docs/netcode/AUTHORITY_MATRIX.md`'s "client-owned and unreplicated" row, and it needs no Lua.

### Non-goals for the whole framework

- **Client-side scripting.** Not in any planned iteration. If a mod needs new client behaviour
  that the presentation layer cannot express, that is a C change to the client, gated behind a
  capability the server advertises.
- **New meshes, textures or audio.** The client reads every asset from the user's disc image
  (`CLAUDE.md` → "no extraction step"). Adding art is a separate problem — see §8.1.
- **Replacing the original event scheduler.** Mods *add* to `event_schedule_data`'s vocabulary;
  they do not replace `m_event.c`.
- **Deterministic lockstep.** Already rejected project-wide. Mod randomness is server-side.

---

## 2. Architecture

```
towns/default/mods/
  lantern-night/
    mod.toml            manifest: id, version, api_version, entry
    init.lua            entry point
    strings/en.toml     display strings (holiday names, dialogue)
        │
        │  loaded at startup, sandboxed, never reloaded while players are online
        ▼
server/src/mod_host.cpp        VM lifetime, sandbox, instruction budget, error quarantine
server/src/mod_api_calendar.cpp  the `calendar` Lua table (iteration 1)
server/src/mod_registry.cpp    manifest parse, dependency + api_version check, content hashing
        │
        │  mods declare holidays; the host resolves them against TownClock each day
        ▼
server/src/town_runtime.cpp    owns the resolved holiday set; journals it; replicates it
        │
        │  ONE new ResourceKind + one new message. net/ stays Lua-free.
        ▼
net/src/replication.cpp        encode/decode ModCalendar deltas + the mod manifest digest
        │
        ▼
src/game/m_net_hooks.c         applies the holiday set into a client-side registry
        │
        ▼
presentation: calendar marker, Tortimer line, PC banner   (docs/CUSTOM_CONTENT_SYSTEMS.md)
```

### 2.1 Where the dependency lands

`net/CLAUDE.md` is explicit: *"Do not add a dependency here that the client does not already
link."* So:

| Layer | May depend on Lua? | Why |
|-|-|-|
| `net/` | **No** | must stay buildable with no deps; the client links it |
| `server/` | **Yes** | already links SQLite, which the client does not (`pc/CMakeLists.txt:579`) |
| `src/`, `pc/` | **No** | client never runs mod code |

Lua 5.4 vendored under `third_party/lua/` (MIT, ~30 files, no build system of its own — compiles
as a plain source list, same treatment as the vendored fixNES at `pc/CMakeLists.txt:320`). Both
build paths need the source list added: the root `Makefile`'s `NET_SOURCES` gains a parallel
`LUA_SOURCES` compiled with `CFLAGS` minus `-Werror` (upstream Lua is not warning-clean under
`-Wpedantic`), and `pc/CMakeLists.txt` links it into the `AnimalCrossingServer` target only.

**Alternative worth costing before committing:** LuaJIT is faster but has a much heavier build and
weaker sandboxing story. For per-day callbacks measured in microseconds, stock Lua 5.4 is the
right call.

---

## 3. Mod package format

```
lantern-night/
├── mod.toml
├── init.lua
└── strings/
    └── en.toml
```

```toml
# mod.toml
id          = "lantern-night"      # unique, [a-z0-9-], becomes the namespace prefix
name        = "Lantern Night"
version     = "1.0.0"
api_version = 1                    # framework API this mod targets
entry       = "init.lua"
authors     = ["..."]
# Optional
requires    = []                   # other mod ids
conflicts   = []
```

`api_version` is the compatibility gate. The host refuses to load a mod whose `api_version` it
does not implement, rather than letting it fail at the first missing function. The framework
version is independent of `kProtocolVersion`.

### 3.1 Strings

Mod-defined display text is the first hard problem, and it is worth being blunt about it:
**the client has no way to render a string that is not in the disc's `RESOURCE_STRING` archive**
(`docs/CUSTOM_CONTENT_SYSTEMS.md` §9, constraint 6). Holiday names are exactly that.

The plan:

```toml
# strings/en.toml
holiday_name    = "Lantern Night"
tortimer_greet  = "Lovely evening for lanterns, isn't it?"
banner          = "Lantern Night - 6PM to 10PM at the beach"
```

1. The server transcodes each string from UTF-8 into the game's custom 256-codepoint encoding
   (`include/m_font.h:14-273`) at load time, using the same table `pc_utf8_to_game_code`
   (`pc/src/pc_typing.c:37`) already implements for keyboard input. Unmappable characters are a
   **load-time error**, not a runtime surprise.
2. Transcoded strings ship to the client in the `ModCalendar` baseline as length-prefixed byte
   arrays.
3. The client stores them in a small fixed registry and feeds them to the synthesised-message
   path (`docs/CUSTOM_CONTENT_SYSTEMS.md` §4) and to `pc_text_draw` for PC-side UI.

Bounds, enforced on both sides: ≤ 32 strings per mod, ≤ 128 bytes each, ≤ 4 KB per mod total.
These are wire-format limits and belong in `acnet/types.hpp` next to the other constants.

---

## 4. Sandbox and safety

A mod must not be able to hang, crash, or exfiltrate from a town server. The threat model is a
careless mod author, not a hostile one — but the mitigations are the same.

| Control | Mechanism |
|-|-|
| No filesystem, process, network | Load into a fresh `lua_State` with a whitelist environment: `assert error ipairs pairs next select tonumber tostring type unpack math string table`. **Excluded:** `io os package require dofile loadfile load debug collectgarbage rawset rawget setmetatable`. |
| No wall clock | `os` is absent entirely. Mods read town time via `calendar.today()`. A mod cannot observe real time, which is what keeps behaviour reproducible across a restart or a `--time` override. |
| Bounded CPU | `lua_sethook(L, LUA_MASKCOUNT, budget)` per callback. Proposed default 200 000 instructions for `on_day_start`, 20 000 for per-hour hooks; configurable per server. Exceeding it aborts *that callback*, not the process. |
| Bounded memory | Custom `lua_Alloc` with a per-mod ceiling (proposed 4 MB). Allocation past the cap returns NULL, which Lua turns into a catchable error. |
| Deterministic randomness | `math.random` replaced with a host RNG seeded from server state. Mods never call `secure_random` directly. |
| No cross-mod reach | One `lua_State` per mod. Mods communicate only through host APIs, never shared globals. |
| Error containment | Every callback runs under `lua_pcall`. An error is logged with the mod id as structured JSON on stdout (matching the existing lifecycle output convention) and counted. |
| Quarantine | N consecutive errors in the same hook (proposed N=3) disables that mod for the remainder of the process lifetime and emits a warning. **The town keeps running.** A holiday mod that throws must never take a town offline. |
| Immutable while online | Mods load once at startup. No hot reload while players are connected — a mid-session change to the holiday set would desync the calendar clients already hold. Reload is a restart. |

### 4.1 What a mod is still trusted with

Sandboxing bounds resource use; it does not make a mod *safe to install*. A mod can legitimately
grant items and bells (that is the point). The framework mitigates by routing every grant through
the existing `EconomyAuthority` path, so it is journaled, revision-bumped and written to
`audit_log` exactly like the `--grant-bells` admin command already is (`server/CLAUDE.md` →
Operations). A town operator installing a mod is making the same trust decision as running an
admin command; the audit trail is what makes it reviewable.

---

## 5. Iteration 1 — custom holidays

### 5.1 Lua API surface

Deliberately small. Everything is under one `calendar` table.

```lua
-- Registration (call during load only)
calendar.register(spec)          -- declare a holiday; returns a handle
calendar.on(event_name, fn)      -- subscribe to a lifecycle hook

-- Queries (call any time)
calendar.today()                 -- { year, month, day, weekday, hour, min }
calendar.is_active(id)           -- is this holiday live right now?
calendar.weather()               -- "clear" | "cloudy" | "rain" | "snow"

-- Effects (call from hooks only)
calendar.set_weather(kind)       -- force weather for the day; server-committed
calendar.announce(string_key)    -- push a banner to every connected player
calendar.grant(account, item)    -- routed through EconomyAuthority + audit_log

-- Persistence
calendar.store(key, value)       -- per-mod KV, survives restart (numbers/strings/bools)
calendar.load(key)
```

The `spec` passed to `register` mirrors the vocabulary `event_schedule_data` already supports
(`docs/CUSTOM_CONTENT_SYSTEMS.md` §2.1), because that is the vocabulary the client's scheduler
understands:

```lua
{
  id      = "lantern_night",   -- namespaced to <mod-id>.<id> by the host
  name    = "holiday_name",    -- key into strings/*.toml
  -- exactly one recurrence form:
  date    = { month = 10, day = 7 },                        -- fixed date
  -- or  = { month = 11, week = 4, weekday = "thursday" },  -- Nth weekday
  -- or  = { month = 11, week = 4, weekday = "thursday", days_after = 1 },
  -- or  = { month = 6,  every = "sunday" },                -- weekly
  -- or  = { month = 9,  computed = "autumn_equinox" },     -- engine-computed dates
  hours   = { from = 18, to = 22 },
  marker  = true,              -- draw it on the in-game calendar
  rumor   = { days_before = 6 },
}
```

Hooks for iteration 1:

| Hook | Fires | Typical use |
|-|-|-|
| `on_load` | once, at server start, after registration | validate config, seed state |
| `on_day_start` | at each town-day boundary | decide today's variable content |
| `on_hour` | hourly | timed sub-phases |
| `on_holiday_begin(id)` | a registered holiday's window opens | announce, set weather |
| `on_holiday_end(id)` | its window closes | award, clean up |

All five map onto the two `ScheduledJob`s `town_runtime.cpp:573-604` already registers, plus one
new job for holiday-window edges. No new scheduling machinery.

### 5.2 Worked example — the complete iteration-1 mod

```lua
-- mods/lantern-night/init.lua
-- Lantern Night: 7 October, 6PM-10PM. Clear skies, a banner, and a
-- commemorative lantern for anyone online when it ends.

local LANTERN = 0x30A1   -- an existing item id; see §8.1 on why not a new one

calendar.register {
  id     = "lantern_night",
  name   = "holiday_name",
  date   = { month = 10, day = 7 },
  hours  = { from = 18, to = 22 },
  marker = true,
  rumor  = { days_before = 6 },
}

calendar.on("holiday_begin", function(id)
  if id ~= "lantern_night" then return end
  calendar.set_weather("clear")
  calendar.announce("banner")
end)

calendar.on("holiday_end", function(id)
  if id ~= "lantern_night" then return end
  local year = calendar.today().year
  if calendar.load("awarded_year") == year then return end   -- idempotent
  for _, account in ipairs(calendar.players_online()) do
    calendar.grant(account, LANTERN)
  end
  calendar.store("awarded_year", year)
end)
```

Three things this example is designed to demonstrate:

- **Registration is declarative.** The recurrence, the marker and the rumour window are data the
  host can resolve without calling back into Lua — which is what lets the resolved holiday set be
  computed once per day, journaled, and replicated as plain bytes.
- **Idempotency is the mod author's job, and the API makes it easy.** `calendar.store` /
  `calendar.load` exist precisely so the award survives a mid-holiday restart without
  double-granting. The host cannot infer this.
- **Nothing touches the client.** Weather, the banner and the grant are all server-committed;
  the client learns each through a channel it already has.

### 5.3 Replication design

One new `ResourceKind`, **appended** — the enum is encoded as a `u8` and validated against the
last value (`net/include/acnet/replication.hpp:19-51`), so insertion is a wire break:

```cpp
enum class ResourceKind : std::uint8_t {
    /* ... existing values, unchanged ... */
    Turnip,
    /* The town's mod-declared calendar: which custom holidays exist this year,
     * when each is active, and the transcoded strings to name them. Town-wide
     * and slow-moving -- resolved once per town day -- so it rides the normal
     * delta path rather than the snapshot. */
    ModCalendar,
};
```

Payload, following the `encode_shop_delta` / `decode_shop_delta` pattern in
`net/src/replication.cpp` and going through `ByteWriter`/`ByteReader` field by field:

```cpp
struct ModHoliday {
    std::uint16_t id = 0;            /* host-assigned, stable within a town */
    std::uint8_t  month = 0;
    std::uint8_t  day = 0;
    std::uint8_t  hour_from = 0;
    std::uint8_t  hour_to = 0;
    std::uint8_t  flags = 0;         /* marker | rumor_active | live_now */
    std::uint8_t  name_string = 0;   /* index into the strings block */
};

struct ModCalendarState {
    Revision revision = 0;
    std::uint16_t year = 0;                    /* the year these dates resolve for */
    std::array<std::uint8_t, 32> manifest_digest{};  /* SHA-256 of the active mod set */
    std::vector<ModHoliday> holidays;          /* <= kMaxModHolidays */
    std::vector<std::vector<std::uint8_t>> strings;  /* transcoded, <= 128 bytes each */
};
```

Bounds in `acnet/types.hpp` alongside the existing constants:

```cpp
constexpr std::size_t kMaxModHolidays = 64;
constexpr std::size_t kMaxModStrings = 128;
constexpr std::size_t kMaxModStringBytes = 128;
constexpr std::size_t kMaxModCalendarBytes = 8192;   /* whole resource, pre-fragmentation */
```

At 64 holidays × 7 bytes + 128 strings × ~24 bytes average, the resource sits around 3–4 KB —
over `kMaxPlaintextPayloadBytes` (1136), so **it must go through `fragmentation.cpp`**, which is
already the documented path for oversized payloads (`net/CLAUDE.md`). Send it on the Bulk channel
at baseline and on Events for the once-per-year re-resolve.

`kProtocolVersion` goes 20 → 21, and `docs/netcode/PROTOCOL.md` is updated in the same change —
`net/CLAUDE.md` requires the code and the doc to move together.

### 5.4 Client presentation

The client applies `ModCalendarState` into a small registry in `m_net_hooks.c` and exposes it
through three read-only queries that the existing systems already know how to consume:

| Client need | New query | Consumed by |
|-|-|-|
| "is a custom holiday live?" | `Net_ModHolidayActive(index)` | event manager `start_proc` equivalents |
| "what is today's custom holiday called?" | `Net_ModHolidayName(index, u8* out)` | calendar overlay, synthesised messages |
| "mark this day on the calendar" | `Net_ModHolidayForMonth(month, ...)` | `m_calendar_ovl.c` marker pass |

Three touch points in `src/`, each small and hook-shaped:

1. **Calendar marker** — `mCD_make_calendar_data_fixed_day_event` (`m_calendar_ovl.c:303`) gains a
   loop over `Net_ModHolidayForMonth` after its static table, under `#ifdef NETCODE_ENABLED`.
2. **Name lookup** — `mSC_get_event_name_str` (`m_soncho.c:296`) consults the mod registry before
   the ROM string table. This is the PC-side override table `docs/CUSTOM_CONTENT_SYSTEMS.md`
   Example A identified as necessary anyway; the mod framework is what finally justifies it.
3. **Banner** — `calendar.announce` arrives as a delta and calls the PC-side banner module
   (`docs/CUSTOM_CONTENT_SYSTEMS.md` Example D), which lives entirely in `pc/` and costs no
   decomp edit.

Every one of these has a no-op macro fallback under `#else`, so `NETCODE_ENABLED=OFF` compiles
unchanged — the standing rule in `src/CLAUDE.md`.

**A stock client connecting to a modded town degrades cleanly**: it does not understand
`ResourceKind::ModCalendar`, ignores the delta (the decoder already skips unknown kinds rather
than erroring), and sees a normal town with no custom holidays. Only the protocol version gate
applies.

### 5.5 Persistence

Two stores, matching what the data actually is:

| Data | Store | Why |
|-|-|-|
| Resolved holiday set for the current year | `TownRuntime::encode_state` → journal + checkpoint | it is town state; must be identical after a restart mid-holiday |
| Per-mod KV (`calendar.store`) | new SQLite table `mod_state(mod_id, key, value)` | unbounded-ish, queryable, and already the right tool |

`kTownStateVersion` goes 12 → 13 (`server/src/town_state.cpp:17`). An older checkpoint simply has
no mod calendar; the next day-start job resolves one — the same "older checkpoint has none"
pattern the turnip schedule already uses. The SQLite table is a new numbered migration in
`schemas/database/`, applied transactionally at startup.

A new journal record type in the 100–199 band that `town_runtime.cpp:565` treats as town state —
say **130 = mod calendar resolved**, keeping it clear of the 100/110–113/121/122 values already in
use.

**Durability rule that must hold:** `calendar.grant` reports success to the mod only after the
underlying economy transaction is journaled and flushed, exactly as a player transaction does.
A mod must never be told a grant succeeded that a crash then loses.

### 5.6 Work inventory

| # | Change | Files | Rough size |
|-|-|-|-|
| 1 | Vendor Lua 5.4 | `third_party/lua/`, `Makefile`, `pc/CMakeLists.txt` | build plumbing |
| 2 | Manifest parse, registry, content hashing | `server/src/mod_registry.cpp` + header | ~300 lines |
| 3 | VM host: sandbox, budgets, pcall wrappers, quarantine | `server/src/mod_host.cpp` + header | ~500 lines |
| 4 | `calendar` API bindings | `server/src/mod_api_calendar.cpp` | ~400 lines |
| 5 | UTF-8 → game codepoint transcoder (server side) | `server/src/mod_strings.cpp` | ~150 lines |
| 6 | `ResourceKind::ModCalendar` + encode/decode + bounds | `net/`: `types.hpp`, `replication.hpp/.cpp` | ~200 lines |
| 7 | Resolve-per-day job, state encode/decode, journal record | `server/src/town_runtime.cpp`, `town_state.cpp` | ~250 lines |
| 8 | SQLite `mod_state` migration | `schemas/database/` | 1 file |
| 9 | Client registry + 3 queries + no-op fallbacks | `src/game/m_net_hooks.c`, `include/m_net_hooks.h` | ~150 lines |
| 10 | Calendar marker + name override + banner | `m_calendar_ovl.c`, `m_soncho.c`, `pc/src/pc_event_banner.c` | ~150 lines |
| 11 | Protocol doc + deployment doc | `docs/netcode/PROTOCOL.md`, `DEPLOYMENT.md` | docs |

### 5.7 Tests

`net/CLAUDE.md` requires a round-trip case for any new message type and survival under
`make fuzz`. Proposed additions to `tests/net/test_main.cpp`'s registry (following the naming in
`test_main.cpp:5137-5161`):

| Test | Asserts |
|-|-|
| `"mod calendar replicates"` | resolved holidays reach a client, survive a baseline, and match after a re-resolve |
| `"mod calendar round trip"` | encode/decode is lossless at the bounds (64 holidays, 128 max-length strings) |
| `"mod errors do not stop the town"` | a mod throwing in every hook is quarantined; the town continues serving |
| `"mod budgets are enforced"` | an infinite loop in `on_day_start` aborts that callback only |
| `"mod grants are journaled"` | `calendar.grant` survives a simulated crash and does not double-apply |
| `"mod state survives a restart"` | `calendar.store` round-trips through checkpoint + journal replay |

The month-soak harness (`tests/load/town_month_soak.cpp`) is the natural home for the last two —
it already runs an accelerated 31-day calendar with clean and crash restarts, which is exactly the
shape of a holiday bug. **`make check` must stay green with a test mod installed.**

---

## 6. Iteration 2 stub — custom tools

Sketch only. The purpose here is to check that iteration 1's API shape does not obstruct this.

```lua
-- Re-parameterising an existing tool id, NOT introducing a new mesh.
tools.define {
  id       = "golden_lantern",
  base     = tools.BASE_NET,        -- which original tool's behaviour + model
  name     = "tool_name",
  durability = 60,                  -- nil = unbreakable
  price    = 4800,
  sell     = 1200,
  -- Server-validated behaviour hooks:
  on_use   = function(account, target)     -- return true to allow
    if not calendar.is_active("lantern_night") then return false, "refuse_msg" end
    return true
  end,
  on_catch = function(account, species)    -- fires after the server commits
    if species == species.FIREFLY then calendar.grant(account, LANTERN) end
  end,
}
```

**Authority note:** `on_use` is a *validator*, not an effect. It runs before the server commits an
encounter or world transaction and can only veto. The commit itself stays in
`net/src/encounter.cpp` / `world.cpp`, which is what keeps the existing validation chain
(identity, zone, distance, tool, cooldown, revision) intact.

Client side: tool behaviour is `m_player_item_*.c_inc` and is deeply wired into the player state
machine. A custom tool that reuses `base`'s animations and model needs no client change beyond the
name string. **A tool with new animations does not fit this framework** and should be a C feature.

---

## 7. Iteration 3 stub — custom furniture

```lua
furniture.define {
  id       = "lantern_set",
  base     = furniture.BASE_LAMP,   -- existing FTR id: model, footprint, rotation set
  name     = "ftr_name",
  series   = "lantern",             -- for set-bonus rules
  price    = 3200, sell = 800,
  -- Availability, evaluated server-side:
  sold_by  = { shop = "nook", from = { month = 10, day = 1 }, to = { month = 10, day = 31 } },
  catalog  = true,
}
```

Furniture is the better second target than tools, because a furniture item is mostly *data*:
placement footprint, price, series membership, catalog presence and shop availability are all
tables the server can own. Behaviour is minimal.

The blocker is the same one: **the model comes from the disc.** `base` selects an existing
`FTR0_*` / `FTR1_*` id whose mesh is reused.

---

## 8. Cross-cutting constraints

### 8.1 The asset wall

This is the single most important limitation to communicate to mod authors, and it should be
stated in the framework's own README, not discovered:

> A mod can change what an item *is called*, *costs*, *does* and *when it appears*. It cannot
> change what it *looks like*.

The client renders from the user's disc image. There is no asset-injection path today. The
partial escape hatch is the existing HD texture-pack loader (`pc/src/pc_texture_pack.c`), which
replaces textures by content hash — so a mod *could* ship a texture pack that reskins the base
item. That is a per-client install, outside the server-authoritative model, and should be treated
as a separate optional feature rather than part of the mod package.

### 8.2 Item ID space

`mActor_name_t` is a `u16` with a 4-bit type nibble (`ITEM_NAME_GET_TYPE`,
`include/m_name_table.h:205`), giving 4096 ids per type. Whether there is contiguous headroom
above `ITM_TOOL_START` (0x2200) and inside the `FTR0`/`FTR1` ranges **has not been surveyed** and
must be before iteration 2 commits to allocating custom ids. If headroom is thin, the fallback is
a server-side indirection table mapping mod item handles onto base ids, which costs a lookup on
every inventory operation but needs no id space at all.

### 8.3 Mod identity and compatibility

The `manifest_digest` in `ModCalendarState` is the SHA-256 of the sorted `(id, version, content
hash)` triples of the active mod set. Uses:

- The client can show "this town runs 3 mods" and log which.
- A future client-side companion pack (textures, extra strings) can verify it matches the server's
  mod set before applying.
- Support: a bug report carries the digest, which pins the exact mod set.

The digest is **advisory in iteration 1** — a mismatch never refuses a connection. Making it
binding is a policy decision for later, and would need a way for players to obtain mods.

### 8.4 Interaction with the existing scheduler

Mod holidays and `event_schedule_data` coexist; they do not merge. The client-side 16-slot daily
cap (`mEv_TODAY_EVENT_NUM`, `docs/CUSTOM_CONTENT_SYSTEMS.md` §9 constraint 1) applies **only to
the original scheduler**. Mod holidays live in their own registry and do not consume those slots —
which is a real advantage of routing them through replication rather than by synthesising
`event_schedule_data` rows on the client.

Where they *do* interact: `calendar.set_weather` writes the same authoritative weather the
original weather-forcing events (`mEv_EVENT_WEATHER_CLEAR`) drive. Last writer in the day wins,
and the resolution order must be defined — proposal: original events resolve first, mods second,
so a mod can deliberately override a stock event's weather.

---

## 9. Open questions

Flagging these rather than guessing, because each changes the design materially:

1. **Should mods be able to define NPC dialogue?** The synthesised-message path makes it
   technically easy, but dialogue is where the game's voice lives and a bad mod is very visible.
   A restricted form — a mod supplies text, Tortimer delivers it on a mod holiday — is the safe
   subset and is what iteration 1's `strings` block already supports.
2. **Per-player vs per-town mod state.** `calendar.store` is per-town. Per-player state
   (`"has this account attended Lantern Night?"`) needs an account-scoped table and a decision
   about what happens when a visitor leaves. Iteration 1 could ship without it; iteration 2
   probably cannot.
3. **Do we need a mod API for reading the original event schedule?** A mod that wants to avoid
   clashing with the Fishing Tourney needs to see it. Cheap to add (`calendar.stock_events()`),
   but it exposes an enum that is positional and may shift.
4. **Distribution.** Out of scope here, but if the manifest digest ever becomes binding, players
   need a way to fetch mods. Worth deciding *before* the digest goes binding, not after.

---

## 10. Sequencing

| Phase | Deliverable | Gate |
|-|-|-|
| 1a | Lua vendored, VM host, sandbox, quarantine, a mod that only logs | `make check` green; sandbox escape tests pass |
| 1b | `calendar.register` + per-day resolution + journal/checkpoint | month-soak green with a test mod |
| 1c | `ResourceKind::ModCalendar`, protocol 21, round-trip + fuzz | `make fuzz`, round-trip test |
| 1d | Client registry, calendar marker, name override | Windows smoke with a modded server |
| 1e | `announce` banner, `set_weather`, `grant` + audit | full release gate |
| 2 | Custom furniture (data-shaped, lower risk than tools) | — |
| 3 | Custom tools (validators + behaviour hooks) | — |

Phase 1a is worth landing on its own. It is the part with all the safety-critical machinery and
none of the wire-format risk, and a town running a do-nothing mod for a week is the cheapest
possible proof that the host does not destabilise anything.

Per `CLAUDE.md` → Workflow, `docs/netcode/CURRENT_STATUS.md` gets updated at each phase — with no
git history, that file is the project's memory.
