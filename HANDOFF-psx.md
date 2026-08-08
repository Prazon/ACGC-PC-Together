# Handoff — custom furniture + PlayStation emulation

Covers the uncommitted work adding custom furniture items and a libretro-based
PS1 emulator to the PC port. Written 2026-08-07. Nothing here is committed.

---

## 1. Status at a glance

| Piece | State |
|-|-|
| PlayStation furniture item + custom model | **Working, confirmed in-game** |
| libretro host + SwanStation, boots Crash Bandicoot | **Working, confirmed in-game** |
| Memory-card saves, L+R+Z exit, audio | Implemented; not separately verified |
| "quit your PlayStation" dialog wording | Built, **not visually confirmed** |
| Disc picker (multi-game menu) | Written, **never compiled** |
| Four per-game consoles (Crash/Spyro/Tomba!/Ape Escape) | Written, **never compiled** |
| Server-published game manifest | Researched + designed, **not written** |
| Xbox 360 / Xenia | **Blocked** — see §7 |

**The most important fact for whoever picks this up: everything after the
`--auto-start` build is uncompiled.** The last successful build predates the
disc picker, the per-game consoles, the `psx_games` setting, and all the
furniture-table appends for the four new items. Build before assuming anything
in those areas works.

---

## 2. How it works

A furniture item passes a "which game" number to the stock Famicom interaction
handler. Numbers >= 100 mean PlayStation, and `famicom_emu_init` routes those to
a PS1 scene instead of the NES one. Everything upstream — the yes/no dialog, the
fade, the audio hand-off, the return-to-room path — is the original game's code,
untouched. That reuse is the core design decision and the reason the feature is
small.

Two sentinel values (`include/psx_emu.h`):

- `PSX_EMU_ROM_BASE` (100) — open the disc picker. The generic "PlayStation" item.
- `PSX_EMU_ROM_DIRECT` (101) — boot the disc the furniture already chose. The
  per-game consoles.

The emulator itself is a generic libretro frontend. It loads any `*_libretro.dll`,
refuses hardware rendering so cores fall back to software, and per frame: applies
input, runs one frame, uploads the framebuffer as a GL quad, resamples audio into
the game's 32 kHz mixer. Modelled directly on the existing NES/fixNES integration.

### File map

| File | Role |
|-|-|
| `pc/src/pc_libretro_core.c` | Generic libretro host — DLL loading, callbacks, SRAM |
| `pc/src/pc_psx.c` | Game glue: disc scan/shelf, selection, GL render, audio |
| `src/psx_emu.c` | The emulator scene (mirrors `src/famicom_emu.c`) |
| `src/furniture/ac_psx_console.c` | Generic console (picker) |
| `src/furniture/ac_psx_game.c` | The four per-game consoles |
| `src/data/model/int_psx.c` | Console model, generated from `PSONE.obj` |
| `pc/src/pc_custom_furniture.c` | Item-name injection + F5 debug give |
| `tools/converters/obj2gfx.py` | OBJ → compiled-in model converter |
| `pc/tools/psx_smoke.c` | Headless core test (`psx_smoke.exe`) |

Modified stock files: `src/famicom_emu.c` (sentinel dispatch),
`src/actor/ac_my_room.c` + `ac_my_room_msg_ctrl.c_inc` (picker reuse),
`src/game/m_msg_main.c_inc` (dialog wording), `pc/src/pc_settings.c` (`[PSX]` keys).

### Configuration

`settings.ini`, `[PSX]` section: `psx_core`, `psx_roms_dir`, `psx_bios_dir`,
`psx_games` (a `|`-separated shelf for the picker; blank lists the whole folder),
and `psx_game` (legacy, superseded).

---

## 3. Adding things

**A new game for an existing console type** — two edits, kept adjacent on purpose:
the filename in `s_item_discs[]` (`pc/src/pc_psx.c`) and the display name in
`pc_custom_furniture_init()` (`pc/src/pc_custom_furniture.c`). Then append one
furniture entry to every table in §5.

**A new console (SNES, GBA, …)** — the libretro host is already generic, so it is
a new core DLL, a new sentinel value, a scene cloned from `src/psx_emu.c`, a model,
and furniture entries. The one piece deliberately built for this is that the chosen
disc lives in `pc_psx.c` rather than being packed into `current_famicom_rom`.

**A model** — `python tools/converters/obj2gfx.py model.obj --name int_foo --out
src/data/model/int_foo.c`. Handles a single texture, or bakes flat material colours
into a small atlas when the OBJ has no texture. `--rotate-y` and `--fit` adjust
orientation and size.

---

## 4. Verification done

- Headless smoke test passes: core boots Crash Bandicoot, 640×448 video + 44.1 kHz
  audio flowing. Re-runnable:
  `psx_smoke.exe cores/swanstation_libretro.dll F:/bios "<disc>.cue" 900`
- In-game: PlayStation item spawns via F5, places, boots Crash. User-confirmed.
- Netcode suite 30/31. The failure is `IANA timezone DST` — Windows lacks tzdata;
  pre-existing and unrelated.
