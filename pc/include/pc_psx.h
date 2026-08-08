#ifndef PC_PSX_H
#define PC_PSX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PS1 emulation glue for the PlayStation furniture item: wraps the generic
 * libretro host (pc_libretro_core) with the game's GL/audio plumbing.
 * Driven by the psx_emu game scene (src/psx_emu.c). */

/* Boot result codes for the scene's error dialogs. */
enum {
    PC_PSX_OK = 0,
    PC_PSX_ERR_NO_CORE = -1, /* core DLL missing/unloadable */
    PC_PSX_ERR_NO_GAME = -2, /* no disc image found in psx_roms_dir */
    PC_PSX_ERR_BOOT = -3,    /* core refused the disc (bad cue, missing BIOS...) */
};

/* Load core + disc per settings.ini ([PSX] section). Returns PC_PSX_OK or an
 * error code above. */
int pc_psx_boot(void);

/* --- disc list (the in-room picker) ---------------------------------- */

/* Rescan psx_roms_dir and return how many discs are available (capped at
 * PC_PSX_MAX_GAMES). Call before the title/boot accessors below. */
#define PC_PSX_MAX_GAMES 64
int pc_psx_list_games(void);

/* Copy the scanned titles into buf as fixed 16-byte space-padded records in
 * the game's charset (the format aMR_GetNameString hands to the choice
 * widget). Returns the number of records written. */
int pc_psx_list_fill_titles(char* buf, int buf_size);

/* Boot the disc at the given list index (from the most recent
 * pc_psx_list_games call). Returns PC_PSX_OK or an error code. */
int pc_psx_boot_index(int index);

/* --- per-game furniture items ---------------------------------------- */

/* Discs owned by the dedicated per-game console items, in furniture order. */
int pc_psx_item_count(void);
const char* pc_psx_item_disc(int item_index);

/* Remember which disc the next boot should load. select_item uses the
 * per-game table above; select_index uses the last scanned list. */
void pc_psx_select_item(int item_index);
void pc_psx_select_index(int list_index);

/* Boot whatever was last selected. Returns PC_PSX_OK or an error code. */
int pc_psx_boot_selected(void);

/* Run one emulated frame: apply input, run the core, push audio to the
 * game's 32kHz output. buttons is a PC_LR_PAD_* bitmask (pc_libretro_core.h). */
/* Run one emulated frame. buttons is a PC_LR_PAD_* bitmask; stick_x/stick_y
 * are the left stick in libretro's -32768..32767 range (+X right, +Y down). */
void pc_psx_frame(uint16_t buttons, int16_t stick_x, int16_t stick_y);

/* Draw the current framebuffer to the window (own GL pipeline, mirrors the
 * NES path; honors the nes_aspect setting). */
void pc_psx_render(void);

/* Flush the memory card to disk without shutting down. */
void pc_psx_save(void);

/* Save + unload everything and restore the game's GL state. */
void pc_psx_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_PSX_H */
