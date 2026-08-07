# ACGC Dedicated Town Netcode Mod — Master Plan

> Implementation status (2026-08-06): the initial release phases are delivered;
> see `docs/netcode/CURRENT_STATUS.md` for the verified build and test record.
> The requested production authority model deliberately differs from the early
> draft below: movement/collision and door spatial checks are client-authoritative,
> while persistent and contested game state remains server-authoritative.
> Protocol v8 also completes the shared-house polish pass: bounded three-floor
> furniture/light/music state, inventory-conserving decoration updates,
> synchronized interior/exterior lights, exact arrival transforms, and replicated original door animations are implemented
> and covered by the two-client production loopback and passive Windows smoke.

## 1. Project goal

Extend `flyngmt/ACGC-PC-Port` with persistent, dedicated-server towns in which multiple players can live, visit, interact, trade, decorate, shop, fish, catch insects, talk to villagers, attend events, and continue progressing while other players are offline.

The modified client continues to load original game assets from each player's legally obtained disc image. The project must never distribute Nintendo assets.

### Initial release target

- One persistent town per dedicated-server instance.
- Four permanent residents, preserving the original four player and house slots.
- Eight to sixteen concurrent connections, including visitors.
- Shared outdoor town and public interiors.
- Persistent resident houses.
- Server-controlled town time, weather, villagers, events, shops, economy, mail, museum, and world state.
- Client-authoritative original-game player movement and collision.
- GCI import/export and rotating server backups.

More than four permanent residents is a later save-format and housing expansion. It should not block the initial netcode release.

## 2. Existing architecture and constraints

The PC port executes the original C simulation natively. It is not a modern entity/component remake.

### Global save state

`Save_t` in `include/m_common_data.h` contains most persistent town data:

- `private_data[PLAYER_NUM]`
- `homes[PLAYER_NUM]`
- foreground item grids
- acre layout
- villagers
- shop inventory
- turnip prices
- events
- weather
- museum state
- bulletin board
- mail and player state

Most code accesses this through `Save_Get`, `Save_Set`, `Common_Get`, and `Common_Set`. These calls currently produce no semantic record that networking or persistence can replicate.

### Actor system

`ACTOR` and `ACTOR_PROFILE` in `include/m_actor.h` provide actor type, scene, position, rotation, velocity, state, lifecycle callbacks, and linked actor lists. `Actor_info_call_actor()` in `src/game/m_play.c` is the central actor update seam.

### Single-player assumptions

The code frequently assumes one meaningful player through:

```c
GET_PLAYER_ACTOR(play)
Now_Private
Common_Get(player_no)
```

NPC AI, conversations, events, collision, cameras, menus, inventory, scene transitions, and scripted actions may all depend on this assumption. Remote users must not simply be instantiated as additional normal `PLAYER_ACTOR` objects.

### Core architectural consequences

- One normal locally controlled `PLAYER_ACTOR` remains on each client.
- Remote users use a lightweight remote-player actor.
- The server uses headless player simulation/proxy objects.
- Server gameplay gains multiplayer-aware player queries.
- Persistent changes pass through explicit semantic transactions.
- Raw process memory is never replicated.
- Initially, each active zone runs in a separate process because `common_data` and many systems are global.

## 3. High-level system design

### Modified game client

- Connection and login UI.
- Local movement prediction.
- Snapshot reconciliation.
- Remote-player rendering and interpolation.
- Interaction request generation.
- Network-aware scene transitions.
- Inventory and world-state reconciliation.
- Local rendering, audio, UI, camera, and cosmetic effects.
- Asset loading from the player's disc image.

### Town gateway/coordinator

- Authentication and protocol negotiation.
- Town selection and invite policy.
- Connection routing.
- Rate limiting.
- Reconnect tokens.
- Player presence.
- Zone handoffs.
- Shared persistence coordination.

The first implementation may integrate this into the town-server process.

### Simulation workers

- One persistent exterior worker per active town.
- Public and private interior workers created on demand.
- Empty zones checkpoint and sleep.
- The exterior can enter low-frequency background simulation when empty.
- A town coordinator serializes shared persistence operations.

### Persistence

Recommended server layout:

```text
towns/<town-id>/
    town.gci
    town.db
    journal/
    snapshots/
    config.toml
```

- `town.gci`: original-compatible town checkpoint.
- `town.db`: multiplayer accounts, sessions, visitors, transactions, extended metadata, and later extended housing.
- `journal`: append-only accepted world operations.
- `snapshots`: recovery checkpoints.
- `config.toml`: capacity, permissions, time policy, moderation, and networking settings.

