# Custom content systems

Which existing systems can carry **custom cutscenes, timed events, calendar days and menus**,
what each one costs, and worked example designs for six representative additions.

This is a design document. **Nothing here is implemented and no code in the tree is changed by
it.** The examples are sketches meant to be read before writing anything, not patches. Companion
to `docs/UI_SYSTEMS.md`, which covers the rendering and menu substrate in detail.

---

## 1. What is reusable, at a glance

| Want to add | Use | Where the change lands | Merge risk vs. ac-decomp | Multiplayer-safe? |
|-|-|-|-|-|
| A calendar day (holiday marking, Tortimer greeting) | `event_schedule_data` + calendar tables | 3 data tables in `src/` | Low (table rows) | Needs server agreement on the date |
| A timed town event with NPCs/props | Event scheduler + `ac_event_manager.c` ctrl table | 1 table row + a new control block | Medium | Server-owned; see §7 |
| A scripted cutscene | `mDemo` request queue + a control actor + `Camera2` | 1 new actor file | Low (new file) | Local presentation only |
| New dialogue anywhere | Synthesised message ids (`MSG_PC_ONLINE_TOWN_TIME` pattern) | `m_msg_main.c_inc` | Low | Local |
| A pocket-style menu page | Submenu overlay | 5 edits + new overlay file | High | Must respect authority matrix |
| A HUD / banner / toast | PC-side module drawing into `font_thaga` | `pc/src/` only | **None** | Local |
| A settings/pause page | `pc_settings_menu.c` / `pc_pause_menu.c` | `pc/src/` only | **None** | Local |

The general rule that falls out of the table: **anything presentational should go in `pc/`**;
only reach into `src/` when you need the original scheduler, the original save fields, or the
original actor lifecycle.

---

## 2. The event scheduler — how a day becomes a live event

Four layers, each a table you can extend.

```
event_schedule_data[]          declarative "when"          m_event_schedule.c_inc
        │  update_schedule_today() once per day/hour
        ▼
event_today[16] + index_today[]  today's live slots        m_event.c:341-362
        │  mEv_check_schedule / mEv_check_status
        ▼
schedule_event[]               per-event start/stop/in/out ac_event_manager.c:4433
        │  today_event[32] filtered by mEv_check_run_today
        ▼
actors, NPCs, props, BGM, weather
```

### 2.1 Layer 1 — the schedule table

`src/game/m_event_schedule.c_inc` is 135 rows of pure data. One row:

```c
{{{month, day, 0x00, start_hour}, {month, day, 0x00, end_hour}}, 0x0000, mEv_EVENT_HALLOWEEN}
   └──── begin date ────┘          └──── end date ────┘                  └── event id ──┘
```

The month/day/hour bytes are **flag-encoded**, which is what makes the table expressive without
code (`include/m_event.h:17-53`):

| Field | Flag | Meaning |
|-|-|-|
| month | `mEv_SCHEDULE_NOW_MONTH` (0x20) | "whatever month it is now" — for weekly/recurring events |
| month | `mEv_SCHEDULE_USE_SAVE_MONTH` / `mEv_SCHEDULE_SAVE_MONTH(n)` | read the date from save slot *n* (per-town randomised dates) |
| month | `mEv_SCHEDULE_LUNAR(m)` (0x40) | resolve against this year's lunisolar harvest-moon date |
| day | `mEv_SCHEDULE_WEEKLY` (0x80) + `mEv_SCHEDULE_MAKE_WEEKLY_DATA(week, weekday)` | "2nd Sunday", "last Friday", "every Saturday" |
| day | `mEv_SCHEDULE_LAST_DAY_OF_MONTH` (0x20) | month-end |
| day | `mEv_SCHEDULE_TOWN_DAY` (0x40) | the town's own founding day from the save |
| day | `mEv_SCHEDULE_DAY_AFTER` (0x40, with `WEEKLY`) | day after the Nth weekday — how Black Friday is expressed |
| hour | `mEv_SCHEDULE_TODAY` (0x80) | active on the day the save is loaded |
| hour | `mEv_SCHEDULE_MULTIDAY` (0x40) | spans a date range rather than one day |
| hour | `mEv_SCHEDULE_HOUR_SLOT(h)` (0x20) | read the start hour from save slot *h* |

Convenience wrappers exist for the common weekly cases: `mEv_SCHEDULE_1ST_WEEKDAY(d)` …
`mEv_SCHEDULE_LAST_WEEKDAY(d)`, `mEv_SCHEDULE_EVERY_WEEKDAY(d)`.

Practical range of what layer 1 alone can express, with **no code**: fixed dates, date ranges,
Nth-weekday-of-month, last-weekday, every-weekday, day-after-Nth-weekday, month-end, lunar dates,
per-town randomised dates, per-player birthday, and multi-day windows with different start/end
hours. That covers nearly every real-world holiday shape.

### 2.2 Layer 2 — today's slots

