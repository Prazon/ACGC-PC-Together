/* pc_mod_music.h - custom song playback for the house stereos (P9).
 *
 * The original K.K. tracks are JAudio *sequences*. A mod's song is decoded PCM,
 * which is strictly simpler: it needs no synthesis, only mixing into the buffer
 * pc_audio.c already fills. That is why custom audio is easier than original
 * audio here rather than harder.
 *
 * Mixing is deliberately free of SDL so the arithmetic -- saturation, looping,
 * fades -- can be tested on its own. pc_audio.c calls pc_mod_music_mix from its
 * callback; everything else is host-independent.
 *
 * Format is 16-bit signed stereo interleaved at the device rate. Resampling and
 * decoding happen at pack time, not here: the audio callback runs on a realtime
 * thread and must not do work it can avoid.
 */
#ifndef PC_MOD_MUSIC_H
#define PC_MOD_MUSIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Song slots match the stereo's music_box bits. Only 55..63 are custom; the
 * rest belong to the original minidisks. */
#define PC_MOD_MUSIC_FIRST_SLOT 55
#define PC_MOD_MUSIC_SLOT_COUNT 9

/* Registers decoded PCM for a slot. The buffer is borrowed and must outlive
 * playback -- in the real flow it lives in the mod arena. `frames` counts
 * stereo frames, so the buffer holds frames * 2 samples. Returns 0 for a slot
 * outside the custom range or a null buffer. */
int pc_mod_music_register(uint8_t slot, const int16_t* pcm, size_t frames, size_t loop_start_frame);

/* Starts a slot, replacing whatever was playing. A slot with no registered PCM
 * plays silence rather than the wrong song -- for a stereo showing a title the
 * player chose, wrong audio is worse than none. */
int pc_mod_music_play(uint8_t slot);

/* Fades out over `frames` and stops. 0 stops immediately. */
void pc_mod_music_stop(size_t fade_frames);

int pc_mod_music_playing(void);
uint8_t pc_mod_music_current_slot(void);

/* Mixes into `out` (interleaved stereo, `samples` total samples, so
 * samples / 2 frames), saturating rather than wrapping. Safe to call with
 * nothing playing. Called from the audio callback. */
void pc_mod_music_mix(int16_t* out, int samples);

/* 0-256, applied to the custom track only. */
void pc_mod_music_set_volume(int volume);

void pc_mod_music_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_MUSIC_H */