SQLite in WAL mode is sufficient initially.

## 4. Authority model

| System | Authority |
| --- | --- |
| Input | Client originates; server validates |
| Local movement | Client authoritative; server does not correct |
| Player position | Client transform, bounded/sequenced and relayed by server |
| Remote rendering | Client interpolation |
| Inventory and bells | Server |
| Ground items and terrain mutations | Server |
| Trees, flowers, holes, rocks | Server |
| Fish and insect outcomes | Server |
| Villagers and schedules | Server |
| Shops, prices, purchases, debt | Server |
| Weather, time, events | Server |
| Conversations | Server grants an interaction lease |
| House furnishing | Server transaction |
| Mail and attachments | Server |
| UI, camera, sound, particles | Client |
| Save/export | Server |

Clients send requests, never outcomes. For example, the client requests a fishing action; it does not report that it caught a particular fish.

## 5. Network identity

Pointers, actor-list positions, save-array indices, and memory addresses are never network identifiers.

```c
typedef uint64_t NetEntityId;
typedef uint64_t PlayerAccountId;
typedef uint64_t TownId;
typedef uint32_t ZoneId;
typedef uint32_t Revision;
```

Replicated categories include players, villagers, ground items, movable actors, encounters, doors, structures, furniture, event controllers, and selected temporary gameplay actors. Cameras, menus, particles, sound emitters, loading helpers, and most cosmetic effects remain client-only.

IDs must contain or resolve through a generation number so a stale packet cannot address a newly reused entity slot.

## 6. Transport and protocol

Use UDP with reliable ordered, reliable unordered, and unreliable sequenced delivery. ENet is a practical C-friendly starting point. GameNetworkingSockets is an alternative if Steam networking becomes a priority.

### Logical channels

| Channel | Delivery | Content |
| --- | --- | --- |
| Control | Reliable ordered | Login, handshake, zone transfer, disconnect |
| Transactions | Reliable ordered | Inventory, trade, purchases, furnishing, mail |
| Chat | Reliable ordered | Text chat and emotes |
| Snapshots | Unreliable sequenced | Movement and transient actor state |
| Events | Reliable sequenced/unordered | Animation, effects, action events |
| Bulk | Reliable fragmented | Initial zone baseline and save export |

### Serialization

Never transmit C structs directly. They contain padding, pointers, host endianness, and build-dependent layouts.

Use FlatBuffers, Protobuf-C, or another explicit schema for control and transactions. A custom packed bitstream may be introduced later for high-frequency snapshots.

```c
struct NetHeader {
    uint16_t protocol_version;
    uint16_t message_type;
    uint32_t sequence;
    uint32_t ack_tick;
    uint32_t payload_size;
};
```

The handshake includes protocol version, build identifier, feature flags, account/session credentials, town ID, requested character, and reconnect token when applicable.

## 7. Simulation timing

Retain the game's native simulation cadence initially. Do not combine the multiplayer project with a variable-timestep rewrite.

Suggested network rates:

- Local input: 20–30 Hz.
- Player snapshots: 15–20 Hz.
- Villager snapshots: 5–10 Hz.
- Static world changes: event driven.
- Clock/weather: on join and change.
- Persistence checkpoint: every one to five minutes plus critical transactions.

Each command includes an input sequence and estimated server tick. Each authoritative snapshot acknowledges the last processed input sequence.

## 8. Player movement

### Client-authoritative movement

1. The original client samples input and runs its native movement/collision.
2. The client sends the resulting full transform with a monotonic input sequence.
3. The server validates finite values, world bounds, sequence, and zone membership.
4. The server acknowledges and relays the transform without correcting its owner.
5. Remote clients buffer snapshots and smooth position/facing at render cadence.
6. Zone changes clear transform history so door teleports snap cleanly.

### Server validation

- Monotonic input sequence.
- Finite transform values and broad world bounds.
- Correct zone membership.
- Registered door route and destination capacity (door collision is client-owned).
- Tool and action cooldowns.
- Player not locked by another transaction or scripted action.

Client-submitted coordinates are authoritative for player presentation. This
intentionally does not attempt anti-cheat; semantic world operations remain
server-authoritative and revision-checked.

### Remote-player actor

Create `PROFILE_NET_REMOTE_PLAYER`. It stores only replicated presentation state:

- Position, rotation, velocity.
- Animation state and phase.
- Clothing and design references.
- Equipped tool and held item.
- Face, emote, name, and nameplate.
- Snapshot interpolation buffer.

