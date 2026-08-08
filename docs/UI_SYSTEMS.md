# UI systems

How every on-screen 2D element in this build is produced — the shared rendering substrate, the
original game's four UI stacks, and the PC-native UI this fork added on top. Written so that a
future change (a new menu, a new setting, restyled text, resolution independence, online
nameplates) can be scoped without re-reading 60 000 lines of overlay code.

Sections 1–8 are the analysis. Section 9 is the constraint list, section 10 is a set of
change recipes, section 11 is the ranked gap list.

Nothing here proposes work that has been started. It documents what exists.

---

## 1. The five UI stacks

| Stack | Lives in | Draws into | Owns input | Notes |
|-|-|-|-|-|
| Dialogue window (`mMsg`) + choice box (`mChoice`) | `src/game/m_msg*.c*`, `m_choice*.c*` | `font_thaga` | A/B while open | ROM message data, control-code VM |
| Submenu overlays (30 of them) | `src/game/m_*_ovl.c` under `m_submenu.c` | `polygon_opaque_thaga` | full pad grab | Inventory, map, catalog, bank, design… |
| Full-screen game states | `src/game/m_select.c`, `src/player_select.c`, `src/save_menu.c`, `m_trademark.c`, `ac_animal_logo.c` | mixed | full pad grab | Each is a `DLFTBL_GAME` entry |
| Debug overlay | `m_debug_display.c`, `libu64/gfxprint` | `polygon_opaque_thaga` | `zurumode` gated | Japanese debug font, not shipped-facing |
| PC-native UI (this fork) | `pc/src/pc_pause_menu.c`, `pc_settings_menu.c`, `pc_menu_util.c`, `pc_text_draw.c` | `font_thaga` | SDL events, pre-game | Pause, settings, keybinding capture |

They share exactly two things: the **display-list bucket system** (§2) and the **12×16 font
atlas** (§3). Everything else — layout convention, input model, animation, state machine — is
per-stack and does not compose.

---

## 2. Rendering substrate

### 2.1 Display-list buckets

Nine arenas are carved out of one static `dynamic_t` every frame (`include/sys_dynamic.h:38-53`,
constructed in `src/graph.c:57-65`):

| Bucket | Macro | Size (Gfx) | Bytes | Used by UI for |
|-|-|-|-|-|
| `poly_opa` | `NOW_POLY_OPA_DISP` | 9952 | 79 616 | **all submenu overlays**, debug display |
| `poly_xlu` | `NOW_POLY_XLU_DISP` | 2048 | 16 384 | — |
| `overlay` | `NOW_OVERLAY_DISP` | 1024 | 8 192 | fades/wipes (drawn last) |
| `work` | `NOW_WORK_DISP` | 128 | 1 024 | frame entry point |
| `font` | `NOW_FONT_DISP` | 1792 | 14 336 | **mMsg, mChoice, all PC menus** |
| `shadow` | `NOW_SHADOW_DISP` | 512 | 4 096 | — |
| `light` | `NOW_LIGHT_DISP` | 256 | 2 048 | EFB→texture copy |
| `new0` (bg opa) | `NOW_BG_OPA_DISP` | 512 | 4 096 | — |
| `new1` (bg xlu) | `NOW_BG_XLU_DISP` | 256 | 2 048 | — |

`graph_draw_finish` (`src/graph.c:223-235`) chains them with `gSPBranchList`, which fixes the
**execution order**:

```
work → bg_opa → shadow → bg_xlu → poly_opa → poly_xlu → light → font → overlay → end
```

Consequences that matter for any UI change:

- **`font` draws on top of `poly_opa`.** A dialogue box or PC menu is always above a submenu
  overlay, regardless of the order the code ran in.
- **`overlay` draws on top of `font`.** Screen fades and scene wipes cover the pause menu and
  dialogue. `game_draw_last` (`src/game.c:100-116`) appends a `poly_opa` sub-list into `overlay`,
  which is how `makeBumpTexture` gets fades above everything (`src/game/m_play.c:700-731`).
- Overflow of any arena sets `err` in `graph_draw_finish` (`src/graph.c:261-292`) and the frame is
  dropped. There is no growth path; the sizes are compile-time.

### 2.2 The one allocator trap

`GRAPH_ALLOC` (`include/graph.h:289-292`) **always allocates from the tail of `poly_opa`**, no
matter which bucket the caller is writing commands into:

```c
#define GRAPH_ALLOC(graph, size) \
    ((void*)((graph)->polygon_opaque_thaga.tha.tail_p = \
                 (char*)((intptr_t)(graph)->polygon_opaque_thaga.tha.tail_p - (intptr_t)(size))))
```