- Table alignment: all eleven per-furniture tables at 1271 entries, price table
  1272 with its `0xFFFF` terminator last. Re-run that check after any table edit.
- Three adversarial review passes; all confirmed findings fixed.

---

## 5. Gotchas that will bite

**Table lockstep.** A new furniture item must be appended to *all* of:
`m_ftr_def.h`, `m_name_table.h` (`ftr1_e`), `ac_furniture_profile_data.c_inc`,
`ac_furniture_data.c_inc`, `ftr_price.c` (before the `-1` sentinel — it is a
terminator, not a price), `m_room_type.c` (×2), `m_mark_room_ovl.c`,
`m_huusui_room_ovl_data.inc`, `m_item_name.c`, `ac_my_room_data.c_inc`, plus a
catalog row and a 16-byte name record. `furniture_quality` is read **unchecked** —
a missing entry there is a crash, not a glitch.

**Append after `FTR_DUMMY`, never before.** Inserting earlier shifts every item ID
in the bank and invalidates existing saves. Same reason the generic PlayStation
item must keep its index: it is already placed in the user's town.

**`current_famicom_rom` is an `s8`.** It carries the sentinel and truncates
silently. Worse than truncation: a value past 127 wraps negative, fails the
`>= 100` test, and silently boots the *NES* emulator. Never pack a game index in
there — that is why selection lives in `pc_psx.c`.

**The choice widget compacts NULL slots.** Passing NULL for a middle option does
not leave a gap; later options shift up. This already caused a critical bug where
a single-disc list turned "No thanks." into "boot disc #2".

**Item names are not ASCII.** The font is ASCII-compatible for letters, digits,
space and some punctuation; other codes render as accented glyphs, and `0x7F`,
`0x80`, `0xCD` are parser control bytes. Records are 16 bytes, space-padded, **no
NUL** — a NUL renders as a glyph. `psx_copy_title()` sanitizes.

**Most source files are CRLF.** Scripted edits must preserve line endings or the
whole file shows as changed.

**Building needs the exe closed** — the linker cannot replace a running
`AnimalCrossing.exe`. Never kill by image name; the user runs their own instances.

---

## 6. Open items

**Fresh-start crash — unresolved, highest priority.** With no save file, pressing
Start at the title screen produces an endless `__osFree` invalid-free loop
(constant address `0x983ec3a0`) and `OSPanic at __osMalloc.c:738`; the game hangs.
An investigation was launched but never reported back. **Critically, this was never
tested against clean `master`, so whether these changes caused it is unknown** —
that bisect is the first thing to do. It does not affect saves that already exist,
and the online quickstart path may bypass it.

**Server game manifest.** Fully researched, not written. Design: server config
lists title + filename, publishes to clients as message type 60, clients match by
filename against their own `psx_roms_dir`. Do **not** bump `kProtocolVersion` —
negotiation is exact-match lockstep and bumping locks out every unpatched peer;
unknown message types are dropped harmlessly. No file bytes ever transfer; each
player supplies their own discs.

**Smaller things**

- `src/data/model/int_x360.c` is compiled but unreferenced — ~150 KB of dead data.
  Delete it or use it.
- Menu titles truncate at 16 characters, so discs sharing a long prefix look
  identical (a "Final Fantasy VII (Disc 1/2/3)" set would).
- F5 gives only the generic console, not the four per-game items.
- `run_two_clients.bat` was rewritten to self-assemble its session folder; folder
  assembly is verified, an actual two-client launch is not.
- `pc_psx_boot()` / `psx_find_game()` are now dead but still exported.

---

## 7. Xbox 360 — why it stopped

The Xenia core at `F:\Game Dev Stuff\xenia_edge_libretro-win64` declares
`hw_render = "true"`: it renders through D3D12 or Vulkan and has no software path,
because a 360 GPU cannot reasonably be emulated into a plain memory framebuffer.
The host deliberately refuses hardware contexts, which is exactly why SwanStation
integrates cleanly.

Supporting it means accepting `SET_HW_RENDER`, standing up a D3D12/Vulkan device,
and sharing the rendered image into the game's OpenGL 3.3 context (DXGI shared
handles with `WGL_NV_DX_interop2`, or Vulkan external memory). That is a real
graphics-interop subsystem with driver-specific failure modes, and Halo 3 inside
another game's frame budget is optimistic even once it works.

The model is already converted and ready if the item is ever wanted decoratively,
or as a launcher for external Xenia.

---

## 8. Repo hygiene

Also changed, unrelated to furniture, both pre-existing Windows build breaks:
`tests/net/test_main.cpp` gained a missing `<algorithm>` include (GCC 15), and the
`Makefile` now links `ws2_32`/`bcrypt` instead of `-ldl` on Windows.

`.gitignore` gained the ISO and `bash.exe.stackdump` (not my change).

Build: `build_pc.bat`. Netcode tests: `make test` with MSYS2 `make` and `TMP` set
to a writable directory.
