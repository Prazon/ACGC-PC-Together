# Modding — technical implementation plan

The buildable specification for the three-part modding system: **server-side Lua**, **client-side
asset injection**, and **server-to-client asset delivery**.

This document is the *how*. The *why* and the architectural rationale live in:

- `docs/netcode/MODDING_PLAN.md` — server-side Lua, authority model, holidays
- `docs/MODLOADER_PLAN.md` — asset tiers, the lockstep table problem, delivery design
- `docs/CUSTOM_CONTENT_SYSTEMS.md` — the event scheduler the calendar API drives
- `docs/UI_SYSTEMS.md` — the display-list buckets the loading screen draws into

Read those first for context. This one assumes them and gets concrete: file lists, declarations,
wire layouts, phase gates, and test names.

**Scope note (1.0):** the town capacity default is now **4** (`ServerConfig::capacity`,
`packaging/server.ini`). The wire bound `kMaxPlayersPerZone` stays at 16 — it is a decoder
validation limit, not a policy — so raising capacity later is a config edit, not a protocol bump.
All bandwidth and load figures below assume 4 concurrent clients.

---

## 1. Phase map and gates

Nine phases. Each has a hard gate; **do not start the next phase until the current gate is
green.** Phases marked ◆ are independently shippable — they leave the tree in a better state even
if the rest is abandoned.

| Phase | Deliverable | Gate | Ship? |
|-|-|-|-|
| **P0** | Lockstep table `static_assert`s | `make check` green; a deliberately-short table fails the build | ◆ **done** (`8122b17`) |
| **P1** | Lua vendored + host + sandbox | Sandbox escape suite passes; a no-op mod loads; `make check` green | ◆ |
| **P2** | `calendar` API, holiday resolution, persistence | `make month-soak` green with a test mod; grant survives crash | |
| **P3** | `ModCalendar` replication + client presentation | Round-trip + fuzz; Windows smoke shows a custom holiday marker | ◆ |
| **P4** | T1 asset override | A replaced model renders; size mismatch is a clean load error | ◆ |
| **P5** | Mod arena + `.pcasset` + model compiler | A hand-packed model renders from a file, offline | |
| **P6** | Table growth + registry + placeholder/hot-swap | A new item exists, is priced, catalogued, and swaps from base → real | |
| **P7** | Delivery protocol (server side) | Chunk service under load; gameplay traffic unaffected | |
| **P8** | Client cache + fetch + loading UI | Cold-join a modded town, play through placeholders, assets land | ◆ |
| **P9** | Custom songs and discs (§11b) | A custom disc plays on a stereo; silence-with-title before its audio lands | |

`make check` must stay green at **every** gate, not just at the end.

---

## 2. Build system

### 2.1 Vendored Lua 5.4

```
third_party/lua/
  lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c
  lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c
  ltm.c lundump.c lvm.c lzio.c
  lauxlib.c lbaselib.c lcorolib.c lmathlib.c lstrlib.c ltablib.c lutf8lib.c
  *.h
```

**Deliberately excluded** (they are the sandbox holes): `liolib.c`, `loslib.c`, `loadlib.c`,
`ldblib.c`, `linit.c`. Excluding them at the *build* level rather than the environment level means
an accidental `luaL_openlibs` cannot reintroduce them — the symbols do not exist.

Root `Makefile`:

```makefile
LUA_SOURCES := $(wildcard third_party/lua/*.c)
LUA_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LUA_SOURCES))
# Upstream Lua is not clean under our flags. Compile it with its own set.
LUA_CFLAGS := -std=c99 -O2 -g -DLUA_USE_POSIX -MMD -MP
$(BUILD_DIR)/third_party/lua/%.o: third_party/lua/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -Ithird_party/lua -c $< -o $@
```

`LUA_OBJECTS` links into `AnimalCrossingServer` and `netcode_tests` **only**. Not into
`AnimalCrossing`. `pc/CMakeLists.txt` mirrors this on the `AnimalCrossingServer` target.

`make client-link` (already in the Makefile) is the guard that proves the client still links
without server-only translation units — it must keep passing.

### 2.2 New source files