Every `Vtx`, `Mtx` and `Vp` used by a font string, a choice window, or a PC menu comes out of the
**79 KB `poly_opa` arena**, while the commands go into the 14 KB `font` arena. Text-heavy UI is
therefore bounded by `poly_opa`'s tail, not by `FONT_SIZE`. This is the single most common
surprise when adding UI.

### 2.3 The 2D coordinate space

There is one UI projection, computed once and cached
(`mFont_CulcOrthoMatrix`, `src/game/m_font_main.c_inc:607-615`):

```c
guOrtho(m, -160*16, +160*16, -120*16, +120*16, -800, 800, 1.0f);
```

So the interface layer is a **320 × 240 space, centre-origin, Y up, scaled by 16** (fixed-point
`mFont_SCALE_F`). Every UI position in the codebase is a float in 0…320 × 0…240 with Y **down**;
the conversion to centred/Y-up happens per primitive:

```c
pos_x = x - 160.0f;      /* mFont_SetVertexRectangle, m_font_main.c_inc:385-386 */
pos_y = -y + 120.0f;
```

`mFont_SetMatrix(graph, mode)` (`m_font_main.c_inc:617-641`) loads that projection plus an identity
modelview into either `font` (mode 1) or `poly_opa` (mode 0). `mFont_UnSetMatrix` only pops the
software matrix stack — it does **not** restore the previous projection, so anything drawn after a
UI block in the same bucket inherits the ortho matrix. `pc_pause_menu_draw` re-loads it every frame
for exactly this reason (`pc/src/pc_pause_menu.c:242`).

Submenu overlays get the same ortho matrix through
`mSM_setup_view` (`src/game/m_submenu_ovl.c:56-99`), which caches it in
`Submenu_Overlay_c::projection_matrix`, and can swap to a *perspective* view via
`mSM_change_view` (`m_submenu_ovl.c:101-174`) for the 3D bits (rotating item models, the player
mannequin, the hand cursor).

### 2.4 Widescreen: a three-state flag

The GC frame is 640 × 480 4:3; PC windows are not. `GXSetProjection` (`pc/src/pc_gx.c:1204-1253`)
multiplies `projection[0][0]` by `g_aspect_factor` depending on `g_pc_widescreen_stretch`:

| Value | Name | Perspective | Orthographic | Set by |
|-|-|-|-|-|
| 0 | hor+ (default) | corrected | corrected | frame reset, `pc_gx.c:388` |
| 1 | stretch | uncorrected | uncorrected | `PC_NOOP_WIDESCREEN_STRETCH` |
| 2 | UI / pillarbox | uncorrected | corrected | `PC_NOOP_WIDESCREEN_STRETCH_OFF` |

The mechanism is a tagged `gDPNoOpTag` intercepted in `emu64::dl_G_NOOP`
(`src/static/libforest/emu64/emu64.c:4551-4562`). Tags are defined at
`pc/include/pc_platform.h:97-98`. Current emitters:

- `src/game/m_play.c:706` — transitions/wipes/fades fill the whole screen (stretch).
- `src/game/m_play.c:754` / `:768` — prerendered submenu backdrop stretches, then flips to mode 2
  so the overlay UI on top of it stays 4:3.
- `pc/src/pc_menu_util.c:13` / `:25` — the PC dim rectangle covers the full window.

**A new full-screen UI element must bracket itself with these tags**, or it will be pillarboxed
while the game behind it is not.

---

## 3. Text

### 3.1 The atlas

One 4-bit intensity atlas, 16 × 16 glyph grid, 12 × 16 pixels per glyph
(`include/m_font.h:518-522`):

| Property | Value | Where |
|-|-|-|
| Symbol | `FONT_nes_tex_font1` | `src/data/font/FONT_nes_tex_font1.c:8` |
| Size | `0x6000` bytes (192 × 256, I4) | same |
| Disc offset | `0x4E1A40` | `pc/src/pc_assets.c:14600` |
| Cell | 12 × 16 | `mFont_TEX_CHAR_WIDTH/HEIGHT` |
| Codepoints | 256, custom encoding | `include/m_font.h:14-273` |

The encoding is **not ASCII**. It is ASCII-ish in the 32–126 range and diverges everywhere else:
`127` is the control-code escape, `128` the message tag, `205` is newline, `176`–`212` are
symbols (hearts, weather, animal faces, currency). `include/m_font.h` is the authoritative table.

Per-glyph advance is `12 - offset[c]` where the offset table is a hand-tuned 256-byte kerning
array (`src/game/m_font_offset.c_inc:2-19`), used only when the caller passes `cut = TRUE`.

Five extra 16 × 16 mark textures exist for arrows/cursors
(`src/game/m_font_mark.c_inc:1-15`): `jyouge`, `sayuu`, `cursor`, `next`, `choice`.