`update_schedule_today` resolves every row against the current date and installs matches into
`event_today[16]` (`m_event.c:341`), with `index_today[mEv_EVENT_NUM]` mapping event id → slot.
The per-slot record carries a **24-bit `active_hours` bitfield**, the begin/end dates, and a
status byte.

**Hard cap: 16 events may be scheduled on any one day** (`mEv_TODAY_EVENT_NUM`). `add_event_today`
returns `FALSE` and silently drops the event when the table is full (`m_event.c:659-677`). Some
existing days already run close to this — 21 March schedules the equinox, four sports-fair
sub-events, the fair itself, weather forcing, and the rumour, which is 8 slots before anything
custom. **Any new event must budget against this cap on its busiest day.**

Status bits (`include/m_event.h:415-422`) are the vocabulary actors use to talk about an event:

| Bit | Meaning |
|-|-|
| `mEv_STATUS_ACTIVE` | running right now |
| `mEv_STATUS_STOP` | suppressed |
| `mEv_STATUS_SHOW` | presentation should be visible |
| `mEv_STATUS_PLAYSOUND` | sound cue pending |
| `mEv_STATUS_RUN` | should execute |
| `mEv_STATUS_ERROR` | setup failed — queries return FALSE |
| `mEv_STATUS_TALK` | needs a player conversation |
| `mEv_STATUS_EXIST` | scheduled today |

Queries: `mEv_check_schedule(event)` (is it active *this hour*), `mEv_check_run_today(event)` (is
it on today at all), `mEv_check_status(event, bit)`, and the shorthand `mEv_IsEventActive(event)`.

> Note: `mEv_check_status_edge` has a known upstream bug — it uses `|` where `&` was intended, so
> it returns TRUE for any non-zero argument unless `BUGFIXES` is defined (`m_event.c:2245-2255`).
> Do not build new logic on it in a non-`BUGFIXES` configuration.

### 2.3 Layer 3 — the event-manager control table

`schedule_event[]` (`src/actor/ac_event_manager.c:4433-4504`, 71 rows) binds an event id to five
optional callbacks:

```c
{ mEv_EVENT_HALLOWEEN, halloween_start, halloween_stop, halloween_in, wait_culling, halloween_behind, {0,0} }
```

| Callback | Fires when |
|-|-|
| `start_proc` | the event's hour window opens |
| `stop_proc` | the window closes |
| `in_proc` | the player enters the event's acre |
| `out_proc` | the player leaves it (`wait_culling` is the stock "despawn politely" handler) |
| `behind_proc` | per-frame while the event is live but the player is elsewhere |

`set_today_event()` (`:4516`) filters the 71 rows down to today's `today_event[32]` list each day.
All five callbacks may be `NULL` — several rows are pure schedule markers with no behaviour
(`mEv_EVENT_SONCHO_NEW_YEARS_DAY`), which is exactly what a data-only calendar day looks like.

### 2.4 Layer 4 — placement and per-event storage

The event manager provides a library of placement helpers that pick a legal spot and spawn an
actor there (`ac_event_manager.c:1192-1470`):

| Helper | Placement rule |
|-|-|
| `make_actor_in_free_block` | any unoccupied acre |
| `make_actor_in_select_block` | a named acre |
| `make_actor_in_fixed_block` | an exact acre + unit |
| `make_actor_in_seaside_block` | beach row |
| `make_move_actor_in_free_block` | free acre, actor may wander |
| `make_actor_in_free_block_hide` | free acre, starts hidden |
| `make_FG_somewhere_lot4sale` | an empty house lot |

Each returns an `mEv_place_data_c*` reserved through `mEv_reserve_common_place(type, id)`.

Three scratch stores back custom event state without touching `Save_t`'s named fields:

| API | Slots | Lifetime | Use for |
|-|-|-|-|
| `mEv_reserve_save_area(type, id)` | 5 (`mEv_AREA_NUM`) | persists across sessions | event progress that must survive a save |
| `mEv_reserve_common_area(type, id)` | 5 | in-memory, cleared daily | today's working state |
| `mEv_reserve_common_place(type, id)` | 10 (`mEv_PLACE_NUM`) | in-memory | "where did I put the event actor" |

Each area is 11 `int`s (`mEv_area_c::data[11]`, `include/m_event.h:593-596`) — 44 bytes. Slots are
allocated by bitfield and matched on `(type, id)`, so re-reserving in the same session returns the
same block. **These are the intended extension points**: they exist precisely so an event can keep
state without a new save field.

### 2.5 The rumour channel

`mEv_spread_rumor(type)` appends to a per-frame table; `mEv_get_rumor()` returns a rotating pick
(`m_event.c:2569-2580`), which is what makes villagers mention upcoming events in idle chat.
`event_rumor_table[]` (`:348-358`) lists the rumour-carrying event ids — and notably contains
`mEv_EVENT_76` **twice as a placeholder**, alongside the spare `mEv_EVENT_76` enum slot at the end
of `enum event_table`. There is one unused event id already reserved in the enum.

