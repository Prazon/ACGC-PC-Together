# Gameplay Coverage Audit

Audit date: 2026-08-06. Method: every transaction family in `net/`/`server/` was
traced to its call sites in `src/`, and every major gameplay subsystem in
`src/game`/`src/actor` was checked for net awareness.

The transport, world-tile, house, zone, and clock layers are genuinely
delivered. Most of what makes the town *shared* is still simulated per client.

## Status key

- **Wired** — client hook exists and the server commits it.
- **Server-only** — authority exists in `net/`/`server/` with no game-side caller.
- **Local** — runs entirely on each client; diverges silently between players.

## Findings

### Tier 1 — loses state or misbehaves today

**1. Economy family is server-only.** `EconomyOpType{Buy, Sell, Deposit,
Withdraw, PayDebt, Donate, AttachMail}` (`net/include/acnet/economy.hpp`) has
zero game-side callers. `m_shop.c`, `m_bank_ovl.c`, `m_repay_ovl.c`,
`m_post_office.c`, `m_museum.c`, and `m_mail.c` contain no net references.
Local wallet writes remain in `ac_npc_shop_common.c:2211-2247`,
`m_hand_ovl.c:501`, `ac_ev_gypsy_move.c_inc:105-107`,
`ac_haniwa_move.c_inc:91-108`, `ac_ike_pst_pig01.c:10`. Because
`m_net_hooks.c` overwrites `wallet`/`loan`/`bank_account` from the server on
every inventory-revision bump, a purchase is refunded at the next server delta
and two players can sell the same goods for bells the server never sees.

**2. Inventory grants outside the hooked verbs are erased.** 137
`mPr_SetPossessionItem`/`mPr_SetFreePossessionItem` call sites across 40 files;
only the pickup path is hooked. `Net_ApplyAuthoritativeState` overwrites all 15
pocket slots whenever the server's inventory revision changes, so villager
gifts, purchases, tree shakes, balloon presents, HRA rewards, and mail
attachments vanish. `Player_actor_putin_item` with `pos_p == NULL`
(`m_player_common.c_inc:2284-2296`) — the "receive an item" path — never
contacts the server at all.

**3. Pickup and drop were broken indoors.** *Fixed.* Every call site branched on
`Net_IsConnected()` and took the network path in **any** zone, but
`Net_RequestPickup`/`Net_RequestDrop`/`Net_RequestTerrain` address the exterior
tile table. Interior unit coordinates never match that table's range
(x=16..95, z=16..111), so `acnet_client_tile(1, ...)` failed: dropping anything
inside a building reported "no room", and picking an item off a house floor
silently did nothing because `m_player_common.c_inc` issued the request and
returned while ignoring the result.

The connection test was the wrong question. Call sites now branch on
`Net_WorldTilesAuthoritative()` — connected *and* the authoritative baseline
zone is the exterior — so indoor actions keep the original local path. That is
correct rather than a fallback: a room's floor items live in
`layers[].items[z][x]` and are already replicated by the house-state submit,
which captures all four layers. Testing the baseline zone rather than the local
scene also keeps a mid-transfer frame, where the two disagree, out of the tile
path. Outdoors the behaviour is unchanged: a rejected request commits nothing
locally and the server's next delta corrects the client.

Changed: `include/m_net_hooks.h` (new hook plus its no-op fallback),
`m_net_hooks.c`, `m_player_common.c_inc`, `bg_item_common.c_inc`,
`m_player_main_dig_scoop.c_inc`, `m_player_main_fill_scoop.c_inc`,
`m_player_main_swing_axe.c_inc`.

Verified by compile only: all parent translation units — `m_player.c` and the
four `bg_item` parents (`bg_item.c`, `bg_cherry_item.c`, `bg_xmas_item.c`,
`bg_winter_item.c`) — build clean with the real CMake flags both with and
without `NETCODE_ENABLED`. The netcode suite does not exercise game-side C, so
confirming the behaviour needs `scripts/smoke_windows.ps1` with a disc.

**4. Fishing and bug catching returned the wrong species.** *Fixed server-side
in this pass — see below.* The client still needs to send and apply the species.

**5. Shop stock was two placeholder entries.** *Fixed server-side.*
`town_runtime.cpp` pushed `{0x2001, 400, 8}` and `{0x2002, 800, 4}`, and the
daily job only refilled quantities. See below.

### Tier 2 — silent per-client divergence

**6. Villagers.** *(Roster resolved 2026-08-08 — see CURRENT_STATUS.md. The
roster is server-owned, villagers are registered entities, and `mNpc_Grow` no
longer runs per client. Move-ins/outs, conversation leases, per-player memories
and schedules remain.)* Original text:

**Villagers are entirely local.** `m_npc.c` (7383 lines), `m_npc_schedule.c`,
`m_quest.c`, and all 68 `src/actor/npc/*.c` files have no net references. The
server's `NpcAuthority` registers exactly one placeholder shopkeeper
(`town_runtime.cpp:319-328`) and its hourly job only bumps `schedule_state`.
`acnet_client_request_conversation` has no callers, so conversation leases never
engage: two players can talk to the same villager at once, both receive the
first-meeting greeting, both complete the same favour, and both collect the
reward. Move-ins, move-outs, and friendship diverge permanently.