### 3.2 Two draw paths

| | Rect path | Poly path |
|-|-|-|
| Function | `mFont_gppDrawCharRect` (`m_font_main.c_inc:655`) | `mFont_gppDrawCharPoly` (`m_font_main.c_inc:679`) |
| Primitive | `gSPTextureRectangle` | 4 `Vtx` + 2 triangles |
| Scaling/rotation | no | yes |
| Selected by | `mFont_CHAR_FLAG_USE_POLY` | same flag |

Both call `mFont_gppLoadTexture` (`m_font_main.c_inc:324-346`), which **re-uploads the entire
0x6000-byte atlas per glyph**. On GC that was a TMEM tile load; on PC it becomes a texture-cache
lookup per character. This is the reason `pc_text_draw.c` exists.

### 3.3 The control-code VM

Strings are byte streams interpreted at draw time. `CHAR_CONTROL_CODE` (127) introduces one of
~130 opcodes (`include/m_font.h:285-413`), each with an *attribute* class
(`mFont_CONT_ATTRIBUTE_*`) and a size, in `mFont_cont_info_tbl`
(`src/game/m_font_main.c_inc:42-167`). Classes:

| Attribute | Effect | Handled by |
|-|-|-|
| `CHARACTER` | per-glyph colour, scale | `mFontChar_cont_proc_get` (`:793`) |
| `SENTENCE` | line offset/type/scale, wide space | `mFontSentence_cont_proc_get` (`:946`) |
| `STRING` | substitute player name, item name, date, free strings | `mMsg_Copy*` in `m_msg.h:317-336` |
| `DEMO` | drive the talking actor's animation | `m_msg_ctrl.c_inc` |
| `BGM` / `SE` | music + voice control | `m_msg_sound.c_inc` |

`mFont_CodeSize_get` (`:168`) walks the stream; every consumer that indexes text must use it
rather than `+1`, because control codes are 2+ bytes.

Substitution slots on the window: 20 free strings × 16 chars, 5 item strings, 1 mail string
(`include/m_msg.h:199-205`). `mMsg_SET_FREE_STR` and friends are the public entry points.

### 3.4 String sources

| Source | Resource | Loader |
|-|-|-|
| Messages (dialogue) | `RESOURCE_MESSAGE` + `RESOURCE_MESSAGE_TABLE` | `mMsg_LoadMsgData`, `m_msg_main.c_inc:373` |
| Strings (nouns, months, units) | `RESOURCE_STRING` + `RESOURCE_STRING_TABLE` | `mString_Load_StringFromRom`, `src/m_string.c:22` |
| Item names | `RESOURCE_*` via `m_item_name.c` | — |
| Choice options | `RESOURCE_SELECT` + `RESOURCE_SELECT_TABLE`, 607 entries | `mChoice_Load_ChoseStringFromRom`, `m_choice.c:91-94` |

Both use the same indexed-blob format read by `mMsg_Get_BodyParam`
(`m_msg_main.c_inc:283-303`): a `u32` offset table in ARAM, byte-swapped on PC
(`pc_bswap32_array(tmp_buff, 16)`), then a 32-byte-aligned DMA of the payload. Message ids run to
`MSG_MAX = 0x3F91` (`include/m_msg_data.h:22`).

### 3.5 Synthesising text without a ROM string

The netcode needed a message that does not exist on the disc. The pattern
(`m_msg_main.c_inc:325-376`) is:

1. Reserve an id at or past `MSG_MAX` — `MSG_PC_ONLINE_TOWN_TIME` is `MSG_MAX`
   (`include/m_msg_data.h:25`).
2. Widen the `mMsg_ChangeMsgData` validity test for it (`m_msg_main.c_inc:432-435`).
3. Write bytes straight into `msg_data->text_buf.data` with helpers that append literal text and
   `CHAR_CONTROL_CODE`-prefixed opcodes, then set `msg_len = mMsg_Count_MsgData(...)`.

This is the supported way to add new dialogue. It costs nothing at build time and works with the
full control-code VM (the online-time message uses the hour/min/AM-PM/date substitutions).

### 3.6 `pc_text_draw` — the PC fast path

`pc/src/pc_text_draw.c` re-implements glyph emission for ASCII-only PC menus:

- One state setup + **one atlas load per call** instead of per glyph.
- Batches 7 glyphs per `gSPVertex` (28 verts, the 5-bit index limit), packing triangles with
  `gSPNTrianglesInit_5b` / `gSPNTriangles_5b`. A 46-char string becomes ~7 draw calls instead
  of ~46.
- Geometry math is a deliberate clone of `mFont_gppDrawCharPoly`'s cut-mode + half-texel inset,
  so output is pixel-identical to `mFont`.

