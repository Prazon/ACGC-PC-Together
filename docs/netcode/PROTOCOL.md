# Dedicated town protocol v15

`kProtocolVersion` in `net/include/acnet/types.hpp` is the source of truth;
negotiation is strict (`min == max`), so a version mismatch is a clean
rejection rather than a partial session.

The client and dedicated server communicate over UDP. Every packet uses an
explicit bounded wire codec; native C/C++ structs, pointers, actor memory, and
save-memory blocks are never transmitted.

## Town occupancy

`Baseline` carries `town_population` and `town_capacity` as two u8s alongside
the clock and weather fields. These are town-wide and unrelated to the
baseline's `players[]`, which is only the viewer's interest set. A population
of 0 means "not reported" and must not be rendered; capacity is never 0, and
a decoder rejects `capacity == 0` or `population > capacity`.

Because baselines only arrive on join and zone transfer, the count is kept
live by `ResourceKind::Town` replication deltas (2-byte payload: population,
capacity), published by the server whenever the connected count changes.
Like `Clock` and `Weather`, a `Town` delta is town-wide: it bypasses zone and
distance interest filtering.

## Resident roster

`Baseline` carries the four original resident slots after the inventory bells
field, 18 bytes each in slot order: `occupied` (u8, 0 or 1), `gender` (u8, 0-2),
`account` (u64), `name` (8 bytes in the game's own encoding). A vacant slot must
be entirely zero — a decoder rejects a name, gender, or account attached to
`occupied == 0`, so a peer cannot smuggle an identity past a reader that only
consults the flag. An occupied slot must name a non-zero account.

This is ownership, not presence: it covers residents who are logged out, and it
is not derivable from `players[]` or from `TransformSnapshot`. A connected
client's own `Save_t.private_data` holds only the account it logged in as, so
without the roster every other house reads as vacant — most visibly on the town
map, which labels each of the four houses with its owner.

`ResourceKind::Resident` deltas carry the same 72-byte encoding and keep the
roster live between baselines; the server publishes one from the tick whenever
the roster differs from the last one published, which covers a first login
claiming a slot, an appearance change, and a GCI import alike. Like `Town`, a
`Resident` delta is town-wide and bypasses zone and distance filtering — the map
is usually opened from indoors.

## Encounters

`EncounterRequest` carries the `species` the client observed itself hooking or
swinging at, as an item identifier (`ITM_FISH_START + n`, `ITM_INSECT_START + n`).
It does not name the rod or net: the server reads whatever the account is
authoritatively holding, so a tool sitting in a pocket cannot catch anything.
Fish and insect spawns are still simulated on the client, so this is a *claim*,
not an outcome. The server accepts it only if that species can legally appear at
the town's current month, hour, and weather; an illegal claim is rejected with
`InvalidState` and spends no cooldown, and the decoder rejects an identifier
outside the species range for the request's kind before the authority sees it.
The server alone decides whether the catch succeeds and commits the inventory.

Zero means "no claim", and the server then picks uniformly from every species
legal at that moment.

Availability lives in `net/src/encounter_tables.inc`, generated from the
original spawn overlays by `tools/gen_encounter_tables.py`; run that script with
`--check` to confirm the committed table is not stale. Fish are accepted in
either half of the month because the original blends the outgoing half's fish in
over a randomised transition of up to five days. The coelacanth, bee, and ant
are absent from those tables because the original spawns them through dedicated
paths, and are allowed on the same terms — the coelacanth only while raining or
snowing, the other two unconditionally.

## Mail and banking

Banking is an `InventoryRequest` transaction family: `Deposit`, `Withdraw`, and
`PayDebt` quote the observed `AccountLedger` revision, and the accepted result
returns the committed balance, debt, and the next ledger revision. Bells never
move because a client said so.

A letter is a bounded server-side record carried whole so the original UI can
render it unchanged: identifier, sender account, recipient account, one attached
item, revision, location, and the letter text. The text fields (sender name,
header, body, footer) are opaque bytes in the game's own font encoding, sized to
the matching `Mail_c` fields, so nothing reinterprets player-written content.

Mail moves in the same two steps as the original game, and both are
transactions. `TakeMail` moves a letter from the recipient's house mailbox into
the letters they carry; `ClaimMail` then moves that carried letter's present
into their pocket, leaving the letter itself in hand with an empty attachment.
`DiscardMail` throws away a letter that no longer holds a present -- refused
while one does, since that would destroy the item. `AttachMail` posts a letter
into someone else's mailbox.

Both halves are bounded at ten, matching `HOME_MAILBOX_SIZE` and
`mPr_INVENTORY_MAIL_COUNT`, and a full one is refused with `Capacity` rather
than dropping the item. One revision covers both halves of an account's mail, so
a client always quotes a single observed value; every delivery, take, claim, and
discard bumps it. Every operation checks that the letter is addressed to the
acting account, and replaying an idempotency key returns the original result
instead of moving anything twice.

`Baseline` carries the viewer's own mail revision and every letter they own, in
mailbox order then carried order. Between baselines, `ResourceKind::Mail` deltas
keep it live: they are always addressed to one account, so the recipient learns
about a letter wherever they are standing.

`EconomyOpType` values at or below `kMaximumClientEconomyOp` (`ClaimMail`) are
the only ones a client may send. The operator operations above it
(`AdminGrantBells`, `AdminSendMail`) are refused by the request codec in both
directions and by `EconomyAuthority::apply`, so they exist only inside the
server process. `EconomyResult` echoes the operation type, so a client knows
whether `auxiliary_revision` refers to the shop, the museum, the bank ledger,
or a mailbox.

## Connection and security

`ClientHello` negotiates the protocol/build, town, account, invitation proof,
and optional reconnect credential. `ServerHello` returns the result, stable
player entity, server tick, short-lived reconnect credential, server town seed,
native land ID/name, assigned resident slot, and canonical-world readiness.
All of those identity fields are included in the authenticated server proof.
Invitation and server proofs are authenticated. Session traffic uses independently
derived client-to-server and server-to-client keys with authenticated
encryption; unauthenticated or replayed packets are rejected before dispatch.

Reconnect credentials are signed, account/town scoped, expiring, and safe to
use from a changed UDP endpoint. Accounts may have only one active session.

## Channels

| Channel | Delivery | Uses |
| --- | --- | --- |
| Control | reliable ordered | handshake, ping, disconnect, zone transfer |
| Transactions | reliable ordered | world, inventory, trade, furniture, house-update results, encounters |
| Chat | reliable ordered | reserved text/emote channel |
| Snapshots | unreliable sequenced | client transforms and replicated player snapshots |
| Events | reliable sequenced | replication deltas and state events |
| Bulk | reliable fragmented | zone baselines, full house updates, and first-town bootstrap |

Reliable packets use sequence acknowledgements, a selective acknowledgement
window, retransmission limits, duplicate suppression, and ordered dispatch.
Bulk payloads are bounded, fragmented, individually validated, and reassembled
under memory/time limits.

## Authority

The original client controller and terrain collision own player movement.
Protocol v11 `InputCommand` carries the resulting complete transform plus its
input sequence. The server rejects non-finite/out-of-bounds transforms and
stale sequences, but does not resimulate, pull, or correct the originating
client; it relays the accepted transform to peers. Presentation uses a
six-tick transform history plus render-rate position/facing smoothing. History
is cleared across zone changes. A client does not acknowledge a zone handoff
until its destination scene exists; `ZoneReady` includes the exact bounded
destination transform, so peers never observe the server's placeholder
position. Entrance movement begins streaming immediately, including during the
original scene-start animation.

Clients send semantic requests, never persistent results. Such requests
contain a random idempotency key plus the observed tile, inventory, shop,
trade, house, or mailbox revision. The server validates identity, zone, tool,
cooldown, capacity, ownership, currency, and revision; commits the accepted
state; then returns a result and publishes a delta. Door requests validate the
current source zone, registered route, and destination capacity. Their spatial
collision is client-owned, matching movement authority.

World operations are drop, pickup, dig, bury, plant, chop, outdoor
furniture placement/removal, and hole filling. Separate request families cover
shops/banking/debt/donations/mail/mailboxes, escrow trades, conversation leases, house
furniture, fish/insect outcomes, and signed zone handoffs.

Neither a world operation nor an encounter names the tool it needs. Both read
`InventoryState::equipped` — what the player is actually holding — which moves
only through the `HoldItem` inventory operation, a swap between one pocket slot
and the hand. Equipping, putting away, and swapping tools are that one
operation, so it can neither create nor destroy an item, and it quotes the
inventory revision like every other inventory request. A client used to name the
slot it wanted checked, which made "owns a shovel" and "is holding a shovel" the
same thing.

## Player presentation

What another player's skeleton is doing is replicated in the original game's own
indices. `InputCommand` carries a five-byte animation block after the transform:
`body` and `overlay` (`mPlayer_ANIM_*`, driving keyframe0 and keyframe1),
`part_table` (`mPlayer_PART_TABLE_*`, selecting which joints come from which),
`item_state` (`mPlayer_ITEM_MAIN_*`), and a flags byte — bit 0 looping, bit 1
reversed, all other bits must be zero.

Every one of those values indexes a fixed table on the receiving client, so the
decoder rejects anything at or above the matching `kPlayer*Count` in
`acnet/types.hpp`, and `MovementSimulator::submit` rejects the command again
before it reaches any authority. `InputCommand.action` and the transform's
`action` are bounded the same way against `kPlayerActionCount`.

The server republishes a player's presentation only when it changes, as a
zone-scoped reliable `ResourceKind::Player` delta: `account` (u64), `entity`
(u64), the same animation block, and `equipped_item` (u16). The zone baseline
carries the identical block per player so a joining client starts in the right
pose.

Presentation is deliberately absent from `TransformSnapshot`. A full sixteen-
player snapshot already sits near the unfragmented MTU, and the snapshot channel
is unreliable — a dropped animation transition would leave a viewer holding the
previous pose indefinitely, which is a stuck state rather than a stale one.

Appearance carries identity only: name, gender, face, clothing, and the custom
pattern. It no longer carries the held item, so changing tools is not an
appearance change. `AppearanceUpdate` has its own rate bucket (1/s, burst 4)
because each accepted one journals and re-baselines every connection, and the
server now skips both when nothing visible actually changed.

## Tile deltas

A `ResourceKind::Tile` delta carries the tile address, the committed
`TileState`, and then two fields describing *how* the tile came to change:
`actor` (u64) and `cause` (u8).

|Field|Meaning|
|-|-|
|`actor`|The account whose operation committed the change, or 0 when the server changed the tile on its own|
|`cause`|`TileChangeCause`: 0 `Server`, then 1–9 mirroring `WorldOpType` — `Drop`, `Pickup`, `Dig`, `Bury`, `Plant`, `ChopTree`, `PlaceFurniture`, `RemoveFurniture`, `FillHole`|

`TileChangeCause` is appended-never-inserted; the decoder rejects a value above
the last enumerator. `actor` is 0 exactly when `cause` is `Server`, which is
what overnight plant growth, a GCI import, and operator commands publish.

The pair exists so a viewer can *present* the change rather than only observe
it. A drop is animated: the receiving client arcs the item out of the named
player's hand onto the tile, and the drop actor writes the field cell when it
lands. Without the actor the arc has no origin, and two players standing
together are indistinguishable; without the cause a sapling growing overnight
would animate as though somebody had thrown it.

A tile delta is reliable and zone-scoped, so it is not distance-culled — a drop
anywhere in the zone reaches every client in it, whether or not the tile is in
their interest chunk.

Clients keep tile deltas in a bounded queue (256) separate from the baseline
mirror, drained by the game layer so each change can be presented individually.
An overflow is reported rather than silently dropped: the reader then reprojects
the whole interest chunk. A `Baseline` supersedes the queue and clears it,
because it is the whole truth for that chunk and replaying older changes over it
would regress state.

House baselines carry three floors of bounded room cells, canonical inventory
item IDs plus furniture facing, furniture switch bits, light state, and music.
The owner may bootstrap a house once. Later full-room updates atomically consume
added items from authoritative inventory and return removed items; moves and
rotations conserve the item multiset. Free size changes and invented furniture
are rejected. Every client in the house receives the accepted baseline.

Snapshots also carry a four-house light mask sourced from each authoritative
house's `main_light_on` state. The original client uses that switch for the
exterior and retains its normal day/night presentation. Bounded door-transition
metadata and a two-second source ghost let peers finish the original leaving
animation after the authoritative zone handoff; the destination presents the
matching arrival at the client-reported doorway transform. This transition
state is presentation-only and never causes another scene change.

Malformed values, oversized collections, invalid enums, non-finite floats,
incorrect channels/sessions, failed authentication, stale revisions, and
message-rate excess are rejected. Packet and transaction parsers are exercised
by the bounded fuzz harness in `tests/fuzz`.

## Canonical town creation

An online client waits for an authenticated `ServerHello` before original town
generation. The server seed and native eight-character identity are applied to
the local save, so every first resident generates the same layout and skips
local town naming. The first resident then sends `TownBootstrap`: identity,
appearance, and exactly 7,680 explicit `{item, buried}` foreground tiles. The
message is bounded and fragmented; no native save structs are transmitted.

The server validates resident ownership and identity, accepts the first valid
foreground as canonical, persists it before returning success, and publishes
fresh baselines. Later residents may refresh their appearance but cannot
replace the established world. `TownBootstrapResult` gates gameplay replication
until initialization succeeds. State schema v4 persists the initialized flag,
and reconnecting clients receive it in `ServerHello`.

`TownBootstrap` ends with an island section: two acre column indices and either
zero or exactly 512 further `{item, buried}` tiles, being the island's two
16x16 acres read from `Save_t.island.fgblock` rather than the town foreground.
The island is placed by the town's acre layout rather than at a fixed
coordinate, which is why the client reports the columns instead of the server
assuming them; a client that cannot yet read the acre kinds sends no island
section, and the server waits for one that can. This is also the migration
path for a town created before the island had a zone: the island section is
adopted on any login while the island is still empty, and refused afterwards on
the same "cannot replace an established world" rule as the town foreground.

## The island

The island is two acres of the same outdoor field, at block row 8 with its
columns discovered from the acre kinds. Its ground items live in a separate
save region, so it is a separate zone (300) whose tiles keep their **global**
unit coordinates -- the island acres sit outside the town's rectangle, so the
two zones cannot collide, and the original `mFI_*` foreground helpers route an
island write into island storage with no translation at either end. The cabin
(301) and the islander's hut (302) are shared interiors.

The Kapp'n ferry never changes scene, so no door animation announces it. The
client notices its acre kind changed and requests the ferry door (60 outbound,
61 return); `request_transfer` validates the source zone and capacity, not
proximity, exactly as it already does for building doors whose generated
coordinates are not stable enough to check.

The cabin is a **shared house**: `Save_t.island.cottage` belongs to the town
rather than to any resident, so it is registered with no owner and reported on
the wire by the ownerless pair `owner = 0`, `original_slot = 0xFF`. Presence in
its zone is the whole authorization -- `HousingAuthority` skips the ownership
test for a shared house and stops treating the zone test as optional. It is one
room, so a floor index above 0 is refused. Furniture still moves through the
same revisioned, journalled `HouseUpdate`/`FurnitureRequest` transactions as
every other room.
