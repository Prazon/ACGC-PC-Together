/* Checks the custom-song mixer.
 *
 * Mixing bugs are audible rather than fatal, which makes them easy to ship: a
 * wrapped sample is a click, a wrong loop point replays an intro forever, a
 * missing fade cuts dead. Each is asserted here.
 */

#include "pc_mod_music.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    /* Four stereo frames, distinguishable per frame. */
    static int16_t song[8] = { 100, -100, 200, -200, 300, -300, 400, -400 };
    static int16_t loud[4] = { 30000, -30000, 30000, -30000 };
    int16_t out[16];
    int i;

    pc_mod_music_reset();

    /* Slots outside the custom band are refused: 0..54 belong to the original
     * minidisks and must never be overwritten by a mod. */
    expect(pc_mod_music_register(0, song, 4, 0) == 0, "slot 0 refused");
    expect(pc_mod_music_register(54, song, 4, 0) == 0, "slot 54 refused");
    expect(pc_mod_music_register(64, song, 4, 0) == 0, "slot 64 refused");
    expect(pc_mod_music_register(55, song, 4, 0) == 1, "slot 55 accepted");
    expect(pc_mod_music_register(63, song, 4, 0) == 1, "slot 63 accepted");

    /* A slot with no audio plays silence, never a different song. */
    expect(pc_mod_music_play(56) == 0, "unregistered slot does not play");
    expect(!pc_mod_music_playing(), "and nothing is playing");
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 8);
    for (i = 0; i < 8; i++) {
        if (out[i] != 0) { printf("FAIL: silence expected at %d\n", i); failures++; break; }
    }

    /* Playing mixes additively into whatever is already there. */
    expect(pc_mod_music_play(55) == 1, "registered slot plays");
    expect(pc_mod_music_playing(), "playing reported");
    expect(pc_mod_music_current_slot() == 55, "current slot reported");
    memset(out, 0, sizeof(out));
    out[0] = 50;                                   /* pre-existing game audio */
    pc_mod_music_mix(out, 8);
    expect(out[0] == 150, "mixed on top of existing audio, not over it");
    expect(out[1] == -100, "right channel mixed");
    expect(out[6] == 400 && out[7] == -400, "fourth frame mixed");

    /* Looping returns to the declared point, not to zero, so a track with an
     * intro does not replay it every time round. */
    pc_mod_music_reset();
    expect(pc_mod_music_register(55, song, 4, 2) == 1, "registered with a loop point");
    expect(pc_mod_music_play(55) == 1, "plays");
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 16);                     /* 8 frames over a 4-frame song */
    expect(out[0] == 100, "frame 0");
    expect(out[6] == 400, "frame 3, the end");
    expect(out[8] == 300, "frame 4 loops to the loop point, not the start");
    expect(out[10] == 400, "frame 5");
    expect(out[12] == 300, "and keeps looping from there");

    /* A loop point past the end is clamped rather than refused. */
    pc_mod_music_reset();
    expect(pc_mod_music_register(55, song, 4, 99) == 1, "silly loop point accepted");
    pc_mod_music_play(55);
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 16);
    expect(out[8] == 100, "clamped to the start");

    /* Saturation, not wrapping. A wrapped sample is a loud click. */
    pc_mod_music_reset();
    expect(pc_mod_music_register(55, loud, 2, 0) == 1, "loud song registered");
    pc_mod_music_play(55);
    memset(out, 0, sizeof(out));
    out[0] = 30000;
    out[1] = -30000;
    pc_mod_music_mix(out, 4);
    expect(out[0] == 32767, "positive clipping saturates");
    expect(out[1] == -32768, "negative clipping saturates");

    /* Volume scales the custom track only. */
    pc_mod_music_reset();
    pc_mod_music_register(55, song, 4, 0);
    pc_mod_music_play(55);
    pc_mod_music_set_volume(128);                  /* half */
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 2);
    expect(out[0] == 50, "volume halves the mixed sample");

    /* An immediate stop is silent from the next sample. */
    pc_mod_music_reset();
    pc_mod_music_register(55, song, 4, 0);
    pc_mod_music_play(55);
    pc_mod_music_stop(0);
    expect(!pc_mod_music_playing(), "immediate stop stops");
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 8);
    expect(out[0] == 0, "and mixes nothing");

    /* A fade ramps down and then stops, rather than cutting dead. */
    pc_mod_music_reset();
    pc_mod_music_register(55, song, 4, 0);
    pc_mod_music_play(55);
    pc_mod_music_stop(4);                          /* over four frames */
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 8);
    expect(out[0] == 100, "fade starts at full gain");
    expect(out[2] < 200 && out[2] > 0, "and is quieter by the second frame");
    expect(!pc_mod_music_playing(), "the fade finished and stopped playback");

    /* A fade spanning two callbacks stays continuous rather than restarting. */
    pc_mod_music_reset();
    pc_mod_music_register(55, song, 4, 0);
    pc_mod_music_play(55);
    pc_mod_music_stop(8);
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 4);                      /* two frames */
    expect(pc_mod_music_playing(), "still fading after the first callback");
    memset(out, 0, sizeof(out));
    pc_mod_music_mix(out, 12);                     /* six more finishes it */
    expect(!pc_mod_music_playing(), "fade completes across callbacks");

    /* Defensive: nothing playing, null buffer, zero length. */
    pc_mod_music_reset();
    pc_mod_music_mix(NULL, 8);
    pc_mod_music_mix(out, 0);
    pc_mod_music_mix(out, -4);
    expect(1, "degenerate mix calls do not crash");

    printf(failures == 0 ? "mod_music: PASS\n" : "mod_music: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