It has no control codes, no reveal animation, no voice SE. `mFont` remains the path for anything
that needs those. `pc_text_width` measures with the same kerning table.

---

## 4. Dialogue: `mMsg` + `mChoice`

### 4.1 State machine

One global `mMsg_Window_c` (`src/game/m_msg.c:20`), driven by an 8-state table dispatched twice
per frame — once for the *requested* transition, once for the *current* state
(`m_msg.c:48-80`):

```
HIDE → APPEAR → NORMAL ⇄ CURSOL → DISAPPEAR → HIDE
             ↘ APPEAR_WAIT / WAIT / DISAPPEAR_WAIT ↗
```

| State | File | Lines | Role |
|-|-|-|-|
| `NORMAL` | `m_msg_normal.c_inc` | 142 | page shown, waiting for A |
| `CURSOL` | `m_msg_cursol.c_inc` | 1272 | the typewriter reveal + control-code execution |
| `APPEAR`/`DISAPPEAR` | `m_msg_appear.c_inc`, `m_msg_disappear.c_inc` | 89/49 | zoom in/out |
| `*_WAIT` | `m_msg_*_wait.c_inc` | 47/18/30 | held open across a scene event |

`CURSOL` is where the work happens: it advances `end_text_cursor_idx` through the buffer,
executing control codes as it passes them, which is why colour/scale/voice changes take effect
mid-reveal.

Requests are priority-checked (`mMsg_Check_request_priority`) so an actor cannot stomp a
higher-priority window. All the `mMsg_*` macros at `include/m_msg.h:347-377` operate on the
singleton via `mMsg_Get_base_window_p()`.

### 4.2 Layout and assets

Window geometry is data, not code: three 4-bit textures plus a 24-vertex mesh, loaded at runtime
on PC (`src/game/m_msg_data.c_inc`):

| Asset | Size | Disc offset |
|-|-|-|
| `con_kaiwa2_w1_tex` | 0x800 | `0x2E71A0` |
| `con_kaiwa2_w2_tex` | 0x1000 | `0x2E79A0` |
| `con_kaiwa2_w3_tex` | 0x1000 | `0x2E89A0` |
| `con_kaiwa2_v` (verts) | 0x180 | `0x2E99A0` |
| `con_namefuti_TXT` (nameplate) | 0x400 | `0x2E9BA0` |
| `con_kaiwaname_v` | 0x40 | `0x2E9FA0` |

Text is laid out at `centre - (96, 32) × scale`, 4 lines max, 16 px line pitch
(`m_msg_draw_font.c_inc:32-33`, `:110`; `mMsg_MAX_LINE` = 4). Each line gets its own colour slot
(`font_color[4]`). The "press A" arrow is drawn at a hardcoded `(257, 136)`
(`m_msg_draw_window.c_inc:66`).

The window body is a scaled modelview push (`mMsg_SetMatrix`, `m_msg_draw_window.c_inc:1-21`), so
the zoom animation is free — only `window_scale` changes.

### 4.3 `mChoice`

The choice box is a **member of the message window** (`mMsg_Window_c::choice_window`,
`include/m_msg.h:224`), not an independent widget. Up to 6 options × 16 chars
(`include/m_choice.h:11-26`), its own 4-state machine, its own auto-repeat timer for held stick
input (`mChoice_AUTOMOVE_*`). It draws after the message body in `mMsg_Draw_Window`
(`m_msg.c:94`) so it stacks above.

Options come from `RESOURCE_SELECT` (607 entries) or are set directly with
`mChoice_Set_choice_data`. `mChoice` is the only original UI that has PC mouse support wired in
(`src/game/m_choice.c`, `#ifdef MOUSE_INPUT`).

---

## 5. Submenu overlays — the real menu framework

Thirty overlays (`enum submenu_overlay`, `include/m_submenu.h:64-99`) totalling ~47 000 lines.
This is where inventory, map, catalog, design editor, bank, mail, diary and the rest live.

### 5.1 Lifecycle

`Submenu` (`include/m_submenu.h:169-202`) lives inside `GAME_PLAY`. Five process states
(`mSM_PROCESS_*`):

```
WAIT ──open request──→ PREWAIT ──prerender done──→ LINKWAIT ──→ PLAY ──→ END ──→ WAIT
```

- **`mSM_submenu_ctrl`** (`src/game/m_submenu.c:253-319`, called from `m_play.c:511`) is the only
  place an overlay is opened by input. Start/Y opens the inventory, X opens the map (guarded by
  `Common_Get(map_flag)` and an ocean-acre check), A in front of a bulletin/map board opens those.
  It also refuses while a fade or wipe is running.