| File | Phase | Purpose | Est. |
|-|-|-|-|
| `server/src/mod_registry.cpp` | P1 | manifest parse, discovery, load order, hashing | 300 |
| `server/src/mod_host.cpp` | P1 | VM lifetime, sandbox, budgets, pcall, quarantine | 500 |
| `server/src/mod_strings.cpp` | P1 | UTF-8 → game codepage transcode | 150 |
| `server/src/mod_api_calendar.cpp` | P2 | the `calendar` Lua table | 400 |
| `server/src/mod_calendar_state.cpp` | P2 | resolution, journal, SQLite KV | 350 |
| `server/src/mod_packstore.cpp` | P7 | content-addressed blob store + manifest build | 350 |
| `server/src/mod_chunk_service.cpp` | P7 | chunk request handling, pacing | 300 |
| `net/src/mod_messages.cpp` | P3/P7 | `ModCalendar` + `Asset*` encode/decode | 500 |
| `pc/src/pc_modloader.c` | P4 | discovery, manifest, override index | 350 |
| `pc/src/pc_mod_arena.c` | P5 | high-arena allocator for mod assets | 150 |
| `pc/src/pc_mod_assets.c` | P5 | `.pcasset` reader + validation | 400 |
| `pc/src/pc_mod_model.c` | P5 | container → `Vtx`/`Gfx`/texture | 600 |
| `pc/src/pc_mod_registry.c` | P6 | handle → profile, placeholder state, hot-swap | 300 |
| `pc/src/pc_mod_cache.c` | P8 | hash store, index, LRU quota | 400 |
| `pc/src/pc_mod_fetch.c` | P8 | diff, windowed pull, verify, install | 350 |
| `pc/src/pc_mod_loadscreen.c` | P8 | progress UI | 250 |
| `tools/converters/obj2gfx.py` | P5 | OBJ → C (build-time tier) | 400 |
| `tools/converters/mod_pack.py` | P5 | glTF/OBJ → `.pcasset` | 800 |

Headers alongside each. Total ≈ 6 400 lines C/C++ + 1 200 Python.

---

## 3. P0 — the safety net

**Do this first regardless of whether the rest proceeds.** Per `docs/MODLOADER_PLAN.md` §3.1,
per-furniture tables are indexed without bounds checks; a short table is a wild pointer
dereference, not a missing item.

New header `include/m_ftr_tables.h`, included by each parent TU after its `.c_inc`:

```c
/* Every per-furniture table is indexed by the FTR enum without bounds checks.
 * A mismatch is undefined behaviour at runtime, so make it a build error. */
#define FTR_TABLE_ASSERT(tbl) \
    _Static_assert(sizeof(tbl) / sizeof((tbl)[0]) == FTR_NUM, \
                   #tbl " must have exactly FTR_NUM entries")
```

Applied to each of the tables inventoried in `docs/MODLOADER_PLAN.md` §3. Where a table's natural
length legitimately differs (e.g. `ftr_price_table[]` carries a trailing `-1` sentinel, so it is
`FTR_NUM + 1`), assert the actual invariant rather than bending the table:

```c
FTR_TABLE_ASSERT_N(ftr_price_table, FTR_NUM + 1);   /* + sentinel */
```

**Gate:** `make check` green, and deliberately deleting one entry from any asserted table fails
the build with a readable message.

### 3.1 Outcome (implemented, commit `8122b17`)

The survey came out better than this plan assumed. `FTR_NUM` is **1266**, and of the
furniture-indexed tables **five are declared `[FTR_NUM]`** — the declaration enforces their length
and a short initialiser merely zero-fills, so they need no assertion:

```
ftrArt              m_item_name.c:246
mMkRm_ftr_info      m_mark_room_ovl.c:153
mMkRm_ftr_info      m_huusui_room_ovl.c:29
mRmTp_ftr_se_type   m_room_type.c:75
mRmTp_birth_type    m_room_type.c:520
```

Only two are declared `[]` and take their length from the initialiser, and those got the asserts:

| Table | Assert | Why it matters |
|-|-|-|
| `furniture_quality[]` | `FTR_TABLE_ASSERT` | indexed unchecked at `m_catalog_ovl.c:170`, then `profile->vtable` dereferenced — a short table is a wild pointer |
| `ftr_price_table[]` | `FTR_TABLE_ASSERT_SENTINEL` (`FTR_NUM + 1`) | sentinel-checked at runtime, so not unsafe, but a short table silently prices furniture at 0 |

So **P0 is 2 assertions, not 11.** The "~11 unchecked tables" figure came from a description of
another fork and did not survive measurement. `include/m_ftr_tables.h` carries the macros and the
reasoning; `tools/count_init_entries.py` is the brace-aware counter used to establish the lengths.

Verified in both directions: removing one price entry, and widening the expected
`furniture_quality` length, each fail the build naming the table. `make test` 53/53.

---

## 4. P1 — Lua host and sandbox

### 4.1 Manifest

```toml
id = "lantern-night"          # [a-z0-9-]{1,32}, unique, namespace prefix
name = "Lantern Night"
version = "1.0.0"             # semver
api_version = 1               # framework API; refused if unknown
entry = "init.lua"
requires = []
conflicts = []
```

