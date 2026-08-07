# Extended residents and free house placement

Status: proposed. No code written.

Goal: 16 persistent residents, with houses placed away from the single baked
four-house acre.

## 0. Decision record

Settled with the maintainer on 2026-08-06:

| Decision | Choice |
|-|-|
| Dolphin / original-save compatibility | **Not required.** `Save_t` may change shape. |
| Single-player compatibility | **Not required.** |
| Placement | Clusters now (4 acres × 4 houses, dispersed); arbitrary per-house placement is a later phase, so the data model is designed for it today |
| Cluster 0 | Disperses like the rest; no acre is pinned |
| House upgrades for residents 5–16 | Full parity from the first phase |
| Visitors | Not supported for now. 16 residents, no visitor role |
| Migration | **None.** Pre-1.0, so a fresh town is required. No legacy save, checkpoint, or database is carried forward |
| GCI subsystem | **Deleted** — see §4.1. Inferred from the two decisions above, not instructed; easy to keep if wanted |
| Offline / single-player | **Not supported.** This is a multiplayer mod; the server is required to play. The `NETCODE_ENABLED=OFF` build and the entire local-save stack go with it — see §4 |
| Cluster count | **Always 4 clusters / 16 slots**, regardless of configured session capacity |
| Freeing a resident slot | Host-run one-shot `--evict ACCOUNT`; no automatic inactivity reclaim |
| Character creation | Out of scope. The seeded-random appearance stays as a stopgap; tracked separately |

Dropping GCI compatibility is the decision that reshapes everything below. An
earlier draft of this plan built an elaborate three-tier projection — server
registry, a 4-entry client window, and a presentation side table — purely to
keep `Save_t` byte-compatible. **All of that is now unnecessary.** The direct
approach is available: raise `PLAYER_NUM` to 16.

The trade is real but favourable. The old constraint was architectural (you
cannot grow the array, so build indirection around it). The new constraint is
mechanical (`PLAYER_NUM` and its assumptions are load-bearing in ~190 places,
and roughly 38 of them are `& 3` masks that will silently wrap). Mechanical and
greppable beats architectural and pervasive — but it is not free, and §3 is the
part of this plan most likely to bite.

## 1. What the old constraint was

For the record, since `AGENTS.md` and `MASTER_PLAN.md` §25 still forbid this:

| Fact | Value | Source |
|-|-|-|
| `sizeof(mHm_hs_c)` | `0x26B0` (9,904 B) | `include/m_home_h.h:155` |
| `homes[PLAYER_NUM]` | `0x9CE8`, spans `0x9AC0`, ends exactly at `fg[]` | `include/m_common_data.h:94` |
| `sizeof(Private_c)` | `0x2440` (9,280 B) | `server/src/gci.cpp:18` |
| `sizeof(Save_t)` | `0x242A0` (148,128 B) | `server/src/gci.cpp:20` |
| Sector-aligned save region | `0x26000` | `include/m_card.h:128` |
| GCI file data | `0x72000` = others + main + backup | `include/m_card.h:17` |

At 16, `homes[]` grows by 118,848 B and `private_data[]` by 111,360 B, taking
`Save_t` to roughly `0x5CB40` (~379 KB) and `common_data_t` to ~418 KB. On a
GameCube memory card that was fatal. On PC it is irrelevant.

**`AGENTS.md` and `MASTER_PLAN.md` §25 must be amended** as part of this work —
leaving "do not expand fixed arrays inside `Save_t`" in place while doing
exactly that is worse than either choice on its own.

## 2. Design

`PLAYER_NUM` goes from 4 to 16 (`include/m_personal_id.h:13`). `homes[16]`,
`private_data[16]`, `mother_mail[16]`, and `keep_house_size[16]` become real
arrays, and the ~172 `homes[` sites across ~45 files keep working unchanged.

Authority is unaffected: the server still owns every persistent outcome. What
changes is that the client's `Save_t` becomes purely a **replica** the server
writes into — filled by the authoritative-application hooks that already exist
for inventory, clock, and weather (`src/game/m_net_hooks.c`), never written to
disk, and never a local authority. With offline play dropped there is no second
mode, so no accessor needs an online/offline branch.

Interiors are still streamed on demand (§7) — not because of a size limit, but
because pushing 16 × 9,904 B of furniture at every join is ~158 KB per client of
avoidable baseline traffic, and because a client should not hold every other
player's house contents before it has any reason to.

## 3. The `& 3` audit — the main hazard