It must not read controller input, control the camera, use `Now_Private`, perform ordinary player save logic, or directly mutate the world.

## 9. Multiplayer-aware player queries

Introduce a compatibility layer:

```c
NetPlayerView* NetPlayers_GetLocal(void);
NetPlayerView* NetPlayers_GetById(PlayerAccountId id);
NetPlayerView* NetPlayers_GetNearest(const xyz_t* position);
int NetPlayers_QueryRadius(
    const xyz_t* position,
    float radius,
    NetPlayerView** results,
    int capacity
);
```

Audit every `GET_PLAYER_ACTOR`, `Now_Private`, and `Common_Get(player_no)` use and classify it:

1. Local UI/camera behavior: retain local player.
2. World AI: query nearest or relevant players.
3. Interaction: pass initiating player identity explicitly.
4. Cutscene: allocate a scoped participant.
5. Rendering/culling: use the local viewing player.
6. Server gameplay: query all relevant players.

Authoritative server code should warn or assert when it uses legacy sole-player access without a scoped interaction player.

## 10. Persistent world transactions

Wrap mutation seams such as foreground item setters in semantic operations:

```c
typedef enum {
    WORLD_OP_DROP_ITEM,
    WORLD_OP_PICKUP_ITEM,
    WORLD_OP_DIG,
    WORLD_OP_BURY,
    WORLD_OP_PLANT,
    WORLD_OP_CHOP_TREE,
    WORLD_OP_SHAKE_TREE,
    WORLD_OP_PLACE_FURNITURE,
    WORLD_OP_REMOVE_FURNITURE,
    WORLD_OP_BUY,
    WORLD_OP_SELL
} WorldOpType;
```

Transaction lifecycle:

1. Client sends request plus observed revision and idempotency key.
2. Server validates player, state, proximity, ownership, cooldown, and revision.
3. Server performs the original gameplay operation.
4. Server extracts a semantic state delta.
5. Server commits the journal/SQLite transaction.
6. Server acknowledges the requester.
7. Server broadcasts relevant deltas.

Mutable blocks, inventories, shops, houses, and mailboxes require revisions. Stale requests are rejected with current state. This prevents duplicate pickups and last-item races.

## 11. Inventory, economy, and trading

The client displays a cached inventory but all changes are server-owned.

Operations include moving, dropping, picking up, equipping, buying, selling, depositing, withdrawing, attaching mail, claiming mail, donating, and trading. Attaching and claiming are two halves of one round trip: the attachment leaves the sender's pocket into a revisioned mailbox bounded at ten letters, and only the addressee, quoting the mailbox revision it observed, can take it out again.

Every operation validates:

- Ownership.
- Source revision.
- Item identity and condition.
- Capacity.
- Currency.
- Zone and interaction context.
- Idempotency key.

Trading uses escrow:

1. Server creates trade session.
2. Each participant submits an offer.
3. Every offer change invalidates confirmations.
4. Both confirm the same offer revision.
5. Server performs one atomic exchange.

Ground-item dropping must not be the only trade mechanism.

## 12. NPCs and conversations

Villagers simulate on the server. Replace sole-player distance checks with nearest/relevant-player queries.

Each NPC can grant an interaction lease:

```c
struct NpcInteractionLease {
    PlayerAccountId owner;
    NetEntityId npc;
    uint32_t lease_id;
    uint32_t expires_tick;
};
```

- Only the lease owner advances dialogue choices and quest mutations.
- Other players see replicated conversation animations.
- Disconnect or timeout releases the lease.
- Separate villagers may be used concurrently when their global dialogue dependencies permit it.
- Town-wide scripted event controllers are serialized initially.

Replicate high-level NPC state: transform, schedule/action state, animation, emotion, destination, conversation ownership, and quest-visible result. Do not replicate every internal field.

## 13. Zones, scenes, and buildings

Scene changes become coordinated zone transfers:

1. Client requests a door interaction.
2. Source zone validates proximity and state.
3. Coordinator resolves destination zone/instance.
4. Destination reserves a player slot.
5. Client receives a signed transfer token.
6. Client loads the destination scene.
7. Destination sends a baseline.
8. Client acknowledges readiness.
9. Destination spawns the player; source removes the old proxy.

Zone IDs as implemented (`net/include/acnet/zone.hpp`, `Net_SceneZone` in `src/game/m_net_hooks.c`):