---

## 3. The demo (cutscene) system

`mDemo` is a **priority request queue with a control-transfer protocol**, not a timeline editor.
There is no data-driven cutscene format anywhere in the tree; every original cutscene is a
hand-written actor state machine. `src/game/m_demo.c` (1105 lines) + `include/m_demo.h`.

### 3.1 Request model

```c
mDemo_Request(type, actor, req_proc);   /* queue up to mDemo_REQUEST_NUM (32) */
mDemo_Check(type, actor);               /* am I the one running? */
mDemo_Check_and_Go(type, actor);        /* request + check in one call */
mDemo_End(actor);                       /* hand control back */
```

17 demo types (`enum demo_type`): `SCROLL`, `SCROLL2`, `SCROLL3`, `EXITSCENE`, `DOOR`, `DOOR2`,
`TALK`, `SPEAK`, `REPORT`, `SPEECH`, `OUTDOOR`, `EVENTMSG`, `EVENTMSG2`, and four unnamed. Talk
types carry a `mDemo_talk_data_c` (message number, window colour, whether to show the speaker's
name, whether to zoom, what to return to); event-message types carry `mDemo_emsg_data_c` with
message + delay timers + door data.

`mDemo_ORDER_*` is a 10 × 10 `u16` scratch grid (`mDemo_Set_OrderValue` / `mDemo_Get_OrderValue`)
addressed by role — `PLAYER`, `NPC0`, `NPC1`, `NPC2`, `QUEST`. This is how the message control
codes `mFont_CONT_CODE_SET_DEMO_ORDER_*` drive actor behaviour **from inside the dialogue text**:
the script writes an order value, the actor reads it next frame. It is the closest thing the
original engine has to a cutscene scripting language, and it is fully reusable.

### 3.2 Camera

`Camera2` exposes a request API at priority `mDemo_CAMERA_PRIORITY` (6)
(`include/m_camera2.h:339-373`):

| Request | Shot |
|-|-|
| `Camera2_request_main_demo` | full explicit start/goal framing |
| `Camera2_request_main_demo_fromNowPos` | move from the current camera to a target + direction |
| `Camera2_request_main_demo_fromNowPos2` | move to a target with a distance delta |
| `Camera2_request_main_lock` | fixed centre + eye + FOV, with a morph counter |
| `Camera2_request_main_simple2` | centre + direction + distance |
| `Camera2_request_main_talk` / `_cust_talk` / `_listen_front_low_talk` | conversation framings |
| `Camera2_request_main_normal` | release back to gameplay |
| `mDemo_KeepCamera(camera_type)` | hold a shot across a demo state change (`m_demo.c`) |
| `Camera2_change_priority` | let a higher-priority request take over mid-shot |

### 3.3 Player puppeting

`mPlib_request_main_demo_*` (`include/m_player_lib.h:114-134`) drives the player through the same
state machine gameplay uses — `walk`, `wait`, `geton_train`, `getoff_train`, `geton_boat`,
`getoff_boat_standup`, `standing_train`, plus the golden-item presentation states. Combined with
`mPlib_check_label_player_demo_wait(game, label)`, an actor can wait on a *named* player state,
which is how the intro sequences stay in step without frame counting.

### 3.4 Transitions

`GAME_PLAY` owns both a colour fade and a wipe (`include/m_play.h:26-62`):

- `fb_fade_type` — 13 `FADE_TYPE_*` values including a dedicated `FADE_TYPE_DEMO` and
  `FADE_TYPE_EVENT`.
- `fb_wipe_mode` — `WIPE_MODE_CREATE / INIT / MOVE`, drawn by `play->fbdemo_wipe.wipe_procs.draw_proc`.

Both are composited in `makeBumpTexture` (`m_play.c:696-732`) into the `overlay` bucket, so they
correctly cover dialogue and menus. Both are already tagged for widescreen stretch on PC.

### 3.5 Worked reference: the intro demo

`src/actor/ac_intro_demo.c` + `ac_intro_demo_move.c_inc` (517 lines total) is the cleanest model
to copy. Its shape:

1. A control actor with `ACTOR_STATE_CAN_MOVE_IN_DEMO_SCENES`, profile `mAc_PROFILE_INTRO_DEMO`.
2. `ct` registers itself in `Common_Get(clip).demo_clip` with a `mDemo_CLIP_TYPE_*` tag, so other
   actors can find it (`ac_intro_demo.c:62-66`).
3. A 13-value action enum, two parallel proc tables — one `init_proc[]` run once on entry, one
   `process[]` run per frame — and `aID_setupAction(actor, play, act)` as the only transition
   primitive (`ac_intro_demo_move.c_inc:363-410`).
4. `move` is one line: `(*intro_demo->action_proc)(intro_demo, play)`.

The `Clip_c` struct (`include/m_clip.h:50-109`) has **two demo clip pointers** (`demo_clip`,
`demo_clip2`) plus several `void*` spares, so a custom cutscene controller has a discovery slot
available without extending the struct.

