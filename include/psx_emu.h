#ifndef PSX_EMU_H
#define PSX_EMU_H

#include "game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PS1 emulator scene (PC only). Piggybacks on the famicom emulator flow:
 * furniture passes rom_no >= PSX_EMU_ROM_BASE to famicom_emu_common_move_proc,
 * the stock dialog/fade chain runs, and famicom_emu_init dispatches here when
 * Common current_famicom_rom holds a PSX sentinel. */
/* rom_no values routed to the PlayStation scene. BASE opens the disc picker
 * (the generic console); DIRECT boots the disc the furniture already chose,
 * which is how the per-game consoles behave. */
#define PSX_EMU_ROM_BASE 100
#define PSX_EMU_ROM_DIRECT 101

/* The chosen disc itself lives in pc_psx.c (pc_psx_select_item /
 * pc_psx_select_index), so it is never squeezed through Common
 * current_famicom_rom, which is only an s8 and already carries the sentinel. */

/* Take over the GAME the famicom scene was booted with. */
extern void psx_emu_boot(GAME* game);

#ifdef __cplusplus
}
#endif

#endif /* PSX_EMU_H */
