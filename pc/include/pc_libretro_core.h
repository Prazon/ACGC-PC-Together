#ifndef PC_LIBRETRO_CORE_H
#define PC_LIBRETRO_CORE_H

/* Minimal libretro frontend host: loads a *_libretro.dll, drives it one
 * frame at a time, and buffers its video/audio/input on the frontend side.
 * No game or GL dependencies — the game glue (pc_psx.c) and the standalone
 * smoke test both link against this. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_LR_PIXFMT_0RGB1555 = 0, /* matches RETRO_PIXEL_FORMAT_* values */
    PC_LR_PIXFMT_XRGB8888 = 1,
    PC_LR_PIXFMT_RGB565 = 2,
};

/* RETRO_DEVICE_ID_JOYPAD_* bit positions for pc_libretro_set_input. */
enum {
    PC_LR_PAD_B = 0, PC_LR_PAD_Y = 1, PC_LR_PAD_SELECT = 2, PC_LR_PAD_START = 3,
    PC_LR_PAD_UP = 4, PC_LR_PAD_DOWN = 5, PC_LR_PAD_LEFT = 6, PC_LR_PAD_RIGHT = 7,
    PC_LR_PAD_A = 8, PC_LR_PAD_X = 9, PC_LR_PAD_L = 10, PC_LR_PAD_R = 11,
};

/* Load the core DLL and init it. system_dir: BIOS folder; save_dir: SRAM
 * folder. Strings are copied. Returns 0 on success. */
int pc_libretro_load(const char* core_path, const char* system_dir, const char* save_dir);

/* Load a game by path (.cue/.chd/.iso). Returns 0 on success. */
int pc_libretro_load_game(const char* game_path);

/* Run one emulated frame. */
void pc_libretro_run_frame(void);

/* Last completed video frame (frontend-owned copy). Returns NULL if no frame
 * yet. fmt is one of PC_LR_PIXFMT_*. pitch is in bytes. */
const void* pc_libretro_video(unsigned* w, unsigned* h, unsigned* pitch, int* fmt);

/* Drain up to max_frames stereo frames of audio into dst (interleaved L,R).
 * Returns the number of frames written. */
int pc_libretro_audio_take(int16_t* dst, int max_frames);

/* Core's reported audio sample rate (valid after load_game). */
double pc_libretro_sample_rate(void);

/* Set the RetroPad digital state for port 0 (bitmask of PC_LR_PAD_*). */
void pc_libretro_set_input(uint16_t buttons);

/* Set port 0's stick positions, libretro range -32768..32767, +X right and
 * +Y down. Ignored unless analog mode is on (see below). */
void pc_libretro_set_analog(int16_t lx, int16_t ly, int16_t rx, int16_t ry);

/* Choose what port 0 presents as: nonzero for an analog pad (PS1 DualShock),
 * zero for the original digital pad. Cores default to digital, so without
 * this the sticks read as centred no matter what pc_libretro_set_analog says.
 * Takes effect at the next pc_libretro_load_game, so call it before booting.
 * Defaults to analog. */
void pc_libretro_set_analog_mode(int enable);

/* Write SRAM (PS1 memory card) to save_dir/<game>.srm. Safe to call anytime
 * after load_game. */
void pc_libretro_save_sram(void);

/* Unload game + core, saving SRAM first. Safe to call when nothing loaded. */
void pc_libretro_unload(void);

/* 1 if a game is currently loaded and runnable. */
int pc_libretro_loaded(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_LIBRETRO_CORE_H */