---

## 4. Dialogue as a scripting layer

Covered in depth in `docs/UI_SYSTEMS.md` §3.3 and §3.5. The two facts that matter for custom
content:

- The **control-code VM** already contains the opcodes a cutscene needs: actor-animation orders
  (`SET_DEMO_ORDER_*`), branch-to-next-message (`SET_NEXT_MESSAGE_*`, including three random
  variants), choice windows (`SET_SELECT_STRING_2..6`), colour/scale/voice, BGM start/stop, SE
  triggers, and 20 free-string substitution slots.
- **Synthesised messages** need no disc data. Reserve an id at or past `MSG_MAX` (`0x3F91`), widen
  `mMsg_ChangeMsgData`'s validity check, and emit the byte stream at load time — the online
  town-time message (`m_msg_main.c_inc:337-370`) is a complete 30-line worked example.

Together these mean **a custom cutscene's script can live in a C string table**, with branching
and choices, without touching the ROM message archive.

---

## 5. Menus

See `docs/UI_SYSTEMS.md` §5 and §10.4 for the full submenu-overlay recipe and §7 for the PC menu
stack. The decision that matters here:

| If the menu is… | Build it as | Why |
|-|-|-|
| Part of the pocket/tab flow, uses item icons, needs the hand cursor | Submenu overlay | Reuses `m_tag_ovl` selection + `mSM_draw_item` |
| An event/cutscene prompt with 2–6 options | `mChoice` via a dialogue control code | Zero new code; already animated and sound-driven |
| A standalone page (event log, almanac, town board) | PC module in `font_thaga` via `pc_text_draw` | No decomp edit, no merge cost, no 16-slot budget |
| A setting or toggle | `pc_settings_menu.c` item | Persisted to `settings.ini` for free |

One non-obvious capability: an overlay can be opened with `submenu->mode = mSM_MODE_OTHER`, which
skips the prerender backdrop capture and the player-voice spec change (`m_submenu.c:371-380`,
`:399-415`). That is the mode a cutscene would use to show a menu **over a live scene** rather
than over a frozen snapshot.

---

## 6. Calendar days

Marking a day on the in-game calendar is **separate from scheduling an event on it**. Three
independent tables must agree:

| Table | File | Purpose |
|-|-|-|
| `event_schedule_data[]` | `m_event_schedule.c_inc` | makes the event actually happen |
| `mCD_make_calendar_data_fixed_day_event`'s `event_table[]` | `m_calendar_ovl.c:304-322` | draws a marker on a fixed date |
| `mCD_make_calendar_data_unfixed_day_event`'s `event_table[]` | `m_calendar_ovl.c:341-350` | draws a marker on an Nth-weekday date |
| `chg_string_idx[]` in `mSC_get_event_name_str` | `m_soncho.c:296+` | maps the calendar event id to a name string |

Note the second table encodes the *same* recurrence logic in a different format
(`{month, week, weekday, days_after, event}`) than the scheduler's bit-packed day byte. They are
resolved independently, so a mismatch shows a marker on a day where nothing happens (or the
reverse). Computed dates — equinoxes (`lbRk_VernalEquinoxDay`), harvest moon
(`lbRk_HarvestMoonDay`), town day, player birthdays — are patched in afterwards
(`m_calendar_ovl.c:385-400`).

Per-player participation is tracked in `Private_c::calendar`: `event_days[12]` is a
day-of-month bitfield per month, plus `event_flags` for the special-attendance cases
(`m_calendar_ovl.c:33-101`). That is where "did this player attend" lives, and it is
per-resident, not per-town.

---

## 7. The multiplayer dimension

`docs/netcode/AUTHORITY_MATRIX.md` is the reference; the short version for custom content:

**The server already owns the clock, the day boundary, and the weather.** `TownClock`
(`server/src/town_clock.cpp`) advances town time, rolls daily weather, and the client overwrites
its own RTC from it every frame in `Net_ApplyAuthoritativeClock` (`src/game/m_net_hooks.c:1079`).
So an event scheduled off `Common_Get(time.rtc_time)` **automatically fires on the same town day
for every connected player** — no extra work. That is a significant free win.

**The server has a general-purpose scheduler.** `TownClock::add_job(ScheduledJob, callback)`
(`server/include/acserver/town_clock.hpp:52-68`) takes a name, an interval, a next-due time and a
catch-up cap; `town_runtime.cpp:573-604` registers two today: `npc-schedules` (hourly) and
`daily-renewal` (daily shop restock + Sunday turnip roll). Adding a third named job is the
supported way to give a custom event server-side authority. Jobs run on catch-up after downtime,
bounded by `maximum_catchups`.

**What a custom event must not do online:** commit inventory, bells, tiles, housing or mail
locally. The pattern to copy is `m_bank_ovl.c:60-67` — branch on an `Net_*Authoritative()` query,
send a `Net_Request*` and let the authoritative-state hook apply the result, rather than writing
the save.