- **PREWAIT/prerender**: the frame is captured to a texture and re-blitted as the menu backdrop.
  `makeBumpTexture` (`m_play.c:741-806`) drives the three-step
  `PRERENDER_INIT → PRERENDER_WAIT → PRERENDER_DONE` handshake via `copy_efb_to_texture` and
  `prbuf`. This is why the world freezes visually behind a menu without the simulation stopping.
- **LINKWAIT** (`m_submenu.c:343-382`) is the vestigial GC DLL-loading step. On this build
  overlays are statically linked; `SubmenuArea_DoLink` only flips bookkeeping flags
  (`SubmenuArea_allocp = (void*)1`). Nothing is actually paged in.
- **PLAY** runs `move_proc` then `draw_proc` each frame
  (`mSM_submenu_move` / `mSM_submenu_draw`, `m_submenu.c:418-430`).

### 5.2 Dispatch

`Submenu_Overlay_c` (`include/m_submenu_ovl.h:160-217`, 0xA04 bytes) is one static instance
(`ovl_base`, `m_submenu_ovl.c:44`) holding:

- `menu_info[30]` — per-overlay slide position/speed, proc status, return chain
  (`pre_menu_type` / `next_menu_type` form a menu **stack**, unwound by `mSM_return_func`,
  `m_submenu_ovl.c:2210-2246`).
- `menu_control` — the current `menu_move_func` / `menu_draw_func` pair, plus hand and tag
  callbacks, trigger state and the scrolling background texture offset.
- 30 typed pointers, one per overlay's private data (`inventory_ovl`, `map_ovl`, …).
- Ten function pointers (`draw_item_proc`, `cbuf_copy_proc`, `change_view_proc`, …) that overlays
  call **indirectly** — a GC DLL-era indirection that is now pure ceremony but must be preserved
  because every overlay calls through it.

`mSM_program_dlftbl[]` (`m_submenu_ovl.c:1617+`) maps overlay id → `{construct, destruct,
set_proc}`. The construct allocates the private struct from a file-static, calls `init`, then
`set_proc` installs the move/draw pair. `m_bank_ovl.c:351-416` is the clearest small example.

### 5.3 Open/close animation

Every overlay slides in from a screen edge. `mSM_set_new_start_data`
(`m_submenu_ovl.c:2066-2125`) is a 31-row table of `{pos_x, pos_y, speed_x, speed_y}`; e.g.
`{0, 300, 0, 75}` means "start 300 px below, move at 75 px/frame". Overlays read
`menu->position[0..1]` and add it to every hardcoded coordinate — see
`mBN_set_character_dl` (`m_bank_ovl.c:286-341`), where every literal is `X + pos_x`, `Y - pos_y`.

Motion is frame-rate compensated via `mSM_move_accum` + `graph_dt_60hz_ticks`, and interpolated
for display in `mSM_draw_with_move_interpolation` (`m_submenu_ovl.c:2342-2378`).

### 5.4 Two shared sub-overlays

| Overlay | File | Lines | Role |
|-|-|-|-|
| `m_tag_ovl.c` | tag | 9475 | The pocket/tab strip, item grid selection, hover names, drag targets. Used by inventory, catalog, mail, needlework, GBA, original-design menus. Has PC mouse hover state (`include/m_tag_ovl.h:292-302`). |
| `m_hand_ovl.c` | hand | 1181 | The animated 3D hand cursor, with a 7-entry keyframe animation table (`m_hand_ovl.c:26-40`) and per-action offsets. Also mouse-aware. |

They are opted into per-overlay by `mSM_OVL_FLAG_USE_TAG` / `USE_HAND`
(`include/m_submenu_ovl.h:58-63`). Any new grid-style menu should reuse the tag overlay rather
than re-implement selection.

### 5.5 Item icons

`mSM_draw_item` (`m_submenu_ovl.c:908`) → `mSM_set_dl_item` (`:412`) renders an item's 32 × 32
CI4 icon + 16-entry palette, with optional shadow, greying, present-wrap and mark badges. Icon and
palette memory is reserved through the scene object-exchange bank
(`mSM_Object_Exchange_keep_new_MenuTexAndPallet`, `m_submenu.c:649-662`).

### 5.6 Item filters

`mSM_check_open_inventory_itemlist` (`m_submenu.c:596-630`) turns an open-reason
(`mSM_IV_OPEN_*`, 17 of them) into a 16-bit slot mask via a per-reason predicate. This is the hook
point for "which items may I pick here" and already carries one PC-only behaviour change: the
curator filter hides non-donatable items under `PC_ENHANCEMENTS` (`m_submenu.c:579-588`).

---

## 6. Full-screen game states

