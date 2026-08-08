# Remote player held items and facial expressions

Status: **implemented** (2026-08-06; audited against the tree 2026-08-07).
Everything in §0's ship table landed in `ac_net_remote_player.c` /
`m_player_lib.c`. The delivery entry is CURRENT_STATUS.md § "Remote faces,
tools and locomotion speed (2026-08-06)"; the outcome section below records
where reality diverged from this plan. The §8 on-screen visual pass is still
outstanding.

Goal: a remote player's face animates and their tool renders the way the local
player's does — one commit, no protocol change, no server change, no version
bump.

## Outcome (2026-08-07 code audit)

Confirmed in the tree, section by section:

- **§2 faces** — `mPlib_Face_Reset`/`mPlib_Face_Step` (`m_player_lib.c:2246`,
  `:2258`), `mPlib_face_state_c` (`m_player_lib.h:76`), the blink stepper with
  its duplicated 16-entry `pattern_table` (`m_player_lib.c:2191`), and the
  segment-offset draw (`ac_net_remote_player.c:1055`).
- **§3 held items** — hand-matrix capture into `right_hand_mtx`, GFX/skeleton
  draw dispatch, visibility gated on `animation_item_state`, and the resolved
  `SetupItem_Base2` ordinal table with its comment
  (`Net_Remote_Player_item_anim`, `ac_net_remote_player.c:311`). Implementation
  added a guard this plan did not call for: the state/kind pairing is
  re-validated before it can reach a skeleton (`:351`), because `item_state`
  and `equipped_item` come from different authorities and can disagree.
- **§3.4 `item_scale`** — the take-out/put-in ramp is implemented, keyed on
  `remote->action` (`Net_Remote_Player_update_item_scale`, `:785`).
- **§4.1 umbrella** — real `TOOLS_ACTOR` child via `aTOL_birth_proc`,
  destructed on tool change and in `_dt` (`:407`–`:449`).
- **§6 struct additions** — all present, plus `loaded_face` and a balloon
  state block this plan did not anticipate (see the §4.2 correction).
- **§7 test** — the production-client loopback case asserts
  `transform.action` survives the round trip (`tests/net/test_main.cpp:3727`).
- **§5's doc row for `GAMEPLAY_COVERAGE_AUDIT.md`** — Finding 10 had *not*
  been corrected when the code landed; fixed 2026-08-07.

Where reality diverged:

- **§4.2 misread the balloon.** `Player_actor_init_value`'s
  `mAc_PROFILE_BALLOON` child is the *released* balloon; the held balloon is
  not a sub-actor at all — `Player_actor_Item_draw_balloon` draws it from
  `item_keyframe` under its own matrix. The remote does the same: sway, lean
  and string animation are transcribed against per-remote state (`balloon_*`
  fields, `Net_Remote_Player_reset_balloon`). Known wrinkle: the lean chases
  `-shape_info.rotation.x` (terrain pitch), which is not replicated, so a
  remote balloon's lean target is flat.
- **Delivered beyond this plan: walk-cycle speed.** The remote previously
  hardcoded `frame_speed` 1.0 while the original assigns
  `0.6·sqrt(speed/7.5)` (min 0.22), so every remote walk cycle ran 1.7–4.5×
  too fast. It is now derived from the replicated velocity, with
  `READY_WALK_NET`'s own constants (`ac_net_remote_player.c:250`).

Still outstanding:

- **§8 item 3, the on-disc visual pass** — the real gate; none of this has
  been drawn on screen. `run_four_clients.bat` now stages four clients for it.
- **§10's deferrals stand**: no fishing float/line, no net catch label, no net
  bag lean, no remote sub-actor audio.

## 0. Scope

Ships in this commit:

| | |
|-|-|
| Facial expressions | Blink, per-animation eye/mouth texture tracks, and the per-state constant expressions |
| Held item model | The real tool model/skeleton under the hand joint matrix, replacing the world-item model at a bare position |
| Held item visibility | Gated on the replicated item state instead of "inventory is non-empty" |
| Held item animation | The item's own 8-joint skeleton (net bag, rod flex, pinwheel, balloon) |
| Umbrella sub-actor | Per-remote `TOOLS_ACTOR` child, positioned exactly as the local player positions its own |
| Balloon sub-actor | Per-remote `mAc_PROFILE_BALLOON` actor |

Explicitly **not** in this commit, with reasons that are structural rather than
effort-based:

- **Fishing float (uki).** `Player_actor_SetActorUki`
  (`m_player_other_func.c_inc:303`) does
  `Actor_info_name_search(&play->actor_info, mAc_PROFILE_UKI, ACTOR_PART_BG)` —
  the float is a **scene singleton** placed by the scene spawn tables in
  `src/data/scene/*.c`, and the local player claims it by search and writes
  `command`, `right_hand_pos`, `left_hand_pos`, and `rod_top_position` straight
  into it. Two players fishing would fight over one actor. Giving remotes a
  float means either spawning additional UKI actors and teaching every consumer
  (`ac_gyo_test.c`, `ac_gyo_kaseki.c`, the fish AI) which one it belongs to, or
  a per-player float ownership field. That is a design change to a gameplay
  actor, not presentation work, and it does not belong in the same commit as
  everything above. **A remote player casting a rod will show the rod and the
  cast animation with no float and no line.**
- **Net catch label.** `player->item_net_catch_label` is a `TOOLS_ACTOR`
  populated from the encounter path; what a remote caught is a server-owned
  encounter outcome, not presentation. Belongs with the encounter work.

Both exclusions must be written into `CURRENT_STATUS.md` as known limitations,
replacing the two entries that currently cover them more vaguely.

## 1. Why this costs nothing on the wire

Three values the remote actor needs are already replicated and already reaching
`ac_net_remote_player.c`, unread:

| Value | Source | Path to the actor |
|-|-|-|
| `now_main_index` | `Net_CapturePlayerTransform` (`m_net_hooks.c:634`) writes it to `transform.action` | `encode_transform` (`replication.cpp:134`) → `TransformSnapshot` (`protocol.cpp:465`) and the zone baseline → `blend()` (`interpolation.cpp:23`, nearest-neighbour, so it survives interpolation) → `to_c` (`c_api.cpp:37`) → `states[i].transform.action` |
| `now_item_main_index` | `Net_CapturePlayerAnimation` (`m_net_hooks.c:610`) | reliable `Player` delta → `states[i].animation_item_state` |
| `animation0_idx` | same | `states[i].animation_body` — already consumed for the body pose |

Everything in §2 and §3 derives from those three plus data the viewer already
has. No new fields, no `PROTOCOL.md` change, no `kProtocolVersion` bump, and
nothing in `net/` or `server/` is touched.

Replicating an explicit eye/mouth byte instead would be strictly worse: it
changes on most frames during a blink or a conversation, so it cannot ride the
change-triggered reliable delta, and putting it in the unreliable snapshot costs
~240 B/s at sixteen players while still being wrong between packets.

## 2. Facial expressions

### 2.1 What drives the local player

`eye_tex_idx` / `mouth_tex_idx` (`m_player.h:2095`) select one of 8 eye and 6
mouth tiles of 0x100 bytes each. Three things write them:

1. **Blink** — `Player_actor_set_eye_pattern_normal` (`m_player_common.c_inc:900`).
   A random timer walks a 16-entry `pattern_table`. Purely cosmetic.
2. **Per-animation tracks** — `Player_actor_set_tex_anime_pattern` (`:959`).
   Indexes `mPlib_Get_PlayerEyeTexAnimation_p(anim0_idx)` /
   `mPlib_Get_PlayerMouthTexAnimation_p(anim0_idx)` at
   `(int)(keyframe0.current_frame - 1.0f)`, gated on
   `1.0f <= current_frame <= max_frames`. 33 eye tracks and 35 mouth tracks
   across the 157 animations — axe swing, dig, shake tree, eat, `yatta`,
   `gaaan`, mosquito, bee sting, trip-and-fall.
3. **Per-state constants** — seven call sites, listed in §2.2.

`Player_actor_SetupTextureAnimation` (`:985`) additionally resets both to 0 on
entering a flagged state. The viewer model below recomputes from scratch every
frame, so that reset is implicit and does not need porting — this is why the
121-entry flags table does **not** have to be duplicated.

### 2.2 The viewer-side model

Each frame, for each remote, in order:

1. If `mPlib_Get_PlayerEyeTexAnimation_p(animation_body)` is non-NULL and the
   remote's own `keyframe0.frame_control` is in `[1.0, max_frames]`, take the
   eye index from the track. Same for mouth.
2. For whichever of the two has no track: eye ← blink stepper, mouth ← 0.
3. Apply overrides.