**What is safe to do purely locally:** camera, cutscene staging, dialogue, particles, BGM, HUD.
Two players watching the same fireworks show do not need frame-synchronised rockets.

---

## 8. Worked examples

Six sketches, ordered from cheapest to most involved. None of these exist; each names the exact
files it would touch.

---

### Example A — A new calendar day, data only

**Goal:** "Lantern Night", 7 October, 18:00–22:00. Tortimer greets the player, the day is marked
on the calendar, villagers gossip about it for a week beforehand.

**Touches:** 4 table rows + 1 string. No new C functions.

**1. Reserve two event ids.** `enum event_table` (`include/m_event.h`) already carries an unused
`mEv_EVENT_76`; add after it:

```c
    mEv_EVENT_LANTERN_NIGHT,
    mEv_EVENT_SONCHO_LANTERN_NIGHT,
    mEv_EVENT_RUMOR_LANTERN_NIGHT,
```

`mEv_EVENT_NUM` grows automatically and `index_today[]` sizes off it.

**2. Schedule rows** in `m_event_schedule.c_inc` — three lines, following the Halloween/Explorer's
Day rows verbatim:

```c
{{{lbRTC_OCTOBER, 7, 0x00, 18}, {lbRTC_OCTOBER, 7, 0x00, 22}}, 0x0000, mEv_EVENT_LANTERN_NIGHT},
{{{lbRTC_OCTOBER, 7, 0x00, 18}, {lbRTC_OCTOBER, 7, 0x00, 22}}, 0x0000, mEv_EVENT_SONCHO_LANTERN_NIGHT},
{{{lbRTC_OCTOBER, 1, 0x00, mEv_SCHEDULE_MULTIDAY | 0}, {lbRTC_OCTOBER, 6, 0x00, 23}}, 0x0000, mEv_EVENT_RUMOR_LANTERN_NIGHT},
```

The third row is the rumour window: `MULTIDAY` + a 1–6 October range means villagers mention it
for six days.

**3. Behaviour row** in `schedule_event[]` (`ac_event_manager.c`). Tortimer's stock handlers do
the whole job — he appears in the plaza, greets, and despawns:

```c
{ mEv_EVENT_SONCHO_LANTERN_NIGHT, soncho_start, soncho_stop, soncho_in, wait_culling, NULL, {0, 0} },
```

The main `mEv_EVENT_LANTERN_NIGHT` row can be all-`NULL` (like
`mEv_EVENT_SONCHO_NEW_YEARS_DAY`) if the day only needs to *exist* for other systems to query.

**4. Rumour registration:** add `mEv_EVENT_RUMOR_LANTERN_NIGHT` to `event_rumor_table[]`
(`m_event.c:348`) — or replace one of the two `mEv_EVENT_76` placeholders already sitting in it.

**5. Calendar marking:** one row in `m_calendar_ovl.c:304`'s `event_table[]`:

```c
{ lbRTC_OCTOBER, 7, mSC_EVENT_LANTERN_NIGHT },
```

plus an `mSC_EVENT_LANTERN_NIGHT` enum value and its string index in `chg_string_idx[]`
(`m_soncho.c:296`). The name string itself needs a `RESOURCE_STRING` entry that does not exist on
the disc — **this is the one place Example A hits a wall.** Two ways out:

- Reuse an existing string id whose text fits.
- Add a small PC-side override table consulted by `mSC_get_event_name_str` before the ROM lookup,
  keyed on event id — 20 lines in `src/game/m_soncho.c` under `#ifdef PC_ENHANCEMENTS`, and it
  then serves every future custom day.

**Budget check:** 7 October currently schedules nothing, so the three new rows are well inside the
16-slot daily cap.

---

### Example B — A scripted cutscene

**Goal:** on first arrival at the beach after Lantern Night starts, a 20-second sequence: fade,
camera pans to the sea, the player walks two units forward, Tortimer speaks three pages, camera
returns, fade in.

**Touches:** one new actor file. Nothing existing is edited except the actor profile table.

**Shape** — copy `ac_intro_demo.c` structurally:

```c
/* src/actor/ac_lantern_demo.c  (sketch) */

enum {
    aLD_ACT_WAIT_TRIGGER,
    aLD_ACT_FADE_OUT,
    aLD_ACT_CAMERA_TO_SEA,
    aLD_ACT_PLAYER_WALK,
    aLD_ACT_TORTIMER_TALK,
    aLD_ACT_CAMERA_RETURN,
    aLD_ACT_DONE,
    aLD_ACT_NUM
};

ACTOR_PROFILE Lantern_Demo_Profile = {
    mAc_PROFILE_LANTERN_DEMO,
    ACTOR_PART_CONTROL,
    ACTOR_STATE_CAN_MOVE_IN_DEMO_SCENES | ACTOR_STATE_NO_MOVE_WHILE_CULLED,
    EMPTY_NO, ACTOR_OBJ_BANK_KEEP, sizeof(LANTERN_DEMO_ACTOR),
    &aLD_ct, &aLD_dt, &aLD_move, mActor_NONE_PROC1, NULL,
};
```

