/* Minimal libretro frontend host — see pc_libretro_core.h.
 *
 * Design notes:
 *  - The core's video callback hands us a pointer valid only during the call,
 *    so frames are copied into a frontend-owned buffer.
 *  - Audio is accumulated in a ring buffer and drained by the caller each
 *    game frame.
 *  - SET_HW_RENDER is refused and the GPU-renderer core option is answered
 *    with "Software", so DuckStation-family cores fall back to their software
 *    renderer and always deliver a plain framebuffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <SDL2/SDL.h>

#include "libretro.h"
#include "pc_libretro_core.h"

#define LR_MAX_W 1024
#define LR_MAX_H 768
#define LR_AUDIO_RING_FRAMES 65536 /* power of two */

static void* s_dll;
static int s_game_loaded;

static void (*p_retro_set_environment)(retro_environment_t);
static void (*p_retro_set_video_refresh)(retro_video_refresh_t);
static void (*p_retro_set_audio_sample)(retro_audio_sample_t);
static void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*p_retro_set_input_poll)(retro_input_poll_t);
static void (*p_retro_set_input_state)(retro_input_state_t);
static void (*p_retro_init)(void);
static void (*p_retro_deinit)(void);
static bool (*p_retro_load_game)(const struct retro_game_info*);
static void (*p_retro_unload_game)(void);
static void (*p_retro_run)(void);
static void (*p_retro_get_system_av_info)(struct retro_system_av_info*);
static void* (*p_retro_get_memory_data)(unsigned);
static size_t (*p_retro_get_memory_size)(unsigned);
static void (*p_retro_set_controller_port_device)(unsigned, unsigned);

static char s_system_dir[512];
static char s_save_dir[512];
static char s_sram_path[1024];

static int s_pixfmt = PC_LR_PIXFMT_0RGB1555; /* libretro default */
static uint8_t* s_frame;
static unsigned s_frame_w, s_frame_h, s_frame_pitch;
static int s_have_frame;

static int16_t s_audio_ring[LR_AUDIO_RING_FRAMES * 2];
static unsigned s_audio_write, s_audio_read; /* in frames */
static double s_sample_rate = 44100.0;

static uint16_t s_pad_state;

/* Port 0 stick positions in libretro's -32768..32767 range, indexed
 * [RETRO_DEVICE_INDEX_ANALOG_LEFT|RIGHT][RETRO_DEVICE_ID_ANALOG_X|Y].
 * Meaningless unless s_analog_mode put the port in analog mode: a core told
 * to emulate a digital pad never asks for these. */
static int16_t s_analog_state[2][2];
static int s_analog_mode = 1;

/* ---------------------------------------------------------------------- */
/* Callbacks                                                              */
/* ---------------------------------------------------------------------- */

static void lr_log(enum retro_log_level level, const char* fmt, ...) {
    if (level < RETRO_LOG_WARN) return; /* keep stdout usable */
    va_list ap;
    va_start(ap, fmt);
    printf("[libretro] ");
    vprintf(fmt, ap);
    va_end(ap);
}

static bool lr_environment(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**)data = s_system_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char**)data = s_save_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool*)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            int fmt = *(const enum retro_pixel_format*)data;
            if (fmt < 0 || fmt > 2) return false;
            s_pixfmt = fmt;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback*)data)->log = lr_log;
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable* var = (struct retro_variable*)data;
            /* Force the software GPU renderer on DuckStation-family cores so
             * we always get a plain framebuffer. Unknown keys must set value
             * to NULL and still return true (libretro.h GET_VARIABLE docs). */
            if (var != NULL && var->key != NULL && strstr(var->key, "GPU.Renderer") != NULL) {
                var->value = "Software";
                return true;
            }
            if (var != NULL) var->value = NULL;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = false;
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return false; /* software renderer only */
        default:
            return false;
    }
}

static void lr_video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    if (data == NULL) return; /* dupe frame — keep the previous one */
    if (width > LR_MAX_W || height > LR_MAX_H) return;

    /* Copy row-by-row at a packed stride: the ABI does not bound pitch
     * relative to width, so trusting pitch*height could overflow s_frame
     * (and over-read the core's last row). */
    size_t bpp = (s_pixfmt == PC_LR_PIXFMT_XRGB8888) ? 4 : 2;
    size_t row_bytes = (size_t)width * bpp;
    if (row_bytes > pitch) return; /* malformed frame */
    for (unsigned y = 0; y < height; y++)
        memcpy(s_frame + (size_t)y * row_bytes, (const uint8_t*)data + (size_t)y * pitch, row_bytes);

    s_frame_w = width;
    s_frame_h = height;
    s_frame_pitch = (unsigned)row_bytes;
    s_have_frame = 1;
}