```cpp
// server/include/acserver/mod_registry.hpp
struct ModManifest {
    std::string id, name, version, entry;
    std::uint32_t api_version = 0;
    std::vector<std::string> requires_ids, conflicts_ids;
    std::array<std::uint8_t, 32> content_hash{};  // over all files, sorted by path
    std::filesystem::path root;
};

class ModRegistry {
public:
    bool scan(const std::filesystem::path& mods_dir, std::string& error);
    const std::vector<ModManifest>& load_order() const;   // topological, then by id
    std::array<std::uint8_t, 32> manifest_digest() const; // over (id, version, hash) sorted
};
```

Load order is **deterministic**: topological sort over `requires`, ties broken by `id`
lexicographically. Never by directory iteration order — that varies by filesystem and would make
a bug irreproducible.

### 4.2 Host

```cpp
// server/include/acserver/mod_host.hpp
struct ModLimits {
    std::size_t memory_bytes   = 4u * 1024 * 1024;
    int instr_on_load          = 1'000'000;
    int instr_on_day           = 200'000;
    int instr_on_hour          =  20'000;
    int consecutive_errors_to_quarantine = 3;
};

class ModHost {
public:
    bool load_all(const ModRegistry&, const ModLimits&, std::string& error);
    // Returns false if the mod errored or was quarantined. Never throws.
    bool call_hook(const std::string& mod_id, const char* hook, const HookArgs&);
    bool quarantined(const std::string& mod_id) const;
    const ModMetrics& metrics() const;
};
```

**Allocator** — the memory cap, enforced at the source:

```cpp
static void* mod_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* st = static_cast<ModAllocState*>(ud);
    if (nsize == 0) { st->used -= osize; std::free(ptr); return nullptr; }
    if (st->used - (ptr ? osize : 0) + nsize > st->limit) return nullptr;  // Lua raises
    void* np = std::realloc(ptr, nsize);
    if (np) st->used = st->used - (ptr ? osize : 0) + nsize;
    return np;
}
```

**Instruction budget** — `lua_sethook(L, hook, LUA_MASKCOUNT, budget)` where the hook calls
`luaL_error`. Set per call, not per VM, because `on_load` legitimately needs more than `on_hour`.

**Environment whitelist** — build the global table explicitly; never call `luaL_openlibs`:

```
assert error ipairs pairs next select tonumber tostring type
math (minus math.random, replaced)  string  table
```

Absent by construction: `io os package require dofile loadfile load debug
collectgarbage setmetatable rawset rawget rawequal rawlen coroutine`.

`math.random` is replaced with a host RNG seeded from server state, so a mod's randomness is
reproducible from a checkpoint. `os` being absent is what prevents a mod observing wall-clock
time — it reads town time through `calendar.today()` only.

**Every** callback goes through `lua_pcall`. An error is logged as structured JSON on stdout
(matching the existing lifecycle output convention), counted, and after
`consecutive_errors_to_quarantine` in the same hook the mod is disabled for the process lifetime.
**The town keeps running.**

### 4.3 String transcoding

`mod_strings.cpp` ports the mapping in `pc/src/pc_typing.c:37` (`pc_utf8_to_game_code`) to the
server. Records are 16 bytes, space-padded, in the game codepage
(`docs/MODLOADER_PLAN.md` §2.7). An unmappable character is a **load-time error naming the mod,
the key and the offending character** — never a runtime surprise.

### 4.4 Tests (P1 gate)

Added to `tests/net/test_main.cpp`:

| Name | Asserts |
|-|-|
| `"mod sandbox denies escapes"` | `io`, `os`, `require`, `load`, `debug` are all nil; a mod trying each fails cleanly |
| `"mod memory cap holds"` | a mod allocating in a loop errors instead of exhausting the host |
| `"mod instruction budget holds"` | an infinite loop aborts that callback only, town continues |
| `"mod errors quarantine"` | 3 consecutive errors disable the mod; 4th hook is not called |
| `"mod load order is deterministic"` | same set → same order across runs, independent of FS order |

---

## 5. P2 — calendar API and holidays

### 5.1 Lua surface (api_version 1)

```lua
calendar.register(spec) -> handle      -- load time only
calendar.on(event, fn)                 -- load time only
calendar.today() -> {year,month,day,weekday,hour,min}
calendar.is_active(id) -> bool
calendar.weather() -> "clear"|"cloudy"|"rain"|"snow"
calendar.players_online() -> {account, ...}
calendar.set_weather(kind)             -- hooks only, server-committed
calendar.announce(string_key)          -- hooks only
calendar.grant(account, item) -> bool  -- hooks only, EconomyAuthority + audit_log
calendar.store(key, value)             -- number|string|bool
calendar.load(key) -> value
```

Hooks: `load`, `day_start`, `hour`, `holiday_begin(id)`, `holiday_end(id)`.

The `spec` recurrence forms mirror `event_schedule_data`'s vocabulary
(`docs/CUSTOM_CONTENT_SYSTEMS.md` §2.1) — exactly one of:

```lua
date = { month=10, day=7 }
date = { month=11, week=4, weekday="thursday" [, days_after=1] }
date = { month=6, every="sunday" }
date = { month=9, computed="autumn_equinox" }   -- or "vernal_equinox", "harvest_moon"
```

### 5.2 Resolution

One new `ScheduledJob` alongside the existing `npc-schedules` and `daily-renewal`
(`server/src/town_runtime.cpp:573-604`):

```cpp
ScheduledJob mod_day;
mod_day.name = "mod-calendar";
mod_day.interval_seconds = 24 * 60 * 60;
mod_day.next_due = ((town_time / day_seconds) + 1) * day_seconds;
mod_day.maximum_catchups = 64;
```

Callback: resolve every registered holiday against the town year → `ModCalendarState`, fire
`holiday_begin`/`holiday_end` edges, journal, publish the delta.

Holiday-window edges within a day are checked on the existing hourly job — a holiday's start and
end hours are integers, so hourly resolution is exact.

### 5.3 Persistence

| Data | Store | Mechanism |
|-|-|-|
| Resolved `ModCalendarState` | journal + checkpoint | `commit_state(130, error)`; `kTownStateVersion` 12 → 13 |
| `calendar.store` KV | SQLite | new migration `schemas/database/00NN_mod_state.sql` |

```sql
CREATE TABLE IF NOT EXISTS mod_state (
    mod_id TEXT NOT NULL,
    key    TEXT NOT NULL,
    value  BLOB NOT NULL,
    PRIMARY KEY (mod_id, key)
) WITHOUT ROWID;
```

Record type **130** is chosen to sit clear of the values already in use (100, 110–113, 121, 122)
inside the 100–199 town-state band recognised at `town_runtime.cpp:565`.

An older checkpoint has no mod calendar; the next `mod-calendar` job resolves one. This is the
same forward-compatible pattern the turnip schedule already uses.

**Durability rule:** `calendar.grant` returns `true` to Lua only after the underlying economy
transaction is journaled and flushed. A mod must never be told a grant succeeded that a crash then
loses.

### 5.4 Tests (P2 gate)

| Name | Asserts |
|-|-|
| `"mod holidays resolve per year"` | fixed / Nth-weekday / weekly / computed forms all land on the right day |
| `"mod state survives a restart"` | `calendar.store` round-trips through checkpoint + journal replay |
| `"mod grants are journaled"` | grant survives a simulated crash and does not double-apply |
| `"mod holiday edges fire once"` | `holiday_begin`/`end` fire exactly once per occurrence, including across a restart mid-window |

Extend `tests/load/town_month_soak.cpp` to install a test mod — it already runs an accelerated
31-day calendar with clean and crash restarts, which is precisely the shape of a holiday bug.

---

## 6. P3 — ModCalendar replication

### 6.1 Wire format

Append to `ResourceKind` (never insert — it is a validated `u8`):

```cpp
enum class ResourceKind : std::uint8_t { /* … */ Turnip, ModCalendar, };
```

Bounds in `acnet/types.hpp`:

```cpp
constexpr std::size_t kMaxModHolidays     = 64;
constexpr std::size_t kMaxModStrings      = 128;
constexpr std::size_t kMaxModStringBytes  = 128;
constexpr std::size_t kMaxModCalendarBytes = 8192;
```

Payload, field-by-field through `ByteWriter`/`ByteReader` (never `memcpy` a struct —
`net/CLAUDE.md`):

```
u32  revision            (!= 0)
u16  year
u8   manifest_digest[32]
u8   holiday_count       (<= kMaxModHolidays)
  repeated:
    u16 id
    u8  month            (1..12)
    u8  day              (1..31)
    u8  hour_from        (0..23)
    u8  hour_to          (0..23)
    u8  flags            (bit0 marker, bit1 rumor, bit2 live_now; others must be 0)
    u8  name_string      (< string_count)
u8   string_count        (<= kMaxModStrings)
  repeated:
    u8  length           (<= kMaxModStringBytes)
    u8  bytes[length]
```

Decoder rejects: truncation, count overflow, out-of-range dates, reserved flag bits set,
`name_string` out of range, total size > `kMaxModCalendarBytes`. Returns a `ResultCode`, never
throws.

Sent on `Channel::Bulk` at baseline, `Channel::Events` on re-resolve. At 64 holidays × 7 bytes
plus 128 strings it exceeds `kMaxPlaintextPayloadBytes` (1136), so it **goes through
`fragmentation.cpp`** — the documented path for oversized payloads.

`kProtocolVersion` 20 → 21, and `docs/netcode/PROTOCOL.md` updated in the same commit.

### 6.2 Client application

Registry in `m_net_hooks.c` plus three queries in `include/m_net_hooks.h`, each with a no-op macro
fallback under `#else`:

```c
int Net_ModHolidayActive(int index);
int Net_ModHolidayName(int index, u8* out, int out_len);   /* game codepoints */
int Net_ModHolidayForMonth(int month, int slot, int* day_out, int* name_index_out);
```

Three decomp call sites:

| Site | File | Change |
|-|-|-|
| Calendar marker | `mCD_make_calendar_data_fixed_day_event`, `m_calendar_ovl.c:303` | loop over `Net_ModHolidayForMonth` after the static table |
| Holiday name | `mSC_get_event_name_str`, `m_soncho.c:296` | consult the mod registry before the ROM table |
| Banner | new `pc/src/pc_event_banner.c` | `calendar.announce` delta → banner |

**A stock client degrades cleanly:** it does not know `ResourceKind::ModCalendar`, the decoder
skips unknown kinds rather than erroring, and it sees an ordinary town.

### 6.3 Tests (P3 gate)

`"mod calendar round trip"` (lossless at bounds), `"mod calendar replicates"` (reaches a client,
survives a baseline), plus `make fuzz` coverage of the new decoder.

---

## 7. P4 — asset override (T1)

`pc/src/pc_modloader.c`, scanning `mods/*/overrides/`:

```c
int  pc_modloader_init(void);                       /* before pc_assets_init */
const void* pc_mod_override(const char* bin_path, unsigned int expect_size);
```

One branch at the head of `pc_load_asset` (`pc/src/pc_assets.c:80`), ahead of the ROM read.
**Size must match exactly** — the destination is a fixed-size static array. A mismatch is a
startup error naming the mod, the asset and both sizes; not a silent truncation.

Two mods overriding the same asset is a **conflict**: report both mod ids and refuse to start.
Silent last-wins would make a broken install impossible to diagnose.

**Gate:** a replaced model renders; a deliberately wrong-sized override fails at startup with a
readable message; `--verbose` lists every active override.

---

## 8. P5 — arena, container, model compiler

### 8.1 Arena

```c
/* pc/include/pc_mod_arena.h */
int   pc_mod_arena_init(size_t bytes);   /* from the high VirtualAlloc/mmap arena */
void* pc_mod_arena_alloc(size_t bytes, size_t align);
void  pc_mod_arena_reset(void);          /* shutdown only */
```

**Must** come from the ≥ `0x10000000` arena that `pc/DOCUMENTATION.md` describes, not plain
`malloc` — a low heap pointer can be misread by `emu64::seg2k0` as an N64 segment address.
Mod assets are resident for the process lifetime; there is no per-scene free.

Sized at init from the loaded mod set, with a configurable ceiling (default 64 MB).

### 8.2 `.pcasset` container

```
offset  size  field
0       4     magic 'PCAS'
4       2     version (=1)
6       2     chunk_count
8       …     chunks, each: u32 tag, u32 byte_length, payload

tags:
  'VTX ' u32 count, then count × { s16 x,y,z; s16 u,v; u8 r,g,b,a }
  'TEX ' u16 w, u16 h, u8 format, u8 reserved[3], then pixel data
  'PAL ' u16 entries, then entries × u16
  'MSH ' u32 tri_count, then tri_count × { u16 a, b, c }
  'META' f32 bounds[6], f32 suggested_scale
```

Validation **before** allocation, at every step:

| Field | Bound |
|-|-|
| `chunk_count` | ≤ 32 |
| `byte_length` | ≤ remaining file bytes |
| vertex `count` | ≤ 65 535 |
| `tri_count` | ≤ 65 535, every index < vertex count |
| `w`, `h` | powers of two, ≤ 1024 |
| `format` | one of the decoders in `pc_gx_texture.c:584-593` |
| whole file | ≤ 4 MB |

A 32-bit count must never become a 64 GB allocation. This parser is the P8 attack surface; write
it defensively now so P7.7 hardening is a review rather than a rewrite.

### 8.3 Model compiler

`pc_mod_model.c` turns a validated container into runtime structures in the arena:

1. `Vtx` array in the layout `pc_text_draw.c:90` writes.
2. Texture upload through the existing `pc_gx_texture.c` path.
3. A `Gfx` display list built with the normal macros; heap pointers go through
   `pc_gbi_pack_runtime_ptr` (`pc/src/pc_gbi_runtime.c:11`) so `seg2k0` resolves them.
4. `aFTR_PROFILE` populated: `opaque0`, `texture`, `palette`, `scale`, `height`; everything else
   inherited from `base`.

**Gate:** a hand-packed `.pcasset` on disk renders in the model viewer (`--model-viewer`), with no
network and no server involved. This isolates the whole art path from the delivery path.

---

## 9. P6 — table growth, registry, placeholder

### 9.1 Growing the lockstep tables (Option A)

Per `docs/MODLOADER_PLAN.md` §7.5. The mechanism, applied identically to each table:

```c
/* Before, in ac_furniture_profile_data.c_inc: */
static aFTR_PROFILE* furniture_quality[] = { ... };

/* After: */
static aFTR_PROFILE* furniture_quality_base[] = { ... };
aFTR_PROFILE**       furniture_quality = furniture_quality_base;   /* non-static */
size_t               furniture_quality_count = FTR_NUM;
```

At mod-load time, once, before any scene:

```c
void pc_mod_tables_grow(size_t extra) {
    size_t n = FTR_NUM + extra;
    aFTR_PROFILE** grown = pc_mod_arena_alloc(n * sizeof(*grown), 8);
    memcpy(grown, furniture_quality_base, FTR_NUM * sizeof(*grown));
    for (size_t i = FTR_NUM; i < n; i++) grown[i] = /* base profile of that mod item */;
    furniture_quality = grown;                       /* single pointer store */
    furniture_quality_count = n;
}
```

Every existing `furniture_quality[i]` read compiles and behaves unchanged, because the symbol is
now a pointer. **No read site is touched**, which is the entire point of choosing Option A — the
~11 unchecked indexers stay correct by construction.

The P0 assertions become `_Static_assert(ARRAY_COUNT(furniture_quality_base) == FTR_NUM, …)` and
keep doing their job on the base table.

### 9.2 Registry and hot-swap

```c
/* pc/include/pc_modloader.h */
const aFTR_PROFILE* pc_mod_furniture_profile(mActor_name_t item);
int  pc_mod_item_icon(mActor_name_t item, u8** tex_out, u16** pal_out);
int  pc_mod_item_name(mActor_name_t item, u8* out, int out_len);
```

Three states per item: not-a-mod-item (NULL) → placeholder (base profile) → resident (real).

**Atomic publish.** Build the complete `aFTR_PROFILE` in the arena, then publish with a single
relaxed atomic pointer store. A reader sees either the old or the new pointer, never a partial
struct. Do not rely on incidental atomicity of an aligned store — make it explicit.

**Swap only at safe points.** Apply pending swaps at scene transitions and when the submenu
closes; both are existing synchronisation points where nothing holds a cached profile. The
hazard is real: `mCL_dma_furniture_program` (`m_catalog_ovl.c:170`) caches the pointer in
`mCL_Item_c::profile` and dereferences it later.

**Never free a base profile.** It is static, so a stale cached pointer stays valid — the item
simply renders as its placeholder until something re-reads. That is the backstop that makes the
worst case cosmetic.

**Gate:** a mod item exists end-to-end offline — appears in the catalogue at the right price,
places in a room, shows its name and icon, and swaps from base to real model when its asset is
installed by hand.

---

## 10. P7 — delivery, server side

### 10.1 Pack store

Built once at startup, not per connection:

```
towns/default/modpack/
  <hex-hash>      one file per blob
  manifest.bin    precomputed AssetManifest payload
```

Serving a chunk is a bounded `pread` from a blob file. With capacity 4, the worst case is four
clients cold-joining together — comfortably served, but the external-mirror hook (§10.4) should
still exist from day one.

### 10.2 Messages

```cpp
enum class MessageType : std::uint16_t {
    /* … */
    AssetManifest     = 60,   // S→C
    AssetChunkRequest = 61,   // C→S
    AssetChunk        = 62,   // S→C
    AssetStatus       = 63,   // C→S, advisory
};
```

```
AssetManifest:
  u32 revision
  u8  manifest_digest[32]
  u16 entry_count             (<= kMaxAssetEntries = 4096)
    repeated: u8 hash[32], u32 size, u16 kind, u16 item_handle

AssetChunkRequest:
  u8  hash[32]
  u32 first_chunk
  u16 chunk_count             (<= kAssetWindowChunks = 16)

AssetChunk:
  u8  hash[32]
  u32 index
  u32 total_chunks
  u16 length                  (<= kAssetChunkBytes)
  u8  bytes[length]
```

`kAssetChunkBytes` = `kMaxPlaintextPayloadBytes` minus the 42-byte chunk header, so **one chunk is
one packet** and fragmentation is never involved in delivery.

### 10.3 Pacing

Client-paced pull: the server never sends unrequested chunk data. `AssetChunkRequest` gets its own
token bucket in `allow_message` (`server/src/town_runtime.cpp:698`), sized so a full window refill
is comfortable and a flood is not:

```cpp
else if (type == acnet::MessageType::AssetChunkRequest) { rate = 8.0; burst = 24.0; }
```

**Gameplay traffic wins.** The asset channel is serviced only after transactions and snapshots for
that tick, and skipped entirely on any tick where the outbound budget is already spent.

### 10.4 External mirror

`asset_base_url` in `server.ini`. The manifest carries hashes either way; the client verifies the
hash regardless of source, so a mirror changes nothing about integrity. Design the manifest to
allow it now even if client-side HTTP lands later.