Per-action bodies, in prose:

| Action | Entry (`init_proc`) | Per frame (`process`) |
|-|-|-|
| `WAIT_TRIGGER` | — | `if (mEv_check_schedule(mEv_EVENT_LANTERN_NIGHT) && player is in the beach acre && !already_played) → setupAction(FADE_OUT)` |
| `FADE_OUT` | `play->fb_fade_type = FADE_TYPE_DEMO` | wait for the fade to complete, then `setupAction(CAMERA_TO_SEA)` |
| `CAMERA_TO_SEA` | `mDemo_Check_and_Go(mDemo_TYPE_SCROLL, actor)`; `Camera2_request_main_demo_fromNowPos2(play, &sea_pos, 0.0f, 0.0f, 0.0f, mDemo_CAMERA_PRIORITY)` | count down a morph timer |
| `PLAYER_WALK` | `mPlib_request_main_demo_walk_type1(game, goal_x, goal_z, speed, TRUE)` | `mPlib_check_label_player_demo_wait(game, label)` → next |
| `TORTIMER_TALK` | `mDemo_Set_msg_num(MSG_PC_LANTERN_NIGHT)`; `mDemo_Set_talk_display_name(TRUE)`; `mDemo_Check_and_Go(mDemo_TYPE_SPEECH, actor)` | `mMsg_CHECK_MAINHIDE()` → next |
| `CAMERA_RETURN` | `Camera2_request_main_normal(play, 0, mDemo_CAMERA_PRIORITY)`; `play->fb_fade_type = FADE_TYPE_IN` | fade complete → next |
| `DONE` | `mDemo_End(actor)`; set the "already played" bit | idle |

**Two supporting pieces:**

- *Dialogue:* `MSG_PC_LANTERN_NIGHT` is a synthesised message (§4) built with
  `mMsg_AppendOnlineText` / `mMsg_AppendOnlineControl`-style helpers. Page breaks are
  `mFont_CONT_CODE_CONTINUE`; a mid-scene choice is `mFont_CONT_CODE_SET_SELECT_STRING_2`.
- *"Already played" bit:* `mEv_reserve_save_area(mEv_EVENT_LANTERN_NIGHT, id)` gives 44
  persistent bytes — no new `Save_t` field. Byte 0 as a played flag, bytes 4–7 as the year it last
  played so it re-arms next October.

**Multiplayer:** entirely local. Each client plays its own copy when its own player reaches the
beach. Nothing is replicated and nothing needs to be.

**Registration:** add the profile to `src/game/m_actor_dlftbls.c` and spawn it from the event's
`start_proc` (Example C) or from the scene's actor list.

---

### Example C — A recurring weekly visitor

**Goal:** a merchant NPC who appears in a random free acre every Wednesday 09:00–17:00, with a
shop menu, and remembers what the player bought.

**Touches:** 1 schedule row, 1 ctrl row, ~150 lines of new control code in the event-manager
style, and either a submenu overlay or a dialogue-driven purchase flow.

**1. Schedule** — one row, recurring forever:

```c
{{{mEv_SCHEDULE_NOW_MONTH, mEv_SCHEDULE_EVERY_WEEKDAY(lbRTC_WEDNESDAY), 0x00, 9},
  {mEv_SCHEDULE_NOW_MONTH, mEv_SCHEDULE_EVERY_WEEKDAY(lbRTC_WEDNESDAY), 0x00, 17}},
 0x0000, mEv_EVENT_TINKER},
```

`mEv_SCHEDULE_NOW_MONTH` + `EVERY_WEEKDAY` is exactly how the turnip seller and K.K. Slider are
expressed (`m_event_schedule.c_inc:12-13`).

**2. Control block**, modelled on `turnipbuyer_*` / `gypsy_*`:

```c
static int tinker_start(EVENT_MANAGER_ACTOR* evmgr, aEvMgr_event_ctrl_c* ctrl) {
    mEv_place_data_c* place =
        make_actor_in_free_block(evmgr, ctrl, mAc_TINKER, /*id=*/0, /*adjust=*/TRUE);
    if (place == NULL) { mEv_set_status(ctrl->type, mEv_STATUS_ERROR); return FALSE; }
    ctrl->block = place->block;
    return TRUE;
}
static int tinker_stop(...)   { /* despawn, mEv_clear_common_place */ }
static int tinker_in(...)     { /* player entered the acre: ensure the actor exists */ }
/* out_proc = wait_culling; behind_proc = NULL */
```

Ctrl row:

```c
{ mEv_EVENT_TINKER, tinker_start, tinker_stop, tinker_in, wait_culling, NULL, {0, 0} },
```

**3. Purchase memory:** `mEv_reserve_save_area(mEv_EVENT_TINKER, player_slot)` — 11 ints, enough
for a bought-items bitfield plus a last-visit date.