Raising `PLAYER_NUM` does not fix code that hardcodes the number 4. The
dangerous pattern is `& 3`, which silently wraps houses 4–15 onto 0–3 instead of
crashing. There are **38** such sites in the house/home files alone:

| File | Notable |
|-|-|
| `src/game/m_home.c` | `home_no & 3` at lines 236, 327, 328, 330–333, 336–339 |
| `src/game/m_house.c:60` | `Save_Get(homes + (home_no & 3))` |
| `src/game/m_cockroach.c` | 5 sites, incl. `mHS_get_arrange_idx(player_no) & 3` |
| `src/game/m_mark_room.c` | 17 `homes[` sites, mask-bearing |
| `src/game/m_room_type.c`, `src/actor/ac_my_room.c`, `ac_my_indoor.c`, `ac_haniwa_move.c_inc` | remainder |

Each must be reviewed individually — some `& 3` masks are legitimately about a
room index or a 2-bit bitfield, not a house number. A blanket `& 15` rewrite
would be as wrong as leaving them. Add a CI grep gate so no new `& 3` appears in
these files.

Three more 4-assumptions that are not `& 3`:

1. **`house_arrangement` is a `u8`**, 2 bits per player
   (`include/m_common_data.h:104`, `src/game/m_house.c:9`). 16 players at 4 bits
   each needs a `u64`. `ARRANGE_GET`/`ARRANGE_MOVE`/`DEFAULT_ARRANGEMENT` and
   `mHS_set_use`'s swap mask all change width.
2. **`outlook_pal = i`** in `src/game/m_start_data_init.c:250,361` — there are
   only 12 palettes (`mHm_OUTLOOK_PAL_NUM`, `include/m_home_h.h:71`). At 16 this
   writes invalid palettes 12–15. Must become `i % mHm_OUTLOOK_PAL_NUM`.
3. **`l_mHm_player_room_default_data[PLAYER_NUM]`**
   (`src/game/m_home.c:312`) has four hand-authored wall/floor defaults. Either
   author twelve more or cycle `% 4`.

Also touched, lower risk: the four `src/save_check*.c_inc` validators, and
`src/player_select.c` (the title-screen slot UI shows four slots; online it is
bypassed, offline it would need to page).

## 4. Save format

### 4.1 The GCI subsystem goes away

`--import-gci` existed to bring an existing single-player town online, and
`--export-gci` to hand a town back to Dolphin. With single-player compatibility
dropped **and** a fresh town required, neither has a consumer. Delete:

- `server/src/gci.cpp`, `server/include/acserver/gci.hpp`, `kGciResidentCount`
- `--import-gci` / `--export-gci` in `server/src/main.cpp`
- `town.gci` from the town directory layout
- the `semantic GCI round trip` test case, and the GCI half of
  `persistence_recovers_checkpoints_journal_and_gci`

`docs/netcode/PERSISTENCE.md` and `DEPLOYMENT.md` both document the file and the
commands and must be updated in the same phase. This is the one item in this
plan inferred rather than instructed — if the one-way import bridge is worth
keeping, say so and it stays; nothing else in the plan depends on the answer.

The town then persists through what already carries it: the CRC journal, rotating
checkpoints, and SQLite.

### 4.2 The client has no save file

With the server required, nothing on the client is worth persisting — the town,
the houses, and the player's own inventory all live server-side. The local save
stack goes:

- `pc/src/pc_m_card.c`'s GCI read/write path and `pc/src/pc_save_bswap.c`
  entirely. Verified: `pc_save_bswap` has **no caller outside `pc_m_card.c`**, so
  once the card path goes, the byte-swap layer is dead with it. The in-memory
  `Save_t` is then plain native-endian and the whole `PC_BSWAP_TO_BE` /
  `FROM_BE` concept disappears.
- The `save/` directory from the release package (`scripts/package_windows.ps1`,
  `build_pc.bat`) and from the docs.
- `src/player_select.c`'s slot UI is bypassed: identity comes from
  `network.ini`'s `account_id`. Its ten `PLAYER_NUM` sites therefore need no
  16-slot rework — the screen is skipped, not widened.

**This is not a pure deletion, and it is the sharpest edge in the plan.** The
client currently *boots* by reading a local save: `mCD_InitGameStart_bg`
(`pc/src/pc_m_card.c:809`) drives `mSDI_StartDataInit`'s init modes off
`pc_save_loaded`. Server-required means that path must be rewired to initialize
`Save_t` from the server baseline instead. Likewise every in-game save entry
point (`mCD_SaveHome_bg`, `mCD_SaveStation_*_bg`, `mCD_SaveErasePlayer_bg`,
`include/m_card.h:303-317`) must become a server checkpoint request or a
no-op that still satisfies its callers' expectations — the original code calls
these from Nook's dialogue and the sleep/save flow and will hang or fault if
they simply return failure.

