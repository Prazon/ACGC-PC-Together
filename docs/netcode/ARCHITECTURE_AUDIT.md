# Phase 0 Architecture Audit

## Baseline

The PC port compiles the original decompiled C simulation together with a PC
translation layer. Pointer-bearing display-list and JSystem paths have been
made 64-bit safe; the 64-bit ABI is now mandatory at both CMake configuration
and compilation. The graphical client still requires SDL2 and runtime assets
from a user-supplied disc image. The online core has a separate portable target
so protocol, authority, persistence, and headless tests remain asset-free.

This workspace is an extracted source tree without Git metadata. Change and
verification history is maintained in `CURRENT_STATUS.md` and test output.

## Authoritative integration seams

| Concern | Existing seam | Initial hook |
| --- | --- | --- |
| Game mode/startup | `pc/src/pc_main.c:main` | Parse client/listen/server/replay options before SDL initialization |
| Gameplay frame | `src/game/m_play.c:Game_play_move` | `Net_PreSimulation` and `Net_PostSimulation` |
| Actor update | `src/game/m_actor.c:Actor_info_call_actor` | Server/client actor filtering and snapshot collection |
| Actor creation | `src/game/m_actor.c:Actor_info_make_actor` | Assign stable side-table entity ID after successful creation |
| Actor deletion | actor delete path in `src/game/m_actor.c` | Remove side-table mapping before storage is released |
| Player spawn | `src/game/m_actor.c` player profile creation | Keep one ordinary local player; create remote/server proxies separately |
| Scene load | `src/game/m_play.c:Gameplay_Scene_Read` | Apply authoritative baseline after native scene setup |
| Door/scene request | `Common_Get(door_data)` and `Save_Get(scene_no)` call sites | Replace online transition commit with a zone-transfer request |
| Foreground mutation | `mFI_SetFG_common` and `mFI_UtNumtoFGSet_common` | Route player-originated mutation through a revisioned world operation |
| Drop request | `player_drop_entry_proc` in tag/bg-item code | Send semantic drop request; server commits tile and inventory atomically |
| Inventory mutation | `mPr_SetPossessionItem` / `mPr_SetFreePossessionItem` | Account-scoped inventory transaction hook |
| Shops/economy | `src/game/m_shop.c`, direct wallet writes | Server transaction command; client displays replicated result |
| Time | `src/game/m_time.c:mTM_time` | Server clock source; clients consume replicated clock |
| Events | `src/game/m_event.c` renewal/schedule code | Coordinator owns schedules and serializes town-wide controllers |
| GCI storage | `pc/src/pc_m_card.c`, `pc/src/pc_card.c`, `pc/src/pc_save_bswap.c` | Storage adapter plus journal/checkpoint boundary |

## Sole-player audit

Repository search found at least:

- 324 `GET_PLAYER_ACTOR` references.
- 403 `Now_Private` references.
- 125 `Common_Get(player_no)` references.
- 1,202 `Save_Get` and 111 `Save_Set` references.

These are migration inventories, not all defects. Each use is classified when
its subsystem is brought online:

1. Local presentation (camera, menus, rendering): retain local player access.
2. World awareness (NPCs, collisions, effects): query nearest/all eligible
   player views.
3. Interaction code: use the scoped interaction owner.
4. Persistent player state: use the account/resident selected by the accepted
   transaction.
5. Server-only code: legacy sole-player access is diagnosed unless an explicit
   scope is active.

Mechanical replacement is rejected because it would silently target the wrong
player in dialogue, inventory, event, and collision paths.

## Mutation risk map

High-risk conserved state:

- Inventory items, bells, debt, shop stock, trades, donations, and mail
  attachments.
- Ground tiles and items, buried items, trees, flowers, rocks, holes, houses,
  furniture, and storage.
- Resident slots, villager moves, museum state, and event rewards.

These mutations require an idempotency key, resource revision, validation,
atomic journal commit, and authoritative delta. Visual state and transient
actors use snapshots and do not enter the persistent transaction log.

## Actor replication policy

- Replicated: players, villagers, structures/doors, important movable actors,
  encounters, ground mutations, and furniture.
- Server-only: persistence controllers, authoritative encounter outcomes,
  account/transaction state, zone reservations, schedules, and moderation.
- Client-only: cameras, menus, sound, particles, loading helpers, cosmetic
  effects, and most UI actors.
- Audit-required: event controllers and actors that combine a persistent
  mutation with a presentation sequence.

Network identities live in a side table and are never stored as raw pointers in
the protocol. IDs are not reused within a process lifetime; serialized entity
references include a generation.

## Determinism and randomness

The original simulation contains global random sources, floating-point motion,
order-sensitive actor lists, wall-clock reads, and platform behavior. Sharing a
seed cannot make it safe deterministic lockstep. Random gameplay outcomes are
generated and recorded by the server. Clients may run cosmetic randomness only.

## Build and test boundary

`net/` is a dependency-light C++17 library with a stable wire format and narrow
C-compatible integration hooks. `server/` supplies the headless town runtime.
The root build compiles these on 64-bit Linux for tests; `pc/CMakeLists.txt`
builds the same core into the 64-bit Windows client and dedicated server. Tests
use temporary town directories and synthetic state, never copyrighted assets.

## Initial constraints

- Four persistent residents remain mapped to original save slots.
- Visitors use extended server records.
- One process owns one town. Zones are isolated logical workers in the portable
  coordinator and additional towns use separate processes and data paths.
- Public deployment requires authenticated/encrypted transport. The local
  development transport is not treated as an Internet security boundary.
- Full graphical smoke testing needs a legitimate disc and a 64-bit SDL/OpenGL
  environment; the headless core remains fully testable without game assets.
