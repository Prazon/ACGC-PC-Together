# Authority Matrix

| State | Origin | Commit authority | Replication |
| --- | --- | --- | --- |
| Input | Client | Server validates | Commands, sequenced |
| Player transform | Client predicts | Server | Unreliable snapshots |
| Inventory and bells | Client requests | Server transaction | Reliable delta |
| Held item (the hand) | Client requests | Server transaction, inventory revision | Reliable `Player` delta |
| Player animation | Client authors | Server bounds-checks and forwards | Reliable `Player` delta |
| Bank balance and debt | Client requests | Server transaction, ledger revision | Reliable result |
| Mail and mailboxes | Client requests | Server transaction, mail revision | Account-targeted `Mail` delta |
| Carried letters | Client requests | Server transaction, mail revision | Account-targeted `Mail` delta |
| Tiles and ground items | Client requests | Server transaction | Reliable `Tile` delta, carrying the acting account and `TileChangeCause` |
| NPC transform/animation | Client (each client simulates its own villagers) | Not committed | Not replicated |
| NPC schedule state | Server | Server, hourly job | Zone baseline only |
| Conversation | Client requests | Server lease owner | Reliable lease/result |
| Zone transition | Client requests | Coordinator | Reliable transfer token |
| House/furniture | Client requests | Server transaction | Reliable revisioned delta |
| Resident roster (who owns each of the four houses) | Server | Server, from the persistent account table | Baseline plus town-wide `Resident` delta |
| Island tiles and ground items | Client requests | Server transaction, zone 300 | Reliable `Tile` delta, as above |
| Island cabin furniture | Any client present in the cabin | Server transaction, shared house | Re-baseline to the cabin's occupants |
| Island acre layout | First client that can read it | Server, once, then immutable | Baseline tiles |
| Islander | Server | Server, conversation lease | Snapshot plus reliable results |
| Clock/weather/events | Server | Coordinator | Baseline/change event |
| Camera/UI/audio/effects | Client | Client | Not replicated |
| Checkpoint/export | Admin/server | Storage layer | Out of band |
| Operator gifts (bells, mail) | Operator process | Same server authority, journalled and audited | Account-targeted `Mail` delta |

Clients never submit an authoritative economic, item, catch, collision, or
persistent world outcome.

Villagers are the one row above that is *not* server-driven, and the matrix used
to claim otherwise. `NpcAuthority` owns conversation leases, event leases, and an
hourly `schedule_state`, but no NPC transform or animation is ever simulated or
replicated: every client runs the original villager code locally, so two players
do not see a villager standing in the same place. `NpcState::animation` and
`NpcState::emotion` ride the zone baseline and are never written. Making
villagers authoritative is a roadmap phase, not an oversight in this one.

What a player is holding is inventory state, not appearance: it lives in
`InventoryState::equipped`, moves only through the `HoldItem` transaction, and
is what every tool check reads. A shovel in a pocket does not dig. Equipping was
once a purely local move out of a pocket, which the next authoritative
projection undid by restoring the pocket while the tool was still in hand --
duplicating it.

Animation is the one field a client authors that other clients render directly.
It is bounded rather than derived: the server rejects an `InputCommand` whose
`action`, animation indices, part table, or item state fall outside the original
enums (`kPlayerActionCount`, `kPlayerAnimationCount`, `kPlayerPartTableCount`,
`kPlayerItemStateCount` in `acnet/types.hpp`), because every one of those values
indexes a fixed table on the receiving client. A client can therefore lie about
its own pose, which is cosmetic, but cannot make another client read out of
bounds.

The island cabin is the one room with no owner. `Save_t.island.cottage` belongs
to the town rather than to a resident — all four original residents already
shared it — so `HousingAuthority` registers it as a *shared* house and standing
in its zone is the whole authorization. That is deliberately weaker than the
ownership test every other room uses, and it is the only place a player may
change state another player owns nothing of. Anti-grief is presence plus the
audit trail, not ownership.

Operator gifts (`--grant-bells`, `--send-mail`) are not a side channel around
that rule: they run inside the server process through `EconomyAuthority`, bump
the same ledger and mailbox revisions, are appended to the journal before
success is reported, and are recorded in `audit_log`. The `AdminGrantBells` and
`AdminSendMail` operation types sit above `kMaximumClientEconomyOp`, so the
request codec and `EconomyAuthority::apply` both refuse them from a client.