Budget this as real work in E0, not as a line-deletion pass.

### 4.3 What still persists

Nothing changes server-side: the CRC journal, rotating checkpoints, and SQLite
already carry the whole town. `settings.ini`, `keybindings.ini`, and
`network.ini` are separate files and are unaffected.

## 5. Item-name space for HOUSE4..HOUSE15

Unchanged from the previous draft, and still required: the foreground stores the
item name, and the house actor derives identity from `npc_id - HOUSE0`
(`src/actor/ac_my_house.c:147,206`). Four clusters all containing `HOUSE0..3`
would collide onto the same four `homes[]` records, so 16 distinct ids are
needed regardless of clustering.

`ITEM_NAME_GET_TYPE(n)` is `(n & 0xF000) >> 12`
(`include/m_name_table.h:205`), so anything in `0x5000-0x5FFF` types as a
structure. The block runs `STRUCTURE_START 0x5800` .. `STRUCTURE_END 0x5853`
(`include/m_name_table.h:3052,3137`) and `ETC_START` is `0x8000`, leaving a large
free correctly-typed range.

Append twelve ids at `STRUCTURE_START + 83 .. + 94`, move `STRUCTURE_END` to
`+95`. Three tables are indexed by `structure_name - STRUCTURE_START` with **no
bounds check** and must grow by twelve entries in the same order:

| Table | Location | New entries |
|-|-|-|
| `l_structure_set_type` | `src/game/m_field_info.c:2637` | placement footprint (§6) |
| `setupInfo_table` | `src/actor/ac_structure_clip.c_inc:19` | `{ mAc_PROFILE_MYHOUSE, aSTR_TYPE_MYHOME, aSTR_PAL_MYHOME_A, 0 }` ×12 |
| `DUMMY_HOUSE*` | `include/m_name_table.h:3854` | `DUMMY_HOUSE4..15`, used at `src/actor/ac_my_house_move.c_inc:481` |

Widen `ITEM_IS_PLAYER_HOUSE` (`include/m_name_table.h:539`) and the
`structure_name >= HOUSE0 && structure_name < SHOP0` branch
(`src/actor/ac_structure_clip.c_inc:115`).

## 6. Clusters

A cluster is one copy of the proven four-house acre: `mFM_BLOCK_TYPE_PLAYER_HOUSE`,
baked today at block `{2,1}` (`src/game/m_start_data_init.c:103`) and already
placeable by the town generator (`src/game/m_random_field_ovl.c`). Four clusters
give sixteen slots.

Reusing the acre is what makes this cheap. Collision, door approach tiles,
shadow/clip data, and the `side_idx = house_idx & 1` door-orientation parity all
come from data that already works — within a cluster, house *k* uses parity
`k & 1` exactly as the original does. Placement validation reduces from
per-house footprint geometry to "is this 16×16 acre clear?".

Rules:

- **Always four clusters, sixteen slots**, regardless of the configured session
  capacity. A four-player town spends four of its thirty acres on mostly-empty
  houses; in exchange, placement is identical in every town, capacity can be
  raised at any time with no new placement pass, and there is no second code
  path to test.
- Cluster positions are chosen once at town creation, seeded from `town_seed`,
  and stored in authoritative town state. Stable across restarts, never
  re-rolled.
- No acre is pinned; cluster 0 disperses with the rest.
- An acre is eligible if it is inside the outdoor foreground (x 16..95,
  z 16..111 — `server/src/town_runtime.cpp:316`), contains no structure, no
  buried item, no river/cliff/water tile, and has a walkable approach.
- Clusters may not overlap each other, the shops, the museum, or the station.

Even though clusters remove the need for it *now*, `HouseState` carries
`anchor`, `footprint`, and `facing` from day one (§8) so the later free-placement
phase is additive rather than a rewrite. Under clustering these fields are
simply derived from the cluster acre and the in-cluster index.

## 7. Interior streaming and zone mapping

`src/game/m_net_hooks.c:38` maps any player room to `100 +
Common_Get(player_no)` — the **local** player. Entering another player's house
today requests your own zone. This is a pre-existing bug that 16 houses makes
unavoidable to fix: resolve the zone from the door actor's `npc_id`
(`HOUSE0 + n`), not from `player_no`.

Entering a house:

1. Client requests a zone transfer for that house's zone (existing signed
   transfer flow, `net/src/zone.cpp`).
2. Server sends a `HouseInteriorBaseline` on the Bulk channel — already
   fragmented and bounded (`net/src/fragmentation.cpp`).
3. Client decodes it directly into `homes[n]` — no scratch buffer, no accessor
   indirection, because slot *n* now genuinely exists.
4. Furniture edits by a non-owner are rejected by `HousingAuthority::apply`
   (`net/src/housing.cpp`), which already checks `operation.account` against
   `HouseState::owner`.

## 8. Server

**`net/include/acnet/housing.hpp`**

- `kOriginalResidentSlots = 4` loses its meaning; replace with
  `kMaxResidents = 16`.
- `HouseState` gains `TileAddress anchor`, `std::uint8_t footprint`,
  `std::uint8_t facing`, `std::uint8_t cluster`, `std::uint8_t slot_in_cluster`,
  `std::uint8_t outlook_pal`, `std::uint8_t door_original`.
- `residents_` becomes `std::array<AccountId, kMaxResidents>`.
- `house_id_for_slot` generalizes to `kHouseIdBase + resident_index`; ids must
  stay stable across restarts because they appear in zone ids and the journal.

**`server/src/town_runtime.cpp`**

- `configure_zone_topology` (line 194) hardcodes nine buildings with literal ids
  `0x5800..0x5803` and fixed fallback positions. Replace the four house rows
  with a loop over the house registry: zone `100 + n`, enter door `100 + n`,
  exit door `200 + n`, position derived from the stored anchor. Re-run it
  whenever a house or cluster moves, not only at init.
- `initialize` (~line 301) creates zones `100..103`; extend to `100..115`.
- Slot assignment (lines 561–574) fills a
  `std::array<bool, kOriginalResidentSlots>` and demotes the 5th connection to
  `PlayerKind::Visitor`. Widen to `kMaxResidents`; with visitors unsupported,
  connection 17 is **rejected**, not demoted. Residency becomes persistent
  registration, not connection order — a registered account keeps its index
  across restarts.
- House size, debt, and upgrade state become server-authoritative from the
  first phase (maintainer decision). That pulls the `m_shop.c` debt flow and
  `keep_house_size[]` into the critical path early; budget for it.

**Slot reclamation.** Resident indices are permanent once registered, so without
a way to free one a town fills with abandoned accounts and becomes unusable.
Add a one-shot admin command in the existing pattern (run while the server is
stopped, like `--ban`):

```text
AnimalCrossingServer --data towns/default --evict 1001
```

It frees the resident index, releases the house's cluster slot, and archives the
house record rather than dropping it, so an eviction is recoverable from the
journal. No automatic inactivity reclaim — a player on holiday must not lose
their house. Document it in `DEPLOYMENT.md` alongside `--ban`/`--unban`.

**`server/src/town_state.cpp`** — set `kTownStateVersion` to 4 (line 12) and
**delete the v1–v3 read paths** rather than keeping them: with no town carried
forward they have no callers, and a version field that only ever accepts one
value is far easier to reason about than three dead upgrade branches. Keep the
version *check* — it is what stops a corrupt or foreign checkpoint being
misparsed. Line 281's `house_count > kOriginalResidentSlots` becomes
`kMaxResidents`.

**`schemas/database/003_housing.sql`** — edit in place rather than adding a 004
migration. It constrains `original_slot BETWEEN 0 AND 3`; replace with
`resident_index BETWEEN 0 AND 15` plus cluster/anchor/footprint/facing columns.
Rewriting a shipped migration is normally forbidden, but no database exists that
has applied the old one, and a 004 that immediately rewrites 003's table would
leave permanent dead schema. The transactional + auto-backup path in
`server/src/database.cpp` is untouched.

## 9. Protocol

Protocol v6 is already assigned to client-authoritative full transforms. Bump
`kProtocolVersion` 6 → 7 (`net/include/acnet/types.hpp`) for this extension and
update `docs/netcode/PROTOCOL.md` with the registry messages below.

| Message | Channel | Payload |
|-|-|-|
| `HouseRegistrySync` | Events | ≤16 × {house_id, owner, resident_index, cluster, slot, anchor, facing, size, palette, door design, plaques, owner name} |
| `HouseRegistryDelta` | Events | one changed record + revision |
| `HouseInteriorBaseline` | Bulk | house_id, revision, 3 floors × 4 layers, wall/floor, gyroid, mailbox |
| `ClusterLayout` | Events | 4 × acre coordinates, sent at baseline |
| `HousePlacementRequest/Result` | Transactions | idempotency key, house_id, target slot, expected revisions |