**4. Shop UI:** two options.

- *Cheap:* dialogue + `mChoice`. A synthesised message with
  `mFont_CONT_CODE_SET_SELECT_STRING_4` gives a 4-option purchase menu with zero new UI code.
- *Rich:* a new submenu overlay following `docs/UI_SYSTEMS.md` §10.4, opened with
  `mSM_open_submenu_new2(submenu, mSM_OVL_TINKER, ...)`.

**5. Multiplayer — this is the one that needs server work.** The visitor's *presence* is derived
from the authoritative clock, so all clients agree he is there. But his **stock and the purchase
transaction are contested state**. Following the existing shop pattern:

- Server: a named `ScheduledJob` (`interval_seconds = 7 * 86400`, next due Wednesday 09:00) rolls
  the week's stock into a new resource kind and publishes a `ReplicationDelta`, exactly as
  `daily-renewal` does for the shop (`town_runtime.cpp:600-640`).
- Client: `Net_TinkerStockAuthoritative()` / `Net_RequestBuyFromTinker(item)` hooks alongside the
  existing `Net_RequestBuyItem`, with the overlay branching on the authority query.
- Offline, the same code rolls stock locally from the RTC — the same dual path the shop already
  has.

---

### Example D — An event banner (PC-side, zero decomp cost)

**Goal:** when an event starts, a banner slides in at the top of the screen for four seconds:
"Lantern Night — 6PM to 10PM at the beach". Fades out. Never blocks input.

**Touches:** one new file in `pc/src/`, one call site.

```c
/* pc/src/pc_event_banner.c  (sketch) */

static const char* s_text;
static float s_timer;      /* seconds remaining */

void pc_event_banner_show(const char* text, float seconds);   /* called by game code */

void pc_event_banner_draw(struct game_s* game) {
    if (s_timer <= 0.0f || !s_text) return;
    s_timer -= (float)game->graph->dt_num_60fps_frames / 60.0f;

    /* Slide + fade envelope: 0.3s in, hold, 0.5s out. */
    float alpha = ...;                 /* 0..255 from s_timer */
    float y     = -18.0f + 26.0f * ease_in;

    mFont_SetMatrix(game->graph, mFont_MODE_FONT);
    pc_menu_draw_centered(game, s_text, y, 255, 245, 200, (int)alpha, 1.0f);
    mFont_UnSetMatrix(game->graph, mFont_MODE_FONT);
}
```

**Call site:** one line in `src/graph.c` next to the existing `pc_pause_menu_draw(game)`
(`src/graph.c:390`) — so the banner composites above all game UI and below fades, with no
ordering surprises.

**Trigger:** a helper in `m_net_hooks.c` or the event manager's `start_proc` calls
`pc_event_banner_show(...)`. Guard the declaration with a no-op macro fallback so a
`PC_ENHANCEMENTS`-off build compiles unchanged, matching the `m_net_hooks.h` convention.

**Why this is the cheapest option:** no display-list budget concern (one short string is ~7 draw
calls via `pc_text_draw`), no `event_today` slot, no save field, no merge conflict, and it works
identically online and offline.

---

### Example E — An event log / almanac page

**Goal:** a scrollable page listing this month's events, which ones the player attended, and the
next upcoming one. Reachable from the pause menu.

**Recommendation: build it as a PC menu page, not a submenu overlay.**

The submenu route costs 5 coordinated edits across positional tables plus a new 400+ line overlay
file in `-O0` decomp code, and buys you: the slide-in animation, the hand cursor, item icons. An
almanac needs none of those.

```
pc_pause_menu.c              pc_almanac_page.c  (new)
  PAGE_MAIN                    - queries mEv_check_run_today / mEv_check_schedule
  PAGE_SETTINGS                  over the event id range
  PAGE_ALMANAC   ────────────►   - reads Private_c::calendar.event_days for attendance
  PAGE_CONFIRM_QUIT              - reads lbRk_* for computed dates
                                 - renders rows with pc_menu_draw_left + pc_text_draw
                                 - scroll window of ~10 rows, same latched-stick nav
                                   as pc_settings_menu
```

Data it can read with no new plumbing:

| Row content | Source |
|-|-|
| Is the event on today | `mEv_check_run_today(id)` |
| Is it live right now | `mEv_check_schedule(id)` |
| Its hours | `mEv_get_end_time(id)` + the `active_hours` bitfield |
| Its name | `mSC_get_event_name_str` (plus the PC override from Example A) |
| Did this player attend | `Private_c::calendar.event_days[month-1] & (1 << (day-1))` |
| Computed dates | `lbRk_VernalEquinoxDay`, `lbRk_AutumnalEquinoxDay`, `lbRk_HarvestMoonDay` |

Total new code: one PC file plus three lines in `pc_pause_menu.c`. **No file under `src/` changes**
— everything above is a read of an existing extern.

---

### Example F — A town-wide server-authoritative event