**7. Town-wide systems with no replication.** Lost & found, dump, HRA, design
patterns and Able Sisters, catalog, snowmen, balloon presents, and the
holiday/event system (`m_event.c`, 3028 lines). *(The town tune and the
noticeboard came off this list 2026-08-08 — both are server-owned town state
now.)* *(Museum donations and mail
delivery came off this list with the economy work; the turnip market came off
2026-08-07 — it is now server-owned town state, and it was worse than
divergence: turnips could not be sold online at all. See CURRENT_STATUS.md.)*

**8. Daily world regeneration.** *(Half-resolved 2026-08-08.)* The client no
longer runs `mAGrw_RenewalFgItem` while connected — it never survived the
authoritative projection anyway, so this stopped churn rather than removing a
feature. The server still does not perform the regeneration itself, and that is
**blocked on a design decision, not on effort**: weed, fossil and money-rock
placement all read per-tile collision attributes (`mCoBG_Attribute2CheckPlant`)
that the asset-free server has no copy of. The town bootstrap would have to
carry an attribute mask first. See CURRENT_STATUS.md.

**9. The server persists almost nothing per player.** `server/src/gci.cpp`
covers 15 pocket slots plus conditions, wallet, debt, and bank balance. Not
stored: encyclopedia flags, catalog, mail, designs, HRA score, badges, town
tune, town flag, villager relationships, event flags. Meanwhile the client keeps
writing its own local GCI (`pc/src/pc_m_card.c:868,898,932,1124`) with no
connection gating, so every machine accumulates a private divergent save.

### Tier 3 — presentation and interaction

**10. Remote players present fully but remain contactless.** *(Rewritten
2026-08-07 — the original entry predated the animation and held-item work.)*
`ac_net_remote_player.c` now plays the replicated body animation at
velocity-derived walk speed, animates the face (blink, per-animation eye/mouth
tracks, per-state constants), and renders the held tool as the real
model/skeleton under the hand matrix, including the umbrella `TOOLS_ACTOR`
child and the held balloon — `docs/netcode/REMOTE_PRESENTATION_PLAN.md` is the
delivery record, and none of it has been visually verified on a disc yet.
Deliberately still absent: collision — so no pushing or net-hitting — plus the
fishing float/line, net catch label, net bag lean, and any remote audio.
Player-to-player trading has full server escrow with zero game-side callers.

**11. Furniture uses a whole-room submit rather than the transaction API.**
`FurnitureOpType{Place, Remove}` is dead; the client hashes and submits the
entire room. Conflict resolution is last-writer-wins at room granularity.
`AcNetHouseState` now carries everything this finding listed. *(The music box
landed 2026-08-08. The "mailbox" item was stale even when written: a house
mailbox's letters are server-owned through the mail transaction family and its
own `MailboxState` revision, not through the house state.)* Storage layers *are*
covered —
`mCoBG_LAYER_NUM` is 4 and all four are captured. *(The gyroid came off this
list 2026-08-07: display, message, purchases and proceeds are now a
server-owned `GyroidState` with its own transaction family. The wallpaper,
carpet, pattern flags, exterior palette and door design came off it the same
day as `HouseSurfaces` — see CURRENT_STATUS.md.)*

## Delivered in this pass — encounter species authority

Finding 4 is fixed on the server.

Previously `encounter.cpp` rolled a species from a fixed six-entry prefix of the
item range using `(random + hour + zone) % 6`, ignoring what the client hooked,
while the client recorded the *local* species in the encyclopedia. The catch
message and museum entry disagreed with the pocket.

Now:

- `tools/gen_encounter_tables.py` distills per-species availability from
  `src/actor/ac_set_ovl_gyoei.c` and `src/actor/ac_set_ovl_insect.c` into
  `net/src/encounter_tables.inc` — a month × time-slot bitmask per species.
  Generated rather than transcribed so it cannot drift; `--check` fails if the
  committed file is stale.
- `EncounterRequest` carries the `species` the client observed. Spawns are still
  client-simulated, so this is a *claim*: the server accepts it only if that
  species can legally appear at the current month, hour, and weather, and it
  alone decides whether the catch succeeds and commits the inventory. An illegal
  claim is rejected with `InvalidState` and spends no cooldown.
- Fish validation tolerates both halves of the month because the original game
  blends the outgoing half's fish over a randomised transition of up to five
  days (`ac_set_ovl_gyoei.c:1808-1812`).
- Three species are absent from the month tables because the original spawns
  them through dedicated paths, and are special-cased on the same terms: the
  coelacanth only while raining or snowing, the bee and ant unconditionally
  (tree shaking, candy and trash).
- With no claim the server picks uniformly from everything legal right now
  instead of a fixed prefix.
- The decoder rejects an out-of-range species identifier at the parser.

