/* Standalone smoke test for the libretro host (no game, no GL):
 * loads a core + disc, runs N frames headless, reports video/audio activity.
 *
 *   psx_smoke <core.dll> <bios_dir> <disc.cue> [frames]
 *
 * Exit 0 = core produced video and audio; nonzero = something failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc_libretro_core.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: psx_smoke <core.dll> <bios_dir> <disc> [frames]\n");
        return 2;
    }
    const char* core = argv[1];
    const char* bios = argv[2];
    const char* disc = argv[3];
    int frames = argc > 4 ? atoi(argv[4]) : 600;

    if (pc_libretro_load(core, bios, ".") != 0) {
        fprintf(stderr, "SMOKE FAIL: core load\n");
        return 1;
    }
    if (pc_libretro_load_game(disc) != 0) {
        fprintf(stderr, "SMOKE FAIL: game load\n");
        pc_libretro_unload();
        return 1;
    }

    static int16_t audio_buf[16384 * 2];
    long long audio_total = 0;
    int video_frames = 0;
    unsigned w = 0, h = 0, pitch = 0;
    int fmt = -1;

    for (int i = 0; i < frames; i++) {
        pc_libretro_run_frame();
        audio_total += pc_libretro_audio_take(audio_buf, 16384);
        if (pc_libretro_video(&w, &h, &pitch, &fmt) != NULL) video_frames++;
    }

    /* Count non-black pixels in the final frame. */
    long long lit = 0;
    const void* frame = pc_libretro_video(&w, &h, &pitch, &fmt);
    if (frame != NULL) {
        for (unsigned y = 0; y < h; y++) {
            const unsigned char* row = (const unsigned char*)frame + (size_t)y * pitch;
            if (fmt == PC_LR_PIXFMT_XRGB8888) {
                const unsigned* px = (const unsigned*)row;
                for (unsigned x = 0; x < w; x++)
                    if ((px[x] & 0x00FFFFFF) != 0) lit++;
            } else {
                const unsigned short* px = (const unsigned short*)row;
                for (unsigned x = 0; x < w; x++)
                    if (px[x] != 0) lit++;
            }
        }
    }

    printf("SMOKE RESULT: %d/%d frames had video, last %ux%u fmt=%d pitch=%u, "
           "audio %lld frames (%.0f Hz), lit pixels %lld/%u\n",
           video_frames, frames, w, h, fmt, pitch, audio_total,
           pc_libretro_sample_rate(), lit, w * h);

    pc_libretro_unload();

    int ok = video_frames > 0 && audio_total > 0 && lit > 0;
    printf(ok ? "SMOKE PASS\n" : "SMOKE FAIL: no activity\n");
    return ok ? 0 : 1;
}
