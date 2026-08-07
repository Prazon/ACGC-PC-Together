# Networked visuals: what a viewer still cannot draw

Status: audit. Category B and C are proposed, not implemented.

Companion to the delivered work in `CURRENT_STATUS.md` → "Remote faces, tools
and locomotion speed". That commit consumed everything a viewer already had on
the wire. This is the sweep for what is still missing, sorted by what it costs.

## A. Delivered

Faces (blink, per-animation eye/mouth tracks, per-state constants), the held
tool's real model under the hand matrix, tool visibility gating, item skeletons,
the umbrella actor, the held balloon, and locomotion animation speed. All of it
was derived from `transform.action`, `animation_item_state` and the replicated
velocity — no wire change.

## B. Not replicated, but derivable on the viewer — no wire cost

These need no protocol change. Each is a viewer-side computation the client can
already perform because it has the same world data.

| Gap | Why it is derivable |
|-|-|
| **Terrain lean.** Only yaw is replicated, so remotes stand bolt upright on every slope. `Player_actor_set_lean_angle` computes pitch/roll from the ground normal under the player. | The viewer has the same collision mesh at the remote's interpolated position, so it can run the same computation. Also fixes the remote balloon's lean target, currently flat. |
| **Per-state shadow.** `Player_actor_SetupShadow` toggles `shape_info.draw_shadow` per main index (off in a pitfall, etc.). Remotes always draw one — `Actor_info_make_actor` defaults it to TRUE. | Keyed on `action`, which is replicated. Same table. |
| **Footprints, ripples, dust, splash.** `Player_actor_SetEffect_Walk` and friends emit these from the keyframe frame plus the ground attribute. Remotes emit none, so they cross sand and snow without a trace. | Both inputs are local: the viewer runs the remote's keyframe itself and can read the ground attribute at its position. |
| **Nameplate.** `remote->name` is replicated and never drawn, so in a crowd there is no way to tell who is who. | Pure presentation; the data is already there. |

## C. Needs new wire fields

Each of these would change the wire format and require a protocol version bump,
which is strict — client and server must be updated together.

| Gap | Field | Cost | Where it belongs |
|-|-|-|-|
| **Bee-swollen face.** `mPlib_Get_UseFaceTexRom_p_common(sex, face, swell, decoy)` picks the face *resource*; the remote loader hardcodes `swell=FALSE`, so a stung remote looks fine. | `player_bee_swell_flag` | 1 bit | Appearance — but see the rate note below |
| **Decoy face.** Same call, `decoy=FALSE` hardcoded. | `player_decoy_flag` | 1 bit | Appearance |
| **Tan.** `mPlib_Get_UseFacePalletRom_p` selects a different face *palette* by `Now_Private->sunburn.rank`; remotes always use the untanned one. | `sunburn.rank` | 3 bits | Appearance |
| **Animation phase.** A one-shot animation restarts from frame 1 when the delta lands, so a viewer sees a swing begin one latency late rather than in progress. | start frame | 1 byte | Presentation delta |
| **Umbrella open/closed.** `umbrella_state` (`aTOL_ACTION_*`) drives the open/close animation. Every remote umbrella is born `S_TAKEOUT` and never animates. | `umbrella_state` | 3 bits | Presentation delta |
| **Item in hand during pickup/scoop.** `main_data.pickup.item`, `get_scoop.item`. The remote plays the pickup animation with empty hands. | item id | 2 bytes | Presentation delta |
| **Golden-tool / sting colour flash.** `change_color_flag` drives a fog override. | flag | 1 bit | Presentation delta |

Rate note worth settling before implementing: the three face-resource bits are
conceptually appearance, but `AppearanceUpdate` has a 1/s rate bucket and
journals every accepted update. A bee sting is transient and would either be
delayed or would burn the bucket. They may be better as presentation bits
despite being "identity", since presentation is change-triggered and not
journalled.

## D. Blocked on a design change, not a wire field

- **Fishing float and line.** `Player_actor_SetActorUki` claims the float with
  `Actor_info_name_search(&play->actor_info, mAc_PROFILE_UKI, ACTOR_PART_BG)` —
  a **scene singleton** placed by the scene spawn tables, which the local player
  writes hand positions straight into. Needs per-player float ownership across
  the fish AI (`ac_gyo_test.c`, `ac_gyo_kaseki.c`) before a remote can have one.
- **Net catch label.** What a remote caught is a server-owned encounter outcome.
- **Opposite-gender body.** Only the local player's gender object bank is
  resident (`Object_Exchange_keep_new_Player` → `mPlib_get_player_Object_Bank`,
  `ACTOR_OBJ_BANK_8` male / `ACTOR_OBJ_BANK_51` female). A remote of the other
  gender resolves its display lists against the wrong bank, re-established at
  every scene load — this is the reported "appearance changes when entering an
  interior" bug. Fixing it means making a second player bank resident, which
  costs memory and belongs to scene resource loading.

## Recommended order

1. **Category B first.** No wire change, no version bump, and it includes the
   two most visible gaps (lean and nameplate). It can also be verified on screen
   independently of any protocol work.
2. **Then D's gender bank**, because it is a correctness bug rather than a
   missing feature, and because it is the one item on this page a player has
   actually reported.
3. **Category C last, as one bump.** Batching every field into a single version
   change is much cheaper than three separate strict-negotiation breaks. Settle
   the appearance-vs-presentation question above before writing any of it.

Nothing on this page should be written before the delivered work in
`CURRENT_STATUS.md` has been seen on screen with a real disc. It is all layered
on the same remote presentation actor, and stacking unverified changes on
unverified changes makes any eventual visual bug much harder to bisect.