```text
1       town exterior
2-6     public interiors (shops, post office, museum, Able's, police box)
100-103 resident houses
300     island exterior
301     island cabin (shared, ownerless)
302     islander's hut
```

Public buildings and resident/NPC houses are shared zones in v1. Menus remain per-client. Personal cutscenes involve only their participant while world simulation continues safely.

The island is one shared zone per town, not a per-player instance. It is two acres of the same outdoor field whose ground items live in `Save_t.island`, so it gets a zone of its own while keeping global unit coordinates. The Kapp'n ferry does not change scene, so it is reconciled from the acre kind rather than announced by a door animation. See `docs/netcode/PROTOCOL.md` → The island.

## 14. Residents and housing

### Version 1: four residents

Use the original four `Private_c` and home slots. This preserves house upgrades, debts, mail, player selection, and GCI compatibility. Additional connections are visitors with server-side account inventories and return-home information.

### Future: extended residents

Do not enlarge `Save_t` arrays in place. Store extended profiles in `town.db`, assign virtual house IDs, map house doors to dynamic instances, and provide a housing directory/district interface. Where legacy code requires an active original slot, copy selected data into a controlled scratch compatibility context rather than modifying the GCI layout.

## 15. Persistence and recovery

The active simulation owns native-endian state, but GCI writes alone are insufficient.

Use:

- Append-only operation journal.
- SQLite WAL transactions.
- Periodic GCI checkpoints.
- Daily rotating snapshots.
- Backup before schema migrations.
- Clean-shutdown marker.
- Startup replay from the latest checkpoint.

Persist immediately after house upgrades, debt payments, trades, large purchases, museum donations, villager moves, player creation/deletion, and mail attachments.

Refactor the PC port's existing GCI load, endian conversion, temporary-file replacement, and backup rotation behind a storage interface instead of discarding it.

## 16. Headless server build

Add `AnimalCrossingServer` with:

```c
TARGET_PC
TARGET_SERVER
NETCODE_ENABLED
```

Server substitutions:

- No-op renderer and audio.
- No SDL window or local input.
- Omit client menus and camera where possible; provide minimal stubs where unavoidable.
- Load only simulation-required data.
- Use a monotonic fixed-step scheduler.
- Use town-specific save paths.
- Structured logs, metrics, crash dumps, and orderly checkpointing.

The server operator may initially need a legitimate disc image if simulation-required data still comes from it.

## 17. Baselines, snapshots, and deltas

Never send `GAME_PLAY`, `ACTOR`, `Save_t`, or `common_data` memory.

A zone baseline contains:

- Server tick and baseline revision.
- Town clock and weather.
- Zone metadata.
- Foreground tile/block chunks.
- Structures and furniture.
- Active villagers and players.
- Important movable actors.
- Relevant shop/event state.
- Revision map.

After baseline delivery, stream deltas newer than its revision. A small Animal Crossing town can initially send the entire outdoor baseline. Acre/block streaming is a later optimization.

Snapshots contain transient state and may be dropped. Transactions and persistent deltas are reliable.

## 18. Interest management

Always relevant:

- Players in the same small interior.
- Clock, weather, and zone-wide events.
- Conversation partners.
- Transaction results.

Distance relevant:

- Outdoor players.
- Villagers.
- Fish/insect actors.
- Temporary gameplay actors and effects.

Static ground state uses block revisions, not continuous replication.

## 19. Time, events, and empty-town simulation

Server configuration defines timezone and time-travel policy:

```toml
timezone = "America/Winnipeg"
clock_mode = "realtime"
allow_time_travel = false
empty_town_simulation = "scheduled"
```

The server owns daily rollover, shop stock, mail delivery, villager schedules, growth, turnip prices, festivals, weather, and moving villagers.

An empty town need not run every rendered frame. On wake, run elapsed-time renewal systems and scheduled multiplayer jobs before accepting players. Clock changes while players are online should require an orderly restart and audit entry.

## 20. Security and anti-duplication

- Authenticated handshake and encrypted session.
- Expiring reconnect tokens.
- Monotonic input and transaction sequences.
- Idempotency key for every economic operation.
- Server-generated random outcomes.
- Strict deserialization bounds and packet limits.
- Rate limits per message type.
- Inventory, shop, tile, house, and mailbox revisions.
- Atomic persistence.
- Structured moderation/audit log.
- Never load client-provided memory or arbitrary save fragments.
- Never trust client inventory, currency, catches, coordinates, or interaction results.

Community servers can begin with local accounts or invitation keys. Platform authentication can be layered on without coupling the core protocol to one provider.