**Gate:** `tests/load/town_load.cpp` extended with a modded town — four bots cold-join, pull a
synthetic 8 MB pack, and snapshot/transaction latency stays within its existing bounds.

---

## 11. P8 — client cache, fetch, UI

### 11.1 Cache

```
<user data>/modcache/
  ab/ab3f9c…d21     blob, filename == hash
  index.db          hash → size, last_used, origin town
```

Global, not per-town — a second town using the same model downloads nothing. (The privacy
trade-off is logged as an open question in `docs/MODLOADER_PLAN.md` §12.6.)

LRU eviction against a configurable quota (default 256 MB). Never evict a blob referenced by the
current town's manifest.

### 11.2 Fetch loop

```
on AssetManifest:
    missing = [e for e in manifest if not cache.has(e.hash)]
    order   = sort(missing, by=(kind_priority, size))   # name → icon → model
    join the town immediately                            # never blocks
    while missing:
        request a window of <= kAssetWindowChunks
        on each blob complete:
            verify hash → install to cache → pc_mod_registry_mark_resident(handle)
```

Priority `name → icon → model` matters: an item can show its correct name and icon in the pocket
while its world model is still a placeholder, which is the part players notice.

### 11.3 Loading screen

Optional and off once the cache is warm. Offered — not imposed — when the missing set exceeds a
threshold (proposed 2 MB or 20 blobs). **"Play now" is always present and always works.**

Implementation is the PC menu stack from `docs/UI_SYSTEMS.md` §7: `pc_text_draw` into
`font_thaga`, `pc_menu_util` for layout, drawn from `graph_main` next to `pc_pause_menu_draw`
(`src/graph.c:390`). No decomp change.

### 11.4 Security gate — blocking

**P8 does not ship until the `.pcasset` parser has been through `make fuzz`.** Add a
`tests/fuzz/pcasset_fuzz.cpp` harness modelled on `protocol_fuzz`, and wire it into the `fuzz`
target. This is the one ordering constraint in the plan that is a security requirement rather than
a convenience: P8 is the first time the client parses bytes an arbitrary server chose.

Also required before shipping P8:

- Trust prompt on first connect to a town that wants to send assets; remembered per town.
- Size caps enforced before allocation: 4 MB per blob, 64 MB per manifest, 256 MB cache.
- Confirmation that a pack can contain **no executable content** — no Lua, no bytecode, no
  shaders. That property is what bounds the worst case to a parser bug, and it must be asserted
  by the container format (unknown chunk tags are skipped, never executed).

---

## 11b. P9 — custom songs and discs

Playable on the in-game stereos. Surveyed against the tree; this is a **later phase than the
numbering suggests** — it depends on P6 (item registration) and P8 (delivery) — but it is
specified here because it constrains earlier choices.

### 11b.1 How music works today

| Fact | Location |
|-|-|
| 55 minidisk items, `ITM_MINIDISK_START = 0x2A00` | `include/m_name_table.h:166,2642` |
| A stereo is furniture holding the disc in `ftr_actor->items[0]` | `ac_my_room.c:1699,1740` |
| Disc → track is linear: `BGM_MD0 + (item - ITM_MINIDISK_START)` | `ac_my_room.c:2113` |
| Owned songs are a **64-bit `music_box` bitfield**, already server-owned | `TownRuntime::grant_house_song(slot, song)` |
| The music overlay marks owned discs from that bitfield | `m_music_ovl.c:65-80` |
| PC audio is a lock-free ring buffer feeding an SDL callback | `pc/src/pc_audio.c:22,68` |

Two properties make this tractable:

- **The server already owns song ownership.** `grant_house_song` and the `music_box` bitfield
  exist and replicate, so "which songs does this town have" needs no new authority work — a Lua
  mod calling `music.grant(slot, song)` fits the existing path exactly.
- **Custom audio is easier than original audio.** ROM tracks are JAudio *sequences*; a custom
  song is decoded PCM. Mixing PCM into `pc_audio.c`'s ring buffer is strictly simpler than
  synthesising a sequence, and needs no JAudio work at all.

### 11b.2 The binding constraint

`music_box` is **64 bits and 55 are used.** That leaves **9 spare song slots** before the
bitfield must widen — and widening it is a save-format *and* wire-format change touching
`HouseState`, the GCI mapping, and the checkpoint version.

**Decision to make before P9 starts:** ship custom songs in the 9 spare slots (cheap, hard cap of
9 per town), or widen `music_box` to 128 bits as part of the same change (more expensive, but
does it once). Given `docs/netcode/EXTENDED_RESIDENTS_PLAN.md` is already going to touch
`Save_t`, **bundling the widening with that work is probably right** rather than doing it twice.

### 11b.3 Design

