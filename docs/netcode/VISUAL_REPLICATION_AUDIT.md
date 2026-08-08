# Networked visuals: what a viewer still cannot draw

Status: audit. Category B is partly delivered; Category C is delivered
(2026-08-07) and is no longer proposed. See the notes on each below.

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
| **Running lean.** *Delivered 2026-08-07.* Also mis-described here: `Player_actor_set_lean_angle` does **not** read the ground normal. It derives a forward pitch from the body animation's playback speed, so it is a run lean rather than terrain lean, and the viewer that already reproduces that speed gets it for free. Remotes previously left `shape_info.rotation.x` at zero. | Derived from the animation speed the viewer already computes. |
| **Per-state shadow.** *Delivered 2026-08-07.* This entry was **wrong**: remotes did not "always draw one", they drew none at all. `Actor_info_make_actor` does default `draw_shadow` to TRUE, but it also defaults `shadow_proc` to NULL, and `Actor_draw` skips the shadow entirely without one — the remote actor never called `Shape_Info_init`. It now builds the local player's `mAc_ActorShadowCircle` and follows `Player_actor_SetupShadow`'s table from the replicated `action`. | Keyed on `action`, which is replicated. Same table. |
| **Footprints, ripples, dust, splash.** `Player_actor_SetEffect_Walk` and friends emit these from the keyframe frame plus the ground attribute. Remotes emit none, so they cross sand and snow without a trace. | Both inputs are local: the viewer runs the remote's keyframe itself and can read the ground attribute at its position. |
| **Nameplate.** `remote->name` is replicated and never drawn, so in a crowd there is no way to tell who is who. | Pure presentation; the data is already there. |

## C. Needs new wire fields — delivered 2026-08-07 as protocol v20

All of these except the animation phase landed in one bump, as recommended
below. `PlayerAppearanceBits` rides `InputCommand` up and the `Player`
presentation delta back down, so it is change-triggered rather than polled:

- **Bee swell, decoy, tan.** The rate note below was settled in favour of
  presentation. `mPlib_Load_PlayerFaceTexAndPalletEx` is the remote-facing form
  of the two local-player-only resource pickers, taking the three inputs the
  originals read out of `Now_Private` and the town-common block, including the
  tanned-palette branch and its decoy suppression.
- **Umbrella open/closed.** The remote umbrella is now born in the owner's
  actual action rather than always `S_TAKEOUT`, and follows it afterwards. A
  replicated `DESTRUCT` is deliberately never forwarded: the umbrella's
  lifetime belongs to the tool-change path and the actor's `dt`, and honouring
  it from the wire would leave a dangling child.
- **Item in hand during pickup/scoop**, read from the matching branch of the
  `main_data` union only while its own state is running — the union means
  reading `pickup.item` during a scoop would report another state's bytes.
- **Golden-tool / sting colour flash**, replicated but not yet consumed by the
  remote's draw.

**Still outstanding: animation phase.** It was deliberately left out. A plain
"start frame" is not enough — by the time the delta lands the animation has
moved on, so the viewer needs the sender's frame *plus* the elapsed time, and
getting that wrong looks worse than starting from the top. It wants its own
design pass.

Original text, kept for the reasoning:

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
An earlier draft of this page listed a third item here — the opposite-gender
player object bank — as the cause of the reported "appearance changes when
entering an interior" bug. **That was wrong and is withdrawn.**
`cKF_bs_r_boy_1` and `cKF_bs_r_grl_1` are ordinary linked data in
`src/data/model/`, and their joint tables point at linked display lists, so no
bank is involved in drawing a remote body and nothing about it is
scene-dependent. The real cause was that zone-transfer baselines never reached
the players already standing in the destination zone; it is fixed and covered by
a regression test — see `CURRENT_STATUS.md` → "Appearance across a zone
transition".

## Recommended order

1. **Category B first.** No wire change, no version bump, and it includes the
   two most visible gaps (lean and nameplate). It can also be verified on screen
   independently of any protocol work.
2. **Category C last, as one bump.** Batching every field into a single version
   change is much cheaper than three separate strict-negotiation breaks. Settle
   the appearance-vs-presentation question above before writing any of it.

Nothing on this page should be written before the delivered work in
`CURRENT_STATUS.md` has been seen on screen with a real disc. It is all layered
on the same remote presentation actor, and stacking unverified changes on
unverified changes makes any eventual visual bug much harder to bisect.