static void lr_audio_push(int16_t left, int16_t right) {
    unsigned next = (s_audio_write + 1) & (LR_AUDIO_RING_FRAMES - 1);
    if (next == (s_audio_read & (LR_AUDIO_RING_FRAMES - 1))) return; /* full — drop */
    unsigned w = s_audio_write & (LR_AUDIO_RING_FRAMES - 1);
    s_audio_ring[w * 2 + 0] = left;
    s_audio_ring[w * 2 + 1] = right;
    s_audio_write = next;
}

static void lr_audio_sample(int16_t left, int16_t right) {
    lr_audio_push(left, right);
}

static size_t lr_audio_sample_batch(const int16_t* data, size_t frames) {
    for (size_t i = 0; i < frames; i++)
        lr_audio_push(data[i * 2 + 0], data[i * 2 + 1]);
    return frames;
}

static void lr_input_poll(void) {
}

static int16_t lr_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    unsigned base = device & RETRO_DEVICE_MASK;

    if (base == RETRO_DEVICE_ANALOG) {
        /* index 2 is RETRO_DEVICE_INDEX_ANALOG_BUTTON — per-button pressure,
         * which this frontend does not source. 0 there means "no pressure
         * data"; the core falls back to the digital state below. */
        if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT) return 0;
        if (id > RETRO_DEVICE_ID_ANALOG_Y) return 0;
        return s_analog_state[index][id];
    }

    if (base != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)s_pad_state;
    if (id < 16) return (s_pad_state >> id) & 1;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */

#define RESOLVE(name)                                                        \
    do {                                                                     \
        *(void**)(&p_##name) = SDL_LoadFunction(s_dll, #name);               \
        if (p_##name == NULL) {                                              \
            printf("[libretro] missing symbol %s in core\n", #name);         \
            SDL_UnloadObject(s_dll);                                         \
            s_dll = NULL;                                                    \
            return -1;                                                       \
        }                                                                    \
    } while (0)

int pc_libretro_load(const char* core_path, const char* system_dir, const char* save_dir) {
    if (s_dll != NULL) pc_libretro_unload();

    s_dll = SDL_LoadObject(core_path);
    if (s_dll == NULL) {
        printf("[libretro] failed to load core %s: %s\n", core_path, SDL_GetError());
        return -1;
    }

    RESOLVE(retro_set_environment);
    RESOLVE(retro_set_video_refresh);
    RESOLVE(retro_set_audio_sample);
    RESOLVE(retro_set_audio_sample_batch);
    RESOLVE(retro_set_input_poll);
    RESOLVE(retro_set_input_state);
    RESOLVE(retro_init);
    RESOLVE(retro_deinit);
    RESOLVE(retro_load_game);
    RESOLVE(retro_unload_game);
    RESOLVE(retro_run);
    RESOLVE(retro_get_system_av_info);
    RESOLVE(retro_get_memory_data);
    RESOLVE(retro_get_memory_size);
    RESOLVE(retro_set_controller_port_device);

    snprintf(s_system_dir, sizeof(s_system_dir), "%s", system_dir);
    snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);

    s_frame = (uint8_t*)malloc((size_t)LR_MAX_W * LR_MAX_H * 4);
    if (s_frame == NULL) {
        SDL_UnloadObject(s_dll);
        s_dll = NULL;
        return -1;
    }
    s_have_frame = 0;
    s_audio_read = s_audio_write = 0;
    s_pad_state = 0;
    memset(s_analog_state, 0, sizeof(s_analog_state));
    s_pixfmt = PC_LR_PIXFMT_0RGB1555;

    p_retro_set_environment(lr_environment);
    p_retro_init();
    p_retro_set_video_refresh(lr_video_refresh);
    p_retro_set_audio_sample(lr_audio_sample);
    p_retro_set_audio_sample_batch(lr_audio_sample_batch);
    p_retro_set_input_poll(lr_input_poll);
    p_retro_set_input_state(lr_input_state);

    printf("[libretro] core loaded: %s\n", core_path);
    return 0;
}

static void lr_sram_path_for(const char* game_path) {
    const char* base = strrchr(game_path, '/');
    const char* base2 = strrchr(game_path, '\\');
    if (base2 > base) base = base2;
    base = base ? base + 1 : game_path;
    snprintf(s_sram_path, sizeof(s_sram_path), "%s/%s.srm", s_save_dir, base);
}

static void lr_sram_load(void) {
    void* sram = p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sram == NULL || size == 0) return;
    FILE* f = fopen(s_sram_path, "rb");
    if (f == NULL) return;
    size_t got = fread(sram, 1, size, f);
    fclose(f);
    printf("[libretro] loaded SRAM %s (%u/%u bytes)\n", s_sram_path, (unsigned)got, (unsigned)size);
}