## 21. Repository structure

```text
net/
    include/
        net_protocol.h
        net_client.h
        net_server.h
        net_entity.h
        net_snapshot.h
        net_transaction.h
        net_player_query.h
    src/
        transport_enet.c
        protocol_control.c
        protocol_snapshot.c
        protocol_transaction.c
        client_prediction.c
        remote_player_actor.c
        server_simulation.c
        zone_manager.c
        interest_manager.c

server/
    main.c
    headless_platform.c
    town_runtime.c
    persistence_sqlite.c
    account_store.c
    admin_console.c
    config.c

schemas/
    protocol.fbs
    database/
        001_initial.sql
        002_mail.sql
        003_housing.sql

docs/netcode/
    MASTER_PLAN.md
    ARCHITECTURE.md
    AUTHORITY_MATRIX.md
    PROTOCOL.md
    PERSISTENCE.md
    ROADMAP.md
    CURRENT_STATUS.md

tests/
    net/
    transactions/
    persistence/
    deterministic/
    fuzz/
```

Keep original decompiled source changes small and hook-oriented so upstream updates remain manageable.

## 22. Integration hooks

```c
static void Game_play_move(GAME* game) {
#ifdef NETCODE_ENABLED
    Net_PreSimulation((GAME_PLAY*)game);
#endif

    /* existing simulation */

#ifdef NETCODE_ENABLED
    Net_PostSimulation((GAME_PLAY*)game);
#endif
}
```

Other hooks:

```c
Net_OnActorCreated(ACTOR* actor);
Net_OnActorDestroyed(ACTOR* actor);
Net_OnSceneLoaded(GAME_PLAY* play);
Net_OnPersistentMutation(const WorldMutation* mutation);
Net_OnInventoryMutation(PlayerAccountId player, const InventoryMutation* mutation);
Net_OnSaveRequested(void);
Net_GetInteractionPlayer(void);
```

Hooks report semantic operations, not arbitrary changed bytes.

## 23. Development roadmap

### Phase 0 — Architecture audit (2–4 weeks)

- Build and run the current port.
- Catalogue `GET_PLAYER_ACTOR`, `Now_Private`, and `Common_Get(player_no)` usage.
- Catalogue persistent mutation functions.
- Classify actor profiles as replicated, server-only, or client-only.
- Map scenes and transitions.
- Document random sources and order-dependent simulation.
- Establish protocol/save compatibility tests.

Exit: reviewed, code-backed authority and mutation maps.

### Phase 1 — Network playground (3–5 weeks)

- Integrate transport.
- Add handshake and version negotiation.
- Add stable entity IDs.
- Implement remote-player actor.
- Replicate player transforms in the outdoor scene.
- Add interpolation, disconnects, and reconnects.

Exit: eight clients can remain connected and see one another for an hour.

### Phase 2 — Client-authoritative movement (delivered)

- Headless server target.
- Input command protocol.
- Full-transform input commands and bounded validation.
- No originating-client correction or pullback.
- Remote snapshot buffering, interpolation, and render-rate smoothing.

Exit: movement remains usable under 150–250 ms simulated latency, preserves
original camera-relative controls, and peers see smooth relayed transforms.

### Phase 3 — Shared ground state (5–8 weeks)

- Foreground baseline and deltas.
- Pickup/drop/dig/bury/plant transactions.
- Block revisions and contention handling.
- Operation journal and crash recovery.

Exit: concurrent operations cannot duplicate items or erase unrelated changes.

### Phase 4 — Inventory and economy (5–8 weeks)

- Server inventory and bells.
- Shops, selling, storage, debt, donations.
- Secure trading.
- Mail attachments.

Exit: replay and disconnect tests cannot duplicate value.

### Phase 5 — NPCs and conversations (6–10 weeks)

- Server villager simulation.
- Multiplayer proximity queries.
- Conversation leases.
- Quest ownership.
- Replicated NPC animations and emotions.

Exit: players can interact with separate villagers without corrupting dialogue state.

### Phase 6 — Interiors and housing (5–9 weeks)

- Zone coordinator and transfer tokens.
- Public interiors.
- Four resident houses.
- Furniture transactions.
- Interior presence and reconnects.

Exit: shared interiors and decoration persist correctly.

### Phase 7 — Persistent online town (5–8 weeks)

- Empty-town sleeping.
- Calendar jobs, weather, events, and villager moves.
- Automated backups.
- GCI import/export.
- Admin tools.

Exit: a month-long accelerated soak survives restarts and injected crashes.