The override set collapses further than the raw call sites suggest. `TALK`
(`m_player_main_talk.c_inc:169`) and `SHOCK` (`m_player_main_shock.c_inc:118`)
do not set constants unconditionally — they dispatch on `animation0_idx`:

```
anim == GAAAN1 || anim == BIKU1  ->  set_tex_anime_pattern    (step 1 already does this)
anim == GAAAN2                   ->  eye 6, mouth 5
otherwise                        ->  blink + mouth 0          (step 2 already does this)
```

`GAAAN1` (index 110) and `BIKU1` (118) both have tracks, and `GAAAN2` has none.
So TALK and SHOCK reduce to a single **animation**-keyed override and drop out
of the state table entirely. What remains:

| Key | Kind | eye | mouth |
|-|-|-|-|
| `mPlayer_ANIM_GAAAN2` | animation | 6 | 5 |
| `mPlayer_INDEX_TIRED` | state | 4 | 4 |
| `mPlayer_INDEX_NOTICE_MOSQUITO` | state | 4 | 4 |
| `mPlayer_INDEX_WAIT_BED` | state | 2 | — |
| `mPlayer_INDEX_SWING_FAN` | state | 5 | — |
| `mPlayer_INDEX_STRUGGLE_PITFALL` | state | 6 | — |

Six cases, written as a `switch`, not a 121-entry table.

Fidelity caveat worth stating up front: the remote's keyframe starts when the
animation delta arrives, so a derived track plays at a slightly different frame
than the originator's. For a 16-frame blink or a mouth flap that is invisible,
and there is no cheaper way to phase-lock it that does not put per-frame data on
the wire.

### 2.3 New helpers — `m_player_lib.c` / `m_player_lib.h`

The setters and tables in §2.1 are `static` inside `.c_inc` files compiled into
`m_player.c`, so they are unreachable from the actor. Precedent for the fix is
already in the tree: `mPlib_Load_PlayerFaceTexAndPallet` (`m_player_lib.c:1392`)
was added to this same file for this same actor. Follow it — add pure helpers
that take no `PLAYER_ACTOR`:

```c
typedef struct mPlib_face_state_s {
    s16 blink_pattern;
    f32 blink_timer;
    int blink_count;
    u8  eye_tex_idx;
    u8  mouth_tex_idx;
} mPlib_face_state_c;

extern void mPlib_Face_Reset(mPlib_face_state_c* face);
extern void mPlib_Face_Step(mPlib_face_state_c* face, int main_index, int anim_idx,
                            f32 current_frame, f32 max_frames, f32 dt_frames);
```

`mPlib_Face_Step` implements §2.2. The blink stepper is a transcription of
`Player_actor_set_eye_pattern_normal` with `gamePT->graph->dt_num_60fps_frames`
replaced by the passed-in `dt_frames`; `get_random_timer` is already `extern` in
`m_lib.h`. The 16-entry `pattern_table` is duplicated rather than shared —
**do not refactor the local player to call the new helper.** Sixteen bytes of
duplication is cheaper than any risk to the local player's face, and this file
is decomp that upstream may move.

Bounds: clamp the track index to the track's own length as well as to
`max_frames`. The local code relies on the animation and its track agreeing;
the remote drives the same animation so they do agree, but a bound here is one
line and removes a class of out-of-bounds read.

### 2.4 The draw change

`ac_net_remote_player.c:364` currently pins both segments to tile 0:

```c
gSPSegment(POLY_OPA_DISP++, ANIME_1_TXT_SEG, render->face_texture);
gSPSegment(POLY_OPA_DISP++, ANIME_2_TXT_SEG, render->face_texture + mPlayer_EYE_TEX_NUM * 0x100);
```

becomes

```c
gSPSegment(POLY_OPA_DISP++, ANIME_1_TXT_SEG,
           render->face_texture + render->face.eye_tex_idx * 0x100);
gSPSegment(POLY_OPA_DISP++, ANIME_2_TXT_SEG,
           render->face_texture + (mPlayer_EYE_TEX_NUM + render->face.mouth_tex_idx) * 0x100);
```

No extra loading: `mPlib_Load_PlayerFaceTexAndPallet` already pulls the whole
0xE00 buffer (8 eye + 6 mouth = 14 × 0x100), and `render->face_texture` is
sized for it.

## 3. Held items

### 3.1 What the local player does, and what the remote does now

`Player_actor_Item_draw` (`m_player_item.c_inc:65`) pushes
`player->right_hand_mtx`, scales by `player->item_scale`, and dispatches on
`now_item_main_index` into eight procs — axe, net, rod, umbrella, scoop,
balloon, windmill, fan — two of which run a second skeleton
(`player->item_keyframe`, 8 joints).