`game_dlftbls[]` (`src/game/m_game_dlftbls.c:22-35`) enumerates the top-level states; the ones
that are UI:

| Index | State | File | Notes |
|-|-|-|-|
| 1 | `select` | `m_select.c` (1091) | The developer scene-select menu — Japanese labels via `gfxprint`, not player-facing |
| 5 | `trademark` | `m_trademark.c` | Legal screen |
| 6 | `player_select` | `src/player_select.c` (306) | Which resident to play |
| 7 | `save_menu` | `src/save_menu.c` (250) | Save/quit |
| 8 | `famicom_emu` | `famicom_emu.c` | NES; blocks pausing (`g_pc_nes_active`) |
| 9 | `prenmi` | `m_prenmi.c` | Reset screen |
| 10 | `pc_model_viewer` | `pc/src/pc_model_viewer.c` | PC-only, `--model-viewer` |

The **title screen menu** is not a game state — it is drawn by the logo actor,
`src/actor/ac_animal_logo.c:790-850` (`aAL_pc_menu_draw`), a fork addition under
`PC_ENHANCEMENTS`. Three items (Start / Options / Quit) with `pc_menu_sel`, and Options embeds the
same `pc_settings_menu` used by the pause menu. It publishes `g_pc_title_main_menu_visible` so the
pause menu knows to stay shut (`pc_pause_menu.c:41`).

---

## 7. PC-native UI

### 7.1 Pause menu

`pc/src/pc_pause_menu.c` — three pages (Main / Settings / Confirm-quit), drawn from `graph_main`
**after** `game_main()` returns (`src/graph.c:390`), so it appends to `font_thaga` last and lands
above all game UI but below fades.

Key design points worth preserving:

- **It does not pause the simulation.** The world keeps running and rendering; the menu only
  swallows input. Online, the label changes to `- Menu (Town Live) -` because the player's avatar
  is still standing in a live town (`pc_pause_menu.c:216`).
- Input is normalised to a device-independent `MenuAction` enum, with keyboard, controller
  buttons, and a latched left-stick axis mapping onto it (`:81-189`). New menus should copy this
  rather than reading SDL directly.
- Blocked entirely while the title menu or the NES emulator is up (`:41`).
- `g_pc_pause_input_drain` prevents the Enter that dismissed the menu leaking into gameplay.

### 7.2 Settings menu

`pc/src/pc_settings_menu.c` (993 lines) is shared by the pause menu and the title screen — the
host passes `with_dim_backdrop` to pick the presentation. Structure:

- 4 tabs × item tables (`pc_settings_menu.c:43-82`), 16 item ids.
- Edits go to a `s_pending` copy of `PCSettings`; `Apply` commits, `Back` prompts if dirty.
- Resolution changes get a 15-second auto-revert confirm page (`s_res_deadline`).
- Items marked `restart=1` are suffixed `*` and folded into a "requires restart" hint.
- A separate bindings sub-page captures raw SDL key/button events
  (`pc_settings_menu_handle_capture_event`), with a grace period so the confirming press does not
  become the new binding.

The backing store is `PCSettings` (`pc/include/pc_settings.h:8-25`) persisted to `settings.ini` by
`pc/src/pc_settings.c`.

### 7.3 Shared helpers

`pc/src/pc_menu_util.c` is the whole PC widget library: a dim rect, a selected/unselected colour
pair (yellow `255,235,120` / grey `200,200,200`), centred text, left text, and a two-choice row.
Selected rows scale to `PC_MENU_SCALE_SELECTED` = 1.15.

### 7.4 Keyboard typing

`pc/src/pc_typing.c` (guarded by `KEYBOARD_TYPING`) gives the in-game text editor real typing:
Tab toggles typing mode, `SDL_TEXTINPUT` bytes are mapped from UTF-8 to the game's custom
codepoints (`pc_utf8_to_game_code`, covering ASCII + Latin-1 accents), and pushed onto a ring
buffer that `m_editor_ovl.c` drains. Cursor keys and backspace become `PC_TYPING_CMD_*` sentinels.

### 7.5 Mouse

`pc/include/pc_mouse.h` exposes a once-per-frame snapshot (position in 320 × 240 interface coords,
button edges, wheel, lock). Compiled out cleanly via inline no-op stubs when `MOUSE_INPUT` is off.
Consumers today: `m_design_ovl.c` (15 sites — the pattern designer), `m_tag_ovl.c` (4),
`m_hand_ovl.c` (2), `m_choice.c` (1). **Everything else is pad-only.**

### 7.6 Build flags

`pc/CMakeLists.txt:43-47`: `BUGFIXES`, `PC_ENHANCEMENTS`, `KEYBOARD_TYPING`, `MOUSE_INPUT`.
All UI additions in this fork sit behind one of the last three, and each must compile away.