**Goal:** a "Meteor Wish" event — on a server-chosen night each month, every connected player sees
the same meteor shower, and the first player to wish gets a unique item. Presentation is local;
the item award is contested.

This is the only example that spans all three layers, and it shows where the split falls.

**Server (`server/`)** owns *when* and *who won*:

```
town_runtime.cpp
  ├─ ScheduledJob "meteor-wish"
  │     interval_seconds = 30 * 86400 (or recomputed per month)
  │     callback: pick the night, set MeteorState{ active, night_unix, winner_account }
  │               publish a ReplicationDelta (new ResourceKind::Meteor)
  │               commit_state(record type ≥ 100) so it survives restart
  └─ request handler for WishRequest
        validate: event active, player in an outdoor zone, no winner yet,
                  idempotency key unseen, observed revision matches
        commit:   winner_account = requester; award item through the existing
                  inventory transaction path; bump revision; publish delta
```

The validation list is not optional — it is the same identity/zone/revision/idempotency check
every persistent request in `net/` performs (`CLAUDE.md` → Authority model).

**Protocol (`net/`)**: one new `ResourceKind`, one encode/decode pair in `replication.cpp`
mirroring `encode_shop_delta`, one request message, and C-API accessors:
`acnet_client_meteor_active()`, `acnet_client_meteor_winner()`, `acnet_client_request_wish()`.

**Client (`src/game/m_net_hooks.c`)**: thin wrappers `Net_MeteorActive()`,
`Net_MeteorAuthoritative()`, `Net_RequestWish()`, each with a no-op macro fallback.

**Presentation (local, unreplicated)**: an event-manager ctrl row whose `start_proc` fires when
either the local schedule (offline) *or* `Net_MeteorActive()` (online) says so, spawning the
existing meteor effect actors. Nobody replicates individual meteors.

**Offline parity:** the same ctrl row runs off `event_schedule_data` when `Net_IsConnected()` is
false, awarding the item locally. This dual path is the established pattern — the bank, the shop
and the turnip market all do exactly this.

---

## 9. Constraints specific to this area

| # | Constraint | Consequence | Reference |
|-|-|-|-|
| 1 | **16 events per day**, silently dropped when full | Budget against the busiest day; 21 March already uses ~8 | `m_event.c:659-677` |
| 2 | 5 save areas / 5 common areas / 10 common places, 44 bytes each | Custom events share a small pool with stock ones | `include/m_event.h:598-611` |
| 3 | Event ids are a **positional enum**; `index_today[]` is sized off `mEv_EVENT_NUM` | Insert at the end, never in the middle | `include/m_event.h:250-383` |
| 4 | Overlay ids, the slide-in table and `mSM_program_dlftbl[]` are **three parallel positional tables** | An overlay added out of order animates as the wrong menu | `docs/UI_SYSTEMS.md` §10.4 |
| 5 | Calendar markers and the scheduler encode recurrence **in two different formats** | A new day must be entered twice and kept in sync | §6 |
| 6 | Custom event *names* need a `RESOURCE_STRING` entry that does not exist on the disc | Needs a PC-side string override table | Example A step 5 |
| 7 | `mEv_check_status_edge` is buggy without `BUGFIXES` (`|` for `&`) | Do not build edge-triggered logic on it | `m_event.c:2245-2255` |
| 8 | Cutscene actors must set `ACTOR_STATE_CAN_MOVE_IN_DEMO_SCENES` | Otherwise the controller is frozen by its own cutscene | `ac_intro_demo.c:42` |
| 9 | `mDemo` request queue is 32 deep, single active demo | No nested/parallel cutscenes | `mDemo_REQUEST_NUM` |
| 10 | Every `src/` edit is a future ac-decomp merge conflict | Prefer `pc/` for presentation, data rows over new functions | `src/CLAUDE.md` |
| 11 | Online, the clock is the server's | Local RTC writes are overwritten every frame | `m_net_hooks.c:1079` |
| 12 | Anything persistent must go through a `Net_Request*` when connected | Local commits are silently overwritten | `docs/netcode/AUTHORITY_MATRIX.md` |

---

## 10. Recommended layering

For most custom content, work outward from the cheapest layer that can express the idea:

1. **Data rows only** (`event_schedule_data`, calendar tables, ctrl row reusing stock handlers) —
   a calendar day with a Tortimer greeting costs four lines.
2. **Data rows + a control block** in the event-manager style — a recurring visitor, a seasonal
   prop, a timed spawn.
3. **A new control actor** using `mDemo` + `Camera2` + `mPlib` — a scripted cutscene, with its
   script as a synthesised message.
4. **A PC-side module** for anything the player only looks at — banners, logs, almanacs, HUD.
   No decomp edit, no merge cost.
5. **A server job + protocol resource** only when players must agree on the outcome.

The trap to avoid is reaching for layer 3 or a submenu overlay when layer 1 or 4 would do. The
scheduler is far more expressive than it looks, and the PC-side text stack has no budget or merge
cost at all.