### Phase 8 — Production hardening (4–8 weeks)

- Protocol fuzzing.
- Load and soak testing.
- Metrics and observability.
- Moderation.
- Schema migrations.
- Deployment packaging.
- Security review.

A polished public release is approximately an 8–14 month project for a small experienced team. A narrow presence prototype is approximately 1–2 months.

## 24. Testing strategy

### Automated tests

- Protocol encode/decode round trips.
- Truncated and malicious packet fuzzing.
- Prediction and reconciliation.
- Duplicate and reordered requests.
- Inventory and currency conservation.
- Simultaneous tile pickup.
- Shop last-item contention.
- Conversation lease contention.
- Zone transfer during disconnect.
- Checkpoint plus journal replay.
- GCI import/export round trip.
- Restart during a trade.
- Clock rollover while zones sleep.

### Bot clients

Bots should wander, change zones, pick up/drop objects, shop, talk, trade, reconnect, perform conflicting actions, and inject latency, loss, jitter, duplication, and reordering.

Maintain conservation invariants:

```text
items_before + generated_items = items_after + destroyed_items
bells_before + generated_bells = bells_after + removed_bells
```

## 25. Approaches explicitly rejected

- Raw `ACTOR`, `GAME_PLAY`, `Save_t`, or process-memory replication.
- Full save replication every frame.
- Peer-to-peer authority.
- Client-reported catches, purchases, inventory, or currency.
- Multiple ordinary `PLAYER_ACTOR` instances without refactoring sole-player access.
- Multiple towns in one process before global state is isolated.
- Expanding fixed arrays inside the original GCI layout.
- Assuming deterministic lockstep will work by sharing random seeds.
- Making every visual actor authoritative.
- Saving only by rewriting the GCI after every small operation without a journal.

## 26. First vertical slice

Build this before shops, NPC dialogue, or interiors:

1. One dedicated outdoor-town server.
2. Two to eight clients.
3. Remote-player avatars.
4. Client-authoritative movement with bounded transform relay.
5. Server-authoritative ground item pickup/drop.
6. GCI checkpoint plus operation journal.
7. Disconnect/reconnect at last authoritative position.

This validates the headless build, transport, additional players, transform relay, shared mutations, persistence, and recovery.

## 27. Codex repository instructions

Place this plan in `docs/netcode/MASTER_PLAN.md` and create a concise root `AGENTS.md` that requires Codex to read it.

Suggested `AGENTS.md`:

```md
# ACGC Online Development Instructions

This fork adds dedicated-server multiplayer to ACGC-PC-Port.

Before multiplayer work, read `docs/netcode/MASTER_PLAN.md` and
`docs/netcode/CURRENT_STATUS.md`.

Rules:

- The dedicated server is authoritative.
- Never network raw C structs, pointers, or process memory.
- Never trust client-reported economic or interaction outcomes.
- Preserve GCI compatibility where possible.
- Put new multiplayer implementation under `net/` and `server/`.
- Keep original decompiled source changes small and hook-oriented.
- Do not expand fixed arrays inside `Save_t`.
- Use a lightweight actor for remote players.
- Persistent changes pass through the transaction layer.
- Do not distribute copyrighted game assets.

Workflow:

1. Read the master plan and current status.
2. Inspect existing code before changing it.
3. Implement only the authorized roadmap phase.
4. Build and test relevant targets.
5. Update `docs/netcode/CURRENT_STATUS.md` with completed work, tests,
   known issues, and the next recommended task.
6. Do not start the next phase automatically.
```

Initial Codex prompt:

> Read `AGENTS.md`, `docs/netcode/MASTER_PLAN.md`, and `docs/netcode/CURRENT_STATUS.md`. Perform Phase 0 only. Produce a code-backed architecture audit, classify sole-player assumptions and persistent mutation seams, and update `CURRENT_STATUS.md`. Do not begin gameplay networking until the audit is reviewed.

## 28. Definition of success

The mod is ready for public testing when:

- Town state remains correct through simultaneous actions and crashes.
- Clients cannot create items, bells, catches, purchases, or movement authority.
- Movement is responsive at realistic latency.
- Zone transfers and reconnects are reliable.
- Four residents can live in persistent houses.
- Visitors can interact without corrupting resident data.
- NPC schedules, shops, events, mail, museum, weather, and time are server-owned.
- GCI import/export is tested and recoverable.
- Servers can back up, restore, moderate, and upgrade schemas.
- No game assets are included with client or server distributions.