---

## 8. Input routing

| Layer | Source | Notes |
|-|-|-|
| SDL events | `pc/src/pc_main.c:455-520` | Routed to capture → pause menu → typing, in that order |
| Pad snapshot | `padmgr` → `m_controller.c` | `chkTrigger`, `getButton`, `getTrigger` |
| Overlay triggers | `mSM_make_trigger_data` (`m_submenu_ovl.c:2139-2149`) | Merges C-stick with **D-pad under `PC_ENHANCEMENTS`** by shifting the D-pad bits into the C-stick positions |
| Dialogue | `chkTrigger(BUTTON_A/B)` inside `m_msg_cursol.c_inc` | — |

There is no central UI focus manager. Whoever is drawing decides whether to read the pad; the
mutual exclusion is implicit (submenu `process_status`, `mMsg` `main_index`, `g_pc_paused`).

---

## 9. Constraints

| # | Constraint | Why it bites | Reference |
|-|-|-|-|
| 1 | `GRAPH_ALLOC` always draws from `poly_opa`'s tail | Font-heavy UI exhausts the *polygon* arena, not the font one | `include/graph.h:289` |
| 2 | Arena sizes are compile-time; overflow drops the frame | No graceful degradation | `include/sys_dynamic.h:27-36`, `src/graph.c:260` |
| 3 | Bucket order fixes Z ordering | You cannot put a UI element under the world without moving buckets | `src/graph.c:227-234` |
| 4 | UI space is 320 × 240, positions are hardcoded floats | Every overlay embeds literals; no layout engine | §2.3 |
| 5 | Font is a fixed 12 × 16 I4 atlas with a 256-entry custom encoding | No Unicode, no variable metrics, no second face | §3.1 |
| 6 | `mFont` reloads the whole atlas per glyph | Text-heavy screens are draw-call bound | `m_font_main.c_inc:324` |
| 7 | ROM/ARAM data is big-endian | Any new table read from the disc needs an explicit swap | `m_msg_main.c_inc:291-294` |
| 8 | Widescreen correction is opt-out via tagged NoOps | A new fullscreen element pillarboxes unless tagged | §2.4 |
| 9 | `mFont_UnSetMatrix` does not restore the projection | Draw order after a UI block matters | `m_font_main.c_inc:643` |
| 10 | Overlays are `-O0` decomp TUs with `NDEBUG` required | Cannot optimise them; asserts have side effects | `src/CLAUDE.md` |
| 11 | Submenu overlay data is file-static singletons | No two instances of the same overlay | `m_bank_ovl.c:400` |
| 12 | Every UI edit in `src/` is a future ac-decomp merge conflict | Prefer PC-side files and hooks | `src/CLAUDE.md` |

---

## 10. Change recipes

### 10.1 Add a pause-menu item

`pc/src/pc_pause_menu.c` only: bump `MAIN_ITEM_COUNT`, extend the `items[]` array in
`draw_main_page`, add a case to `main_activate`. If it opens a sub-page, add a `PauseMenuPage`
value and a branch in `handle_action` + `pc_pause_menu_draw`. No decomp file is touched.

### 10.2 Add a setting

1. Field in `PCSettings` (`pc/include/pc_settings.h`).
2. Load/save/default in `pc/src/pc_settings.c`.
3. `ITEM_*` enum value, a row in the right `tab_*_items[]`, a `item_cycle` case, an `item_format`
   case, and an `item_changed` case in `pc/src/pc_settings_menu.c`.
4. If it needs a restart, set `restart = 1` and add it to `recompute_restart_needed`.
5. Apply it in `apply_pending` (live) or at startup (restart-only).

### 10.3 Add new dialogue text

Follow §3.5: reserve an id ≥ `MSG_MAX`, widen `mMsg_ChangeMsgData`'s validity check, and build the
byte stream in a `mMsg_Load*` function. Use `mMsg_SET_FREE_STR` for runtime values rather than
formatting numbers into the literal — the control-code substitutions already handle
name/date/time/item, and free strings cover the rest.

### 10.4 Add a new submenu overlay

The heaviest option; only worth it for something that must live inside the pocket/menu stack.

1. `mSM_OVL_*` enum value (`include/m_submenu.h:64-99`) — **at the end**, because
   `mSM_set_new_start_data`'s table and `mSM_program_dlftbl[]` are positional.
2. A row in the slide-in table (`m_submenu_ovl.c:2068-2100`).
3. A `{ct, dt, set_proc}` row in `mSM_program_dlftbl[]` (`m_submenu_ovl.c:1617+`).
4. A private struct pointer in `Submenu_Overlay_c` (`include/m_submenu_ovl.h:180-211`).
5. `m_<name>_ovl.c` + `m_<name>_ovl.h` + `m_<name>_ovl_h.h` following `m_bank_ovl.c`'s shape:
   `construct` → `init` + `set_proc`; `move` reads `overlay->menu_control.trigger`; `draw` calls
   `(*menu->pre_draw_func)(...)` first, then its own frame and text passes.