`ServerHello` gains `resident_index` (0–15). Reject `resident_index >= 16`,
out-of-range clusters/slots, non-finite anchors, and invalid facings at decode
time; `tests/fuzz/protocol_fuzz.cpp` must cover each new decoder.

## 10. Phases

Each phase ends green on `make check` and updates `CURRENT_STATUS.md`.

| Phase | Scope | Done when |
|-|-|-|
| E0 | `PLAYER_NUM` → 16; `house_arrangement` → `u64`; the 38-site `& 3` audit; palette and room-default fixes; amend `AGENTS.md`/`MASTER_PLAN.md` §25 | Client boots to a 16-slot `Save_t`; no `& 3` remains in house/home files |
| E0.5 | Server-required: delete the `NETCODE_ENABLED=OFF` build, the local save stack, and the GCI subsystem (§4.1–4.2); rewire boot and the save entry points; update `PERSISTENCE.md` / `DEPLOYMENT.md` | Client boots from a server baseline with no save file present; Nook's save flow and the sleep/save path complete without faulting; `make check` green with GCI cases removed, not skipped |
| E1 | Server registry to 16 with persistent indices; state v4 with v1–v3 readers deleted; revised `003_housing.sql`; zones 100–115; size/debt/upgrade authority for all 16; 17th connection rejected; `--evict` | 16 residents register, persist, upgrade, evict, and restore across restart; month-soak green |
| E2 | Protocol v8, registry sync/delta, `ClusterLayout`, `resident_index` in `ServerHello` | Round-trip + fuzz coverage for every new decoder |
| E3 | HOUSE4..15 ids, three table extensions, 16 exteriors rendering | Sixteen distinct exteriors render; offline behaviour unchanged |
| E4 | Cluster placement: seeded selection, acre eligibility, foreground commit, dynamic `configure_zone_topology` | Four clusters land on valid acres in a fresh town and survive restart |
| E5 | Per-house zone mapping fix, interior streaming, visitor read-only enforcement, move-between-slots | Two clients enter each other's houses; non-owner furniture edits rejected; move is atomic under an injected crash |
| E6 | Free per-house placement using the `anchor`/`footprint`/`facing` fields | Arbitrary placement passes the same validation and atomicity tests as E5 |

## 11. Tests

Extend `tests/net/test_main.cpp` (31 cases today; the runner has no filter, so
narrow the vector temporarily to iterate):

- 16 residents register, persist, upgrade, and restore with stable indices.
- A 17th connection is rejected rather than demoted.
- Cluster selection is deterministic for a given `town_seed` and never overlaps
  a shop, the museum, the station, or another cluster.
- Placement/move is idempotent under replay and rejects a stale revision.
- A crash injected mid-move recovers to exactly one house.
- Interior baseline round-trips 3 floors × 4 layers within bulk fragment limits.
- A non-owner furniture op inside a visited house is rejected.
- Two clients in two different house zones neither see nor collide with each
  other.
- A v3 checkpoint is rejected, not silently upgraded.
- `--evict` frees the index and the cluster slot, archives the house, and the
  freed slot is reusable by a new registration after restart.
- Cluster placement is unaffected by configured capacity — a capacity-4 town
  still gets four clusters.

`town_chaos.cpp` gains house moves under loss/reorder; `town_month_soak.cpp`
places and moves houses across the 31-day run.

## 12. Deferred and assumed

Nothing blocks E0.

**Tracked separately, not in this plan:**

1. **Character creation.** The seeded-random appearance
   (`Net_RandomizeInitialAppearance`, `src/game/m_net_hooks.c:120`) stays as a
   stopgap: face and clothing derive from `town_seed` plus resident index, which
   is stable across reconnects but gives sixteen players an appearance they
   never chose. A proper naming/face flow needs the intro/Rover event path,
   which is the most fragile place in the decompiled code to add multiplayer
   branching — it deserves its own plan. E0 must keep the
   `slot >= PLAYER_NUM` guard correct as `PLAYER_NUM` moves to 16.
2. **Island cottages.** `COTTAGE_MY` / `mHm_cottage_c` are single-instance in
   `Save_t.island`. Sixteen residents sharing one cottage is a pre-existing
   oddity this plan does not address — flag it before anyone assumes otherwise.

**Assumed, reversible:**

3. **The GCI subsystem is deleted** (§4.1). The one inference rather than an
   instruction; say the word and the `--import-gci` bridge stays.