int pc_libretro_load_game(const char* game_path) {
    if (s_dll == NULL) return -1;

    struct retro_game_info info;
    memset(&info, 0, sizeof(info));
    info.path = game_path;

    if (!p_retro_load_game(&info)) {
        printf("[libretro] retro_load_game failed for %s\n", game_path);
        return -1;
    }

    struct retro_system_av_info av;
    memset(&av, 0, sizeof(av));
    p_retro_get_system_av_info(&av);
    s_sample_rate = av.timing.sample_rate > 0 ? av.timing.sample_rate : 44100.0;
    printf("[libretro] game loaded: %s (%.2f fps, %.0f Hz, max %ux%u)\n", game_path,
           av.timing.fps, s_sample_rate, av.geometry.max_width, av.geometry.max_height);

    /* Cores default port 0 to a digital pad, so the sticks are dead until the
     * port is explicitly switched. Must come after retro_load_game: swanstation
     * builds its controller objects during load and would discard a device set
     * before that. */
    p_retro_set_controller_port_device(0, s_analog_mode ? RETRO_DEVICE_ANALOG : RETRO_DEVICE_JOYPAD);
    printf("[libretro] port 0 device: %s\n", s_analog_mode ? "analog (DualShock)" : "digital pad");

    lr_sram_path_for(game_path);
    lr_sram_load();

    s_game_loaded = 1;
    return 0;
}

void pc_libretro_run_frame(void) {
    if (s_game_loaded) p_retro_run();
}

const void* pc_libretro_video(unsigned* w, unsigned* h, unsigned* pitch, int* fmt) {
    if (!s_have_frame) return NULL;
    *w = s_frame_w;
    *h = s_frame_h;
    *pitch = s_frame_pitch;
    *fmt = s_pixfmt;
    return s_frame;
}

int pc_libretro_audio_take(int16_t* dst, int max_frames) {
    int n = 0;
    while (n < max_frames && s_audio_read != s_audio_write) {
        unsigned r = s_audio_read & (LR_AUDIO_RING_FRAMES - 1);
        dst[n * 2 + 0] = s_audio_ring[r * 2 + 0];
        dst[n * 2 + 1] = s_audio_ring[r * 2 + 1];
        s_audio_read = (s_audio_read + 1) & (LR_AUDIO_RING_FRAMES - 1);
        n++;
    }
    return n;
}

double pc_libretro_sample_rate(void) {
    return s_sample_rate;
}

void pc_libretro_set_input(uint16_t buttons) {
    s_pad_state = buttons;
}

void pc_libretro_set_analog(int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    s_analog_state[RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_X] = lx;
    s_analog_state[RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_Y] = ly;
    s_analog_state[RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_X] = rx;
    s_analog_state[RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_Y] = ry;
}

void pc_libretro_set_analog_mode(int enable) {
    s_analog_mode = enable ? 1 : 0;
}

void pc_libretro_save_sram(void) {
    if (!s_game_loaded) return;
    void* sram = p_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = p_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sram == NULL || size == 0) return;
    FILE* f = fopen(s_sram_path, "wb");
    if (f == NULL) {
        printf("[libretro] cannot write SRAM %s\n", s_sram_path);
        return;
    }
    fwrite(sram, 1, size, f);
    fclose(f);
}

void pc_libretro_unload(void) {
    if (s_dll == NULL) return;
    if (s_game_loaded) {
        pc_libretro_save_sram();
        p_retro_unload_game();
        s_game_loaded = 0;
    }
    p_retro_deinit();
    SDL_UnloadObject(s_dll);
    s_dll = NULL;
    free(s_frame);
    s_frame = NULL;
    s_have_frame = 0;
}

int pc_libretro_loaded(void) {
    return s_game_loaded;
}