`Net_Remote_Player_draw_held_item` (`ac_net_remote_player.c:341`) draws
`equipped_item` through `bg_item_clip->single_draw_proc` /
`shop_goods_clip->single_draw_proc` at a bare position, scale 1.0, no rotation.
That is the **ground/shop pickup model**, not the tool model, unrotated, and it
renders whenever the inventory hand is non-empty — including while stowed, in a
menu, indoors, or mid-pickup.

The feasibility fact that makes all of this cheap: item models, skeletons, and
animations come from `mPlib_Get_Item_DataPointer` (`m_player_lib.c:661`), a
static table of global symbols in the player object bank. There is no per-actor
DMA — `Player_actor_Change_ItemBank` is index bookkeeping only. A remote actor
can index that table directly, and every accessor it needs
(`mPlib_Get_ItemNoToItemKind`, `mPlib_Get_BasicItemShapeIndex_fromItemKind`,
`mPlib_Get_BasicItemAnimeIndex_fromItemKind`, `mPlib_Get_Item_DataPointerType`)
is already `extern` in `m_player_lib.h`. **The item side needs no decomp edits
at all.**

### 3.2 What the remote will do

Capture the matrix, not just the position. `Net_Remote_Player_draw_after`
(`:319`) currently does only `Matrix_Position_Zero(&render->hand_pos)`; add
`Matrix_get(&render->right_hand_mtx)` beside it, mirroring
`Player_actor_draw_After_hand` (`m_player_draw.c_inc:128`).

Per frame in `_move`:

```
kind    = mPlib_Get_ItemNoToItemKind(equipped_item)
visible = animation_item_state != mPlayer_ITEM_MAIN_NONE && kind >= 0
shape   = mPlib_Get_BasicItemShapeIndex_fromItemKind(kind)
anim    = item_anim_for_state(animation_item_state)          /* §3.3 */
```

If `mPlib_Get_Item_DataPointerType(shape) != mPlayer_ITEM_DATA_TYPE_GFX` and
`(shape, anim)` changed since last frame, rebuild the item skeleton with
`cKF_SkeletonInfo_R_ct` +
`cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode`, mirroring
`Player_actor_Item_DMA_Data` (`m_player_item_common.c_inc:198`) minus the
double-bank logic, which exists to avoid a re-DMA the remote never performs.
Then `cKF_SkeletonInfo_R_play`.

In `_draw`, after the body skeleton, replacing `Net_Remote_Player_draw_held_item`
wholesale:

```
Matrix_push(); Matrix_put(&render->right_hand_mtx);
if (scale != 1.0f) Matrix_scale(scale, scale, scale, MTX_MULT);
gSPMatrix(_Matrix_to_Mtx_new(graph))
  GFX-type shape      -> gSPDisplayList(mPlib_Get_Item_DataPointer(shape))
  SKELETON-type shape -> cKF_Si3_draw_R_SV(game, &render->item_keyframe,
                                           render->item_work_mtx[frame & 1], NULL, NULL, NULL)
Matrix_pull();
```

That covers axe, scoop, umbrella, windmill and fan (GFX) and net and rod
(skeleton).

**Deliberate simplification to state in the code comment:** the local procs also
do `Matrix_Position_VecZ` captures into `axe_pos`, `net_pos`,
`net_top_col_pos`, `net_bot_col_pos`, `net_start_pos`, `net_end_pos` and
`item_rod_top_pos`. Every one of those feeds collision or the fishing state
machine. A presentation actor has neither, so the remote reimplements *rendering
only* and skips all of them. The net's after-callback
(`Player_actor_Item_draw_net_After_dummy_net`, `m_player_item_net.c_inc:213`)
applies `net_angle` to joint 3 — the bag lean, computed from
`keyframe0.frame_control.speed` and `shape_angle_delta.y`. Omit it in this
commit; the bag sits at its rest angle. Note it in `CURRENT_STATUS.md`.

### 3.3 The item animation table

Dominant path: `Player_actor_SetupItem_Base0/1` (`m_player_common.c_inc:3921`,
`:3963`) uses `mPlib_Get_BasicItemAnimeIndex_fromItemKind(item_kind)`, derivable
from `equipped_item` alone. That is the default.

