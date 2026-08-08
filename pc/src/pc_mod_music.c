/* pc_mod_music.c - see pc_mod_music.h */

#include "pc_mod_music.h"

#include <string.h>

typedef struct {
    const int16_t* pcm;
    size_t frames;
    size_t loop_start;
} Song;

static Song g_songs[PC_MOD_MUSIC_SLOT_COUNT];
static int g_playing;
static uint8_t g_slot;
static size_t g_cursor;          /* frame position */
static int g_volume = 256;

/* Fade state. Applied as a ramp per frame so a stop is not an audible click --
 * the stereo's own stop is a fade, and a custom track cutting dead would sound
 * like a bug. */
static size_t g_fade_total;
static size_t g_fade_left;

static int slot_index(uint8_t slot) {
    if (slot < PC_MOD_MUSIC_FIRST_SLOT) return -1;
    if (slot >= PC_MOD_MUSIC_FIRST_SLOT + PC_MOD_MUSIC_SLOT_COUNT) return -1;
    return slot - PC_MOD_MUSIC_FIRST_SLOT;
}

int pc_mod_music_register(uint8_t slot, const int16_t* pcm, size_t frames, size_t loop_start_frame) {
    const int index = slot_index(slot);
    if (index < 0 || pcm == NULL || frames == 0) return 0;
    /* A loop point past the end would restart into nothing; clamp rather than
     * refuse, since the audio is otherwise fine. */
    if (loop_start_frame >= frames) loop_start_frame = 0;
    g_songs[index].pcm = pcm;
    g_songs[index].frames = frames;
    g_songs[index].loop_start = loop_start_frame;
    return 1;
}

int pc_mod_music_play(uint8_t slot) {
    const int index = slot_index(slot);
    if (index < 0) return 0;
    /* Registered check is deliberate: a slot the client has not received audio
     * for plays silence, never a different song. A stereo showing the title the
     * player chose must not play something else. */
    if (g_songs[index].pcm == NULL) {
        g_playing = 0;
        return 0;
    }
    g_slot = slot;
    g_cursor = 0;
    g_fade_total = 0;
    g_fade_left = 0;
    g_playing = 1;
    return 1;
}

void pc_mod_music_stop(size_t fade_frames) {
    if (!g_playing) return;
    if (fade_frames == 0) {
        g_playing = 0;
        g_fade_total = 0;
        g_fade_left = 0;
        return;
    }
    g_fade_total = fade_frames;
    g_fade_left = fade_frames;
}

int pc_mod_music_playing(void) { return g_playing; }
uint8_t pc_mod_music_current_slot(void) { return g_playing ? g_slot : 0; }

void pc_mod_music_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 256) volume = 256;
    g_volume = volume;
}

static int16_t saturate(int32_t value) {
    /* Saturating rather than wrapping: a wrapped sample is a loud click, which
     * is far more noticeable than the clipping it replaces. */
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

void pc_mod_music_mix(int16_t* out, int samples) {
    const int index = slot_index(g_slot);
    const Song* song;
    int frames;
    int i;

    if (!g_playing || out == NULL || samples <= 0 || index < 0) return;
    song = &g_songs[index];
    if (song->pcm == NULL || song->frames == 0) return;

    frames = samples / 2;   /* interleaved stereo */

    for (i = 0; i < frames; i++) {
        int32_t gain = g_volume;
        int32_t left;
        int32_t right;

        if (g_fade_total > 0) {
            /* Linear ramp. Computed per frame so a fade spanning several
             * callbacks stays continuous across the boundary. */
            gain = (int32_t)((int64_t)g_volume * (int64_t)g_fade_left / (int64_t)g_fade_total);
            if (g_fade_left > 0) g_fade_left--;
        }

        left = (int32_t)song->pcm[g_cursor * 2];
        right = (int32_t)song->pcm[g_cursor * 2 + 1];
        out[i * 2] = saturate((int32_t)out[i * 2] + ((left * gain) >> 8));
        out[i * 2 + 1] = saturate((int32_t)out[i * 2 + 1] + ((right * gain) >> 8));

        g_cursor++;
        if (g_cursor >= song->frames) {
            /* Loops to the declared point rather than to zero, so a track with
             * an intro does not replay it every time round. */
            g_cursor = song->loop_start;
        }

        /* Checked after mixing, not before: a fade that runs out mid-callback
         * must stop within that same callback. Testing at the top instead left
         * playback "finished" but still reporting as playing until the next
         * call, which a caller watching pc_mod_music_playing would see as a
         * track that would not stop. */
        if (g_fade_total > 0 && g_fade_left == 0) {
            g_playing = 0;
            return;
        }
    }
}

void pc_mod_music_reset(void) {
    memset(g_songs, 0, sizeof(g_songs));
    g_playing = 0;
    g_slot = 0;
    g_cursor = 0;
    g_volume = 256;
    g_fade_total = 0;
    g_fade_left = 0;
}