Verified: compiles clean under `-Werror -Wall -Wextra -Wpedantic`; a standalone
harness confirms date decoding, stringfish winter-nights-only, arapaima
July-September, firefly June-nights-only, weather-gated coelacanth, cross-kind
and out-of-range rejection, and that every one of the 12 months × 24 hours has
at least one legal fish. Spot checks match the retail game.

The wire format is **protocol v10**. The concurrent banking/mail work had
already taken v9, so the encounter change bumps to 10;
`net/include/acnet/types.hpp`, `docs/netcode/PROTOCOL.md`, and `net/CLAUDE.md`
all say v10. Note that `kProtocolVersion` is a single constant under strict
`min == max` negotiation, so v10 is the version carrying *both* the mail/bank
and encounter changes — there is no way to version them apart.

## Delivered in this pass — authentic Nook stock

Finding 5 is fixed on the server.

The original does not hold a fixed stock list; `mSP_MakeRandomGoodsList` rolls
one each day. `tools/gen_shop_tables.py` distils the inputs into
`net/src/shop_tables.inc`: the per-category item pools from the list files in
`src/data/item` plus the file-static diary lists in `m_shop.c`, and the six
price tables. Those lists are written as macro expressions, so the generator
emits a small C program, compiles it against the real headers with the PC
build's flags, and dumps the resolved values — the compiler is the only thing
that can be trusted to agree with the game. `--check` fails on a stale table.

`net/src/shop.cpp` reproduces the roll: the tier fixes how many of each category
are drawn, each draw picks a rarity from the `goods_power`-weighted split in
`mSP_GetItemList`, and the town's A/B/C permutation turns that rarity into a
sublist. Duplicates are refused unless the sublist is smaller than the shelf,
which is the original's own escape hatch. Prices come from
`mSP_ItemNo2ItemPrice`'s per-category indexing, including the furniture
facing-bit shift and stationery's repeating price block; selling is
`price / 4`. The town's permutation is seeded from the town seed, and the
existing daily-renewal job now rolls a fresh shelf instead of refilling
quantities.

Verified: `shop.cpp` compiles clean under `-Werror`; a harness rolls 40 days per
tier and finds no zero item and no zero price in any entry, and the shelf sizes
match `l_goods_count_table` exactly — Cranny 5, Nook 'n' Go 8, Nookway 14 (with
the rare slot and a diary), Nookington's 22. Furniture index 0 prices at 41240,
matching `ftr_price.c`.

**Not yet done for this finding:** `ShopStockState` (tier, goods power,
permutation) is held in `TownRuntime` but not yet written into the checkpoint,
so a restart re-seeds it from the town seed rather than restoring it; the rolled
shelf itself already persists through the existing `ShopState` encoding. Tier is
also fixed at Cranny — nothing raises it from cumulative spending yet. And no
client reads any of this, because buying still runs through the unhooked local
shop code in finding 1.

## Delivered in this pass — encounter client half

Finding 4 is now complete end to end.

`Net_RequestEncounter` takes the species the player actually hooked or swung at
— `uki->get_fish_type_proc()` for a fish, the resolved insect item for a net —
and passes it through `acnet_client_request_encounter_auto`. Because the server
validates that claim before committing, the pocket now holds the creature the
client showed.

The encyclopedia needed more care. The original writes it
(`mSM_COLLECT_FISH_SET` / `mSM_COLLECT_INSECT_SET`) the instant the catch
animation starts, which online would credit a species the server may still
refuse for capacity, cooldown, a stale revision, or an out-of-season claim. Both
call sites now skip that write while `Net_EncounterRecordsPending()` is true, and
`Net_UpdateEncounters` — pumped from `Net_PreSimulation` — drains
`acnet_client_take_encounter_result` and records the entry from the item the
server actually committed. A refused catch leaves the encyclopedia untouched.
The caught-message and the "already collected" check stay local and immediate,
since those are presentation.

Verified: 34/34 tests, protocol fuzz, and `m_player.c` plus `m_net_hooks.c`
compile clean with the real CMake flags both with and without
`NETCODE_ENABLED`.

## Server console

The operator console and the one-line fallback now report a villager count
beside residents and visitors. It reads **1** today because the runtime only
registers the placeholder shopkeeper — see finding 6. The number becomes
meaningful when villager replication lands; until then it is an accurate
statement of what the server is actually simulating.

## Superseded note

**Formerly outstanding:** the client had to send `species` and apply the
server's returned item to both the pocket and the encyclopedia. That touches
`m_net_hooks.c`, `m_player_main_notice_rod.c_inc`,
`m_player_main_notice_net.c_inc`, and `c_api.*` — all held by other sessions at
the time of writing. A round-trip case belongs in `tests/net/test_main.cpp` once
that file is free.

## Recommended order

1. ~~Findings 3 and 5~~ — done (5 is server-side only).
2. Findings 1 and 2 together — route `mPr_*` and the wallet through the economy
   transactions and stop the blind pocket overwrite. They are one problem, and
   the largest remaining source of lost items.
3. Finish finding 4's client half, then finding 5 (the server logic exists; only
   the data is placeholder).
4. Finding 6 needs a design document before any code.