6. Opt into tag/hand with `mSM_OVL_FLAG_USE_TAG` / `USE_HAND` if it is grid- or cursor-driven.
7. Add the source to `pc/CMakeLists.txt`.

For anything that is *not* part of the pocket flow (an overlay HUD, a player list, a chat box), a
PC-side module drawing into `font_thaga` via `pc_text_draw` is dramatically cheaper and merge-safe.

### 10.5 Replace or upscale UI art

All UI textures are runtime-loaded from the disc by offset
(`pc/src/pc_assets.c`, e.g. the message window at `0x2E71A0`). The texture-pack system
(`pc/src/pc_texture_pack.c`, `texture_pack/`) is the intended override path — a Dolphin-compatible
loader keyed on XXHash64 of the texture data (plus TLUT hash for CI formats), so a
`tex1_{W}x{H}_{hash}_{fmt}.dds` drop-in replaces any UI texture without touching the decomp
tables or the asset offset list. Font atlas replacement is
possible the same way, but the 12 × 16 cell geometry and the offset kerning table are hardcoded
(`include/m_font.h:518`, `m_font_offset.c_inc`), so a replacement must keep the same grid.

---

## 11. Gaps and improvement opportunities

Ranked by value-to-effort, based on what the code above does and does not do.

| # | Opportunity | Effort | Notes |
|-|-|-|-|
| 1 | **Online nameplates over remote players** | Small | `ac_net_remote_player.c` has no text at all today. Names are already replicated; drawing them needs a billboarded `pc_text_draw` call in the actor's draw, plus a distance/occlusion policy. This is the most visible missing multiplayer affordance. |
| 2 | **Chat / player-list overlay** | Medium | No chat UI exists anywhere. A PC-side `font_thaga` module plus a `pc_typing`-style input mode avoids touching decomp entirely; the transport side would be new protocol work. |
| 3 | **Route mMsg through `pc_text_draw`'s batching** | Medium | §3.2/§3.6 — dialogue still pays one atlas load per glyph. A batched path preserving control codes would cut UI draw calls several-fold on text-heavy screens (catalog, address book, diary). |
| 4 | **Kill the `mFont_gppLoadTexture` per-glyph reload** | Small–medium | Hoisting the atlas load to sentence scope is a strictly local change in `mFontSentence_gppDraw_before` and helps every UI stack at once. |
| 5 | **Mouse support beyond the design editor** | Medium | The hover/pick plumbing already exists in `m_tag_ovl.c`; extending it to the map, catalog and choice box is mostly wiring, and `pc_mouse.h` already reports 320 × 240 interface coordinates. |
| 6 | **A shared PC widget layer** | Medium | `pc_menu_util.c` is 61 lines and every new PC menu re-implements selection, scrolling and paging. A small list/tab/slider set would pay for itself on the next two menus. |
| 7 | **Resolution-independent UI** | Large | Everything is hardcoded in 320 × 240 with the pillarbox flag as the only lever (§2.4). True scaling means auditing every literal in ~47 000 lines of overlay code — probably only worth doing behind an opt-in "modern UI" flag for PC-native surfaces, leaving original overlays at 4:3. |
| 8 | **Arena headroom telemetry** | Small | `THA_GA_isCrash` already detects overflow but only sets a frame-drop flag (`src/graph.c:261-292`). Reporting free bytes per arena in the diagnostics output would make UI budget regressions visible before they drop frames. |
| 9 | **Font encoding beyond Latin-1** | Large | Ties into the atlas grid, the 256-entry codepoint table, the offset table and `pc_utf8_to_game_code`. Only worth doing alongside a localisation effort. |
| 10 | **Unify input focus** | Medium | Today mutual exclusion between pause menu / dialogue / overlay is implicit and each new surface adds another guard flag (`g_pc_paused`, `g_pc_title_main_menu_visible`, `g_pc_nes_active`, `start_refuse`). A single focus owner would stop that list from growing. |

### Multiplayer-specific note

Overlays that commit persistent state already branch on server authority rather than being
disabled — see `m_bank_ovl.c:60-67` and `:369-376`, where the bank menu clamps to wallet bells and
sends `Net_RequestBankTransfer` instead of writing the save. Any new UI that mutates inventory,
bells, tiles, housing or mail must follow that pattern and check
`docs/netcode/AUTHORITY_MATRIX.md` first: the UI proposes, the server commits.