```lua
-- mods/lantern-night/init.lua
music.define {
  id     = "lantern_waltz",
  name   = "song_name",              -- key into strings/*.toml
  audio  = "lantern_waltz.ogg",      -- mono/stereo, resampled at pack time
  loop   = { start_sample = 88200 }, -- optional loop point
  source = music.SOURCE_KK,          -- how it can be obtained
}
```

| Layer | Work |
|-|-|
| Pack | `.ogg` → decoded PCM blob at pack time (§8.2 container gains an `AUD ` chunk), content-addressed like any other asset |
| Delivery | Nothing new — an audio blob is a blob (§8.3–8.5). Priority tier below models: a disc the player does not own yet is not urgent |
| Server | `music.define` registers a song id; `music.grant(slot, song)` reuses `grant_house_song`; the id → bit mapping is part of the manifest digest |
| Client | New minidisk item ids via the P6 registry; `BGM_MD0 + offset` intercepted so a custom id routes to the PCM player instead of JAudio |
| Audio | A `pc_mod_music.c` PCM source mixed into `pc_audio.c`'s ring buffer, with the same loop-point and fade behaviour the stereo expects |
| Placeholder | A disc whose audio has not arrived plays **silence with the correct title showing**, not a fallback song — a wrong song is worse than none |

### 11b.4 Risks specific to P9

| Risk | Mitigation |
|-|-|
| 9-slot cap reached | decide the widening question up front (§11b.2) |
| Audio blobs dominate pack size | an `.ogg` minute is ~1 MB decoded to ~10 MB PCM — **store compressed, decode on demand**, not at pack time |
| Music continues across a scene change | reuse the stereo's existing stop/fade path rather than a parallel one |
| Copyright | out of scope technically, but the delivery system means a server operator is *distributing* audio; worth a line in the host-facing docs |

**Correction to §8.2 implied by this:** the `.pcasset` container must carry compressed audio and
decode lazily. Storing decoded PCM would make a ten-song mod larger than every model in the game
combined.

---

## 12. Test plan summary

New cases in `tests/net/test_main.cpp` (names as they should appear in the registry at
`test_main.cpp:5137`):

```
"mod sandbox denies escapes"            P1
"mod memory cap holds"                  P1
"mod instruction budget holds"          P1
"mod errors quarantine"                 P1
"mod load order is deterministic"       P1
"mod holidays resolve per year"         P2
"mod state survives a restart"          P2
"mod grants are journaled"              P2
"mod holiday edges fire once"           P2
"mod calendar round trip"               P3
"mod calendar replicates"               P3
"asset manifest round trip"             P7
"asset chunks resume after reconnect"   P7
"asset delivery yields to gameplay"     P7
"asset cache dedups across towns"       P8
"pcasset parser rejects malformed"      P8
```

New harnesses: `tests/fuzz/pcasset_fuzz.cpp` (P8). Extended: `town_month_soak` with a test mod
(P2), `town_load` with a modded town (P7).

`make check` gains nothing new by default — the mod tests live in the existing suite and the
fuzz target grows a second parser.

---

## 13. Risk register

| Risk | Likelihood | Impact | Mitigation |
|-|-|-|-|
| P0 uncovers existing table mismatches | high | low | that is the point; fix each on its merits before P6 |
| Model compiler is larger than estimated | high | medium | P5 gate is offline-only, so it cannot block P1–P4 shipping |
| Table growth breaks an unnoticed read site | medium | **high** | Option A touches no read sites by design; P6 gate exercises catalogue, room placement and pockets |
| Hot-swap tears a cached profile | medium | high | atomic publish + safe-point swap + never free base (§9.2) |
| `.pcasset` parser bug reachable from the network | medium | **high** | P8 blocked on fuzzing; data-only container; caps before allocation |
| Delivery starves gameplay | low | high | client-paced pull; gameplay-first scheduling; P7 gate measures it |
| Lua build not warning-clean under `-Werror` | certain | low | separate `LUA_CFLAGS` (§2.1) |
| Scope creep into client-side scripting | medium | high | explicit non-goal; the C API surface is the three lookups in §9.2 |

---

## 14. Sequencing

```
P0  ─────────────────────────────────────────────► ship (protects the tree today)
 └─ P1 ──► P2 ──► P3 ─────────────────────────────► ship (server mods + holidays)
 └─ P4 ───────────────────────────────────────────► ship (asset override)
      └─ P5 ──► P6 ────────────────────────────────► (new items, local install)
                 └─ P7 ──► P8 ────────────────────► ship (server-delivered content)
```

P1–P3 and P4–P5 are independent and can run in parallel. P6 is the join point for asset work; P7
depends on P6 having something to deliver; P8 depends on P7 and is gated on fuzzing.

Per `CLAUDE.md` → Workflow, update `docs/netcode/CURRENT_STATUS.md` at each phase gate — with no
git history, that file is the project's memory.