The exceptions are the 13 `Player_actor_SetupItem_Base2` sites (swing net, pull
net, stop net, putaway net, notice net, ready/cast/air/relax/collect/vib/fly rod,
putaway rod). Each passes `(item_anim_idx, item_main_index)`, and
`item_main_index` is exactly what lands in `now_item_main_index` and therefore
in the replicated `animation_item_state`. So the table is a direct read-off:
`item_state -> item_data_anim_index`, at most 24 entries, ~13 non-default.

**Hazard.** Those call sites pass `mPlayer_ANIM_*` and `mPlayer_INDEX_*`
constants where `mPlayer_ITEM_DATA_*` and `mPlayer_ITEM_MAIN_*` are expected —
the decomp flags this itself at `m_player_common.c_inc:3977` ("Usage in the
calls seem wrong (fairly random indexes)"). They are mislabelled constants that
happen to have the right ordinals. **Build the table from resolved numeric
values, not from the constant names**; a one-off translation unit that prints
the ordinals of both enums is the reliable way, and the resolved mapping should
go in the commit as a comment next to the table.

### 3.4 `item_scale`

`Player_actor_SetupItemScale` (`:1151`) pins it to 1.0 outside
`mPlayer_INDEX_TAKEOUT_ITEM`; `m_player_main_takeout_item.c_inc:91` and
`m_player_main_putin_item.c_inc:86` ramp it. Both are `action`-keyed, so the
ramp is derivable from `transform.action` plus the remote's own frame counter.
Implement it — it is a few lines once §2 has already established reading
`action`, and without it every tool pops in at full size.

## 4. Sub-actors

### 4.1 Umbrella

Clean, because the tool actor is already decoupled from the player.
`ac_t_umbrella.c:298` draws from `tools_class.matrix_work` and
`parent_actor->drawn`; the *player* writes that matrix in
`Player_actor_Item_draw_umbrella` (`m_player_item_umbrella.c_inc:6`) with three
lines:

```c
Matrix_get(&umbrella->matrix_work);
umbrella->init_matrix = TRUE;
```

The remote does exactly that under the hand matrix. Birth via
`Common_Get(clip).tools_clip->aTOL_birth_proc(kind - mPlayer_ITEM_KIND_UMBRELLA00,
aTOL_ACTION_S_TAKEOUT, (ACTOR*)remote, game, -1, NULL)`, which makes it an
`Actor_info_make_child_actor` of the remote (`ac_tools.c:81`). Destruct through
`aTOL_chg_request_mode_proc(..., aTOL_ACTION_DESTRUCT)` in `_dt` and whenever
the item kind changes, mirroring `Player_actor_LoadOrDestruct_Item`
(`m_player_item_common.c_inc:356`).

The `aTOL_check_data_bank` cleanup branch (`ac_tools.c:40`) is gated on
`actor->part == ACTOR_PART_PLAYER`; the remote is `ACTOR_PART_CONTROL`, so it
does not fire — correct, since it exists to clear a stale child on the local
player.

### 4.2 Balloon

**Corrected after implementation — the plan's premise here was wrong.** The
original text said to mirror `Player_actor_init_value` (`m_player.c:521`)
spawning one `mAc_PROFILE_BALLOON` per player. That child is the *released*
balloon — the one that floats away — and the held balloon is not a sub-actor
at all: `Player_actor_Item_draw_balloon` draws it from `player->item_keyframe`
under its own matrix. As implemented, the remote does the same, transcribing
movement, sway and lean against per-remote state (`balloon_*` in
`NET_REMOTE_RENDER_DATA`, reset in `Net_Remote_Player_reset_balloon`) and its
own hand-position delta. The prediction held in one respect: it was the
largest single piece of the commit, and it carries the one known visual
wrinkle — the lean target is terrain pitch, which is not replicated, so a
remote balloon leans as if on flat ground.

## 5. File-by-file

| File | Change | Rough size |
|-|-|-|
| `src/actor/ac_net_remote_player.c` | Read `transform.action`; face state + segment offsets; item skeleton, hand matrix, gating, GFX/skeleton draw, `item_scale`; umbrella and balloon sub-actors | +350–450 lines |
| `include/ac_net_remote_player.h` | `action`, umbrella/balloon actor pointers on `AC_NET_REMOTE_PLAYER` | +5 |
| `src/game/m_player_lib.c` | `mPlib_Face_Reset`, `mPlib_Face_Step`, blink stepper | +90 |
| `include/m_player_lib.h` | `mPlib_face_state_c` + two prototypes | +12 |
| `docs/netcode/CURRENT_STATUS.md` | Delivered work; replace the three stale limitation bullets; add float and catch-label exclusions | — |
| `docs/netcode/GAMEPLAY_COVERAGE_AUDIT.md` | Finding 10 is stale already (it predates the animation work); correct it | — |

`net/`, `server/`, `tests/`, `schemas/` and `PROTOCOL.md`: **untouched.**

Only two decomp translation units change, both additive, neither altering
existing behaviour — which keeps the upstream-merge cost of this commit near
zero, as `src/CLAUDE.md` requires.

## 6. Remote render data additions

Added to `NET_REMOTE_RENDER_DATA` (`ac_net_remote_player.c:34`):

```c
cKF_SkeletonInfo_R_c item_keyframe;
s_xyz  item_joint_data[8];
s_xyz  item_morph_data[8];
Mtx    item_work_mtx[2][4] ATTRIBUTE_ALIGN(32);
MtxF   right_hand_mtx;
mPlib_face_state_c face;
s16    loaded_item_shape;
s16    loaded_item_anim;
s8     loaded_item_kind;
u8     item_skeleton_loaded;
f32    item_scale;
```

About +0x2E0 bytes on a struct already near 5 KB, allocated once per visible
remote through the existing `zelda_malloc_align(sizeof(*render), 32)`. At the
16-player cap that is ~12 KB more — not a budget concern, but it belongs in the
commit message.

## 7. Tests

There is no honest automated coverage for any of this: it is all client
rendering in decompiled translation units that the portable test suite does not
link. Say so rather than inventing a test that proves nothing.

What is worth adding, and what the automated gate actually proves:

- `make check` / `make sanitize` — proves the commit did **not** touch the
  portable core. Both must stay at 39/39. A change here is a red flag that
  something leaked into `net/`.
- The existing production-client loopback case already holds a rod before
  catching and stows it before dropping, and asserts animation and held item
  converge. Extend its assertions to cover `transform.action` surviving the
  round trip, since §2 and §3.4 now depend on that field being live. This is
  the one genuinely new automated assertion in the commit.
- Everything else is manual.

## 8. Verification gate

1. `make check` and `make sanitize` — 39/39 both, unchanged.
2. Windows build from WSL: `cmd.exe /c build_pc.bat` (never invoke `ninja.exe`
   directly — it pops missing-DLL dialogs).
3. Two-client visual pass with a legitimate disc, via the two-client launcher.
   **Restage from `bin/` after the build** or the run tests stale binaries.
   Check, on the *other* client's view: blink; a mouth flap during conversation;
   `gaaan`/`biku` on a bee sting; the axe swing face; the tool being the right
   model at the right angle; the tool vanishing when stowed and when a menu
   opens; a net bag and a rod that render; an umbrella that opens; take-out
   grow instead of pop.
4. `scripts/smoke_windows.ps1` (offline boot) and
   `scripts/smoke_online_windows.ps1`.

Item 3 is the real gate. Nothing in items 1, 2 or 4 can fail on a bug in this
commit, because none of them draw a remote player.

## 9. Risks

| Risk | Assessment |
|-|-|
| Mislabelled `SetupItem_Base2` constants (§3.3) | The main correctness hazard. A wrong ordinal gives a plausible-looking wrong animation, not a crash — so it will not be caught by review, only by item 3 of the gate. Resolve numerically and record the mapping in a comment. |
| Track index out of range | Guarded by clamping in §2.3. Low. |
| Balloon lean derivation (§4.2) | Most likely to look wrong first try. Contained — it affects only balloon holders. |
| Umbrella object bank contention | `aTOL_Clip_c::bank_id` is shared. Multiple remotes with different umbrella kinds is untested territory; verify with two remotes holding different umbrellas. |
| Struct growth on a `-O0` decomp-adjacent TU | Non-issue at these sizes, but the actor is `zelda_malloc_align`'d, so an allocation failure path must stay handled — it already is (`render == NULL` guards). |
| Scope creep into the float | The temptation will be strong once the rod renders and visibly has no line. Resist; §0 explains why. |

## 10. Deferred

- Fishing float and line (needs UKI ownership; see §0).
- Net catch label (encounter-owned).
- Net bag lean (`net_angle`) (§3.2).
- Remote sub-actor sound. Every tool actor above is silent for remotes in this
  commit; audio for remote players is not addressed anywhere yet and should be
  scoped as its own piece.
