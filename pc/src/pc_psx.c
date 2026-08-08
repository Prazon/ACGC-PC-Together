/* PS1 emulation glue — see pc_psx.h. Mirrors pc_nes_fixnes.c's GL and audio
 * integration, but sources frames from the libretro host instead of fixNES. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL2/SDL.h>
#include <glad/gl.h>

#include "pc_libretro_core.h"
#include "pc_psx.h"
#include "pc_settings.h"

/* From pc_gx.c — same contract as the NES emulator path. */
extern void pc_gx_restore_after_nes(void);
extern void pc_gx_draw_pending(void);
extern int g_pc_window_w;
extern int g_pc_window_h;

/* Game audio input (32kHz s16 stereo), same sink the NES path uses. */
extern void AIInitDMA(uintptr_t addr, uint32_t size);
#define PSX_OUTPUT_RATE 32000

/* ---------------------------------------------------------------------- */
/* Disc image scan                                                        */
/* ---------------------------------------------------------------------- */

static int psx_ext_ok(const char* name) {
    const char* dot = strrchr(name, '.');
    if (dot == NULL) return 0;
    return strcasecmp(dot, ".cue") == 0 || strcasecmp(dot, ".chd") == 0 ||
           strcasecmp(dot, ".iso") == 0;
}

static int psx_name_cmp(const void* a, const void* b) {
    return strcasecmp(*(const char* const*)a, *(const char* const*)b);
}

/* Pick the disc image: settings psx_game if present, else first (sorted). */
static int psx_find_game(char* out, int out_size) {
    const char* dir_path = g_pc_settings.psx_roms_dir;
    DIR* d = opendir(dir_path);
    if (d == NULL) {
        printf("[PSX] roms dir not found: %s\n", dir_path);
        return -1;
    }

    char** names = NULL;
    int count = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!psx_ext_ok(e->d_name)) continue;
        char** grown = (char**)realloc(names, sizeof(char*) * (count + 1));
        if (grown == NULL) break;
        names = grown;
        names[count] = strdup(e->d_name);
        if (names[count] != NULL) count++;
    }
    closedir(d);

    if (count == 0) {
        free(names);
        printf("[PSX] no .cue/.chd/.iso images in %s\n", dir_path);
        return -1;
    }
    qsort(names, count, sizeof(char*), psx_name_cmp);

    int pick = 0;
    if (g_pc_settings.psx_game[0] != '\0') {
        pick = -1;
        for (int i = 0; i < count; i++) {
            if (strcasecmp(names[i], g_pc_settings.psx_game) == 0) {
                pick = i;
                break;
            }
        }
        if (pick < 0) {
            printf("[PSX] psx_game \"%s\" not found in %s; using first image\n",
                   g_pc_settings.psx_game, dir_path);
            pick = 0;
        }
    }

    snprintf(out, out_size, "%s/%s", dir_path, names[pick]);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Disc list for the in-room picker                                       */
/* ---------------------------------------------------------------------- */

static char s_list_paths[PC_PSX_MAX_GAMES][600];
static char s_list_names[PC_PSX_MAX_GAMES][256];
static int s_list_count;

/* The game's font is ASCII-compatible for letters, digits, space and some
 * punctuation, but other ASCII codes map to accented glyphs and a few are
 * message-parser control bytes. Anything outside the safe set becomes a space. */
static int psx_title_char_ok(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    switch (c) {
        case ' ': case '!': case '"': case '%': case '&': case '\'':
        case '(': case ')': case ',': case '-': case '.': case ':':
        case '<': case '=': case '>': case '?': case '@': case '_':
            return 1;
        default:
            return 0;
    }
}

/* One 16-byte record: filename minus extension, sanitized, space-padded, no
 * terminator (the message layer trims trailing spaces only). */
static void psx_copy_title(char* dst, const char* filename) {
    int len = (int)strlen(filename);
    const char* dot = strrchr(filename, '.');
    int i;

    if (dot != NULL) len = (int)(dot - filename);
    if (len > 16) len = 16;

    for (i = 0; i < 16; i++) {
        unsigned char c = (i < len) ? (unsigned char)filename[i] : ' ';
        dst[i] = psx_title_char_ok(c) ? (char)c : ' ';
    }

    /* A stem that sanitizes away entirely would render as a blank, still
     * selectable row — label it so the player can tell what they are picking. */
    for (i = 0; i < 16; i++) {
        if (dst[i] != ' ') return;
    }
    memcpy(dst, "(disc)          ", 16);
}

/* Add one shelf entry if the file is really there. */
static void psx_list_add(const char* dir_path, const char* filename) {
    char path[600];
    FILE* f;

    if (s_list_count >= PC_PSX_MAX_GAMES) return;

    snprintf(path, sizeof(path), "%s/%s", dir_path, filename);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("[PSX] shelf entry not found, skipping: %s\n", path);
        return;
    }
    fclose(f);

    snprintf(s_list_paths[s_list_count], sizeof(s_list_paths[0]), "%s", path);
    snprintf(s_list_names[s_list_count], sizeof(s_list_names[0]), "%s", filename);
    s_list_count++;
}

/* Build the shelf from settings psx_games ('|'-separated filenames), keeping
 * the configured order. Returns the number of discs found. */
static int psx_list_from_settings(const char* dir_path) {
    const char* p = g_pc_settings.psx_games;

    while (*p != '\0') {
        char entry[256]; /* matches s_list_names, so no snprintf truncation */
        int len = 0;
        const char* start;
        const char* end;

        while (*p == ' ' || *p == '\t' || *p == '|') p++;
        start = p;
        while (*p != '\0' && *p != '|') p++;
        end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

        len = (int)(end - start);
        if (len > 0 && len < (int)sizeof(entry)) {
            memcpy(entry, start, (size_t)len);
            entry[len] = '\0';
            psx_list_add(dir_path, entry);
        }
    }
    return s_list_count;
}

int pc_psx_list_games(void) {
    const char* dir_path = g_pc_settings.psx_roms_dir;
    DIR* d;
    struct dirent* e;
    char** names = NULL;
    int count = 0;
    int i;

    s_list_count = 0;

    /* A configured shelf wins; otherwise offer the whole folder. */
    if (g_pc_settings.psx_games[0] != '\0') {
        return psx_list_from_settings(dir_path);
    }

    d = opendir(dir_path);
    if (d == NULL) {
        printf("[PSX] roms dir not found: %s\n", dir_path);
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        char** grown;
        if (!psx_ext_ok(e->d_name)) continue;
        grown = (char**)realloc(names, sizeof(char*) * (count + 1));
        if (grown == NULL) break;
        names = grown;
        names[count] = strdup(e->d_name);
        if (names[count] != NULL) count++;
    }
    closedir(d);

    if (count > 1) qsort(names, count, sizeof(char*), psx_name_cmp);

    for (i = 0; i < count; i++) {
        if (s_list_count < PC_PSX_MAX_GAMES) {
            snprintf(s_list_paths[s_list_count], sizeof(s_list_paths[0]), "%s/%s",
                     dir_path, names[i]);
            snprintf(s_list_names[s_list_count], sizeof(s_list_names[0]), "%s", names[i]);
            s_list_count++;
        }
        free(names[i]);
    }
    free(names);

    if (count > PC_PSX_MAX_GAMES) {
        printf("[PSX] %d discs found; listing the first %d\n", count, PC_PSX_MAX_GAMES);
    }
    return s_list_count;
}

int pc_psx_list_fill_titles(char* buf, int buf_size) {
    int max_records = buf_size / 16;
    int n = s_list_count < max_records ? s_list_count : max_records;
    int i;

    for (i = 0; i < n; i++) {
        psx_copy_title(buf + i * 16, s_list_names[i]);
    }
    return n;
}

/* ---------------------------------------------------------------------- */
/* Per-game console items                                                 */
/* ---------------------------------------------------------------------- */

/* One row per dedicated PlayStation furniture item, in furniture order.
 * Adding a game here is step 1 of 2 — the matching furniture entry lives in
 * src/furniture/ac_psx_game.c and the tables it lists. */
static const char* const s_item_discs[] = {
    "Crash Bandicoot (USA).cue",
    "Spyro the Dragon (USA).cue",
    "Tomba! (USA).cue",
    "Ape Escape (USA).cue",
};

int pc_psx_item_count(void) {
    return (int)(sizeof(s_item_discs) / sizeof(s_item_discs[0]));
}

const char* pc_psx_item_disc(int item_index) {
    if (item_index < 0 || item_index >= pc_psx_item_count()) return NULL;
    return s_item_discs[item_index];
}

/* Full path of the disc the next boot should load. */
static char s_selected_path[600];

void pc_psx_select_item(int item_index) {
    const char* disc = pc_psx_item_disc(item_index);

    s_selected_path[0] = '\0';
    if (disc == NULL) {
        printf("[PSX] no disc configured for console item %d\n", item_index);
        return;
    }
    snprintf(s_selected_path, sizeof(s_selected_path), "%s/%s",
             g_pc_settings.psx_roms_dir, disc);
}

void pc_psx_select_index(int list_index) {
    s_selected_path[0] = '\0';
    if (list_index < 0 || list_index >= s_list_count) {
        printf("[PSX] disc index %d out of range (%d listed)\n", list_index, s_list_count);
        return;
    }
    snprintf(s_selected_path, sizeof(s_selected_path), "%s", s_list_paths[list_index]);
}

int pc_psx_boot_selected(void) {
    FILE* f;

    if (s_selected_path[0] == '\0') return PC_PSX_ERR_NO_GAME;

    f = fopen(s_selected_path, "rb");
    if (f == NULL) {
        printf("[PSX] disc not found: %s\n", s_selected_path);
        return PC_PSX_ERR_NO_GAME;
    }
    fclose(f);

    if (pc_libretro_load(g_pc_settings.psx_core, g_pc_settings.psx_bios_dir, "save") != 0)
        return PC_PSX_ERR_NO_CORE;

    if (pc_libretro_load_game(s_selected_path) != 0) {
        pc_libretro_unload();
        return PC_PSX_ERR_BOOT;
    }
    printf("[PSX] booting %s\n", s_selected_path);
    return PC_PSX_OK;
}

int pc_psx_boot_index(int index) {
    if (index < 0 || index >= s_list_count) {
        printf("[PSX] disc index %d out of range (%d listed)\n", index, s_list_count);
        return PC_PSX_ERR_NO_GAME;
    }

    if (pc_libretro_load(g_pc_settings.psx_core, g_pc_settings.psx_bios_dir, "save") != 0)
        return PC_PSX_ERR_NO_CORE;

    if (pc_libretro_load_game(s_list_paths[index]) != 0) {
        pc_libretro_unload();
        return PC_PSX_ERR_BOOT;
    }
    printf("[PSX] booting %s\n", s_list_names[index]);
    return PC_PSX_OK;
}

/* ---------------------------------------------------------------------- */
/* Boot / frame / shutdown                                                */
/* ---------------------------------------------------------------------- */

int pc_psx_boot(void) {
    char game_path[600];
    if (psx_find_game(game_path, sizeof(game_path)) != 0)
        return PC_PSX_ERR_NO_GAME;

    if (pc_libretro_load(g_pc_settings.psx_core, g_pc_settings.psx_bios_dir, "save") != 0)
        return PC_PSX_ERR_NO_CORE;

    if (pc_libretro_load_game(game_path) != 0) {
        pc_libretro_unload();
        return PC_PSX_ERR_BOOT;
    }
    return PC_PSX_OK;
}

void pc_psx_frame(uint16_t buttons) {
    pc_libretro_set_input(buttons);
    pc_libretro_run_frame();

    /* Drain this frame's audio and resample to the game's 32kHz output,
     * same naive nearest-sample approach as the NES path. */
    static int16_t in_buf[8192 * 2];
    static int16_t out_buf[4096 * 2];
    int in_frames = pc_libretro_audio_take(in_buf, 8192);
    if (in_frames <= 0) return;

    double rate = pc_libretro_sample_rate();
    int out_frames = (int)((int64_t)in_frames * PSX_OUTPUT_RATE / (rate > 0 ? rate : 44100.0));
    if (out_frames > 4096) out_frames = 4096;
    if (out_frames <= 0) return;

    for (int i = 0; i < out_frames; i++) {
        int src = (int)((int64_t)i * in_frames / out_frames);
        if (src >= in_frames) src = in_frames - 1;
        out_buf[i * 2 + 0] = in_buf[src * 2 + 0];
        out_buf[i * 2 + 1] = in_buf[src * 2 + 1];
    }
    AIInitDMA((uintptr_t)out_buf, (uint32_t)out_frames * 4);
}

/* ---------------------------------------------------------------------- */
/* Rendering (mirrors fixnes_render_frame)                                */
/* ---------------------------------------------------------------------- */

static GLuint psx_shader, psx_vao, psx_vbo, psx_texture;
static GLint psx_tex_uniform = -1;

static GLuint psx_compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        printf("[PSX] shader error: %s\n", buf);
    }
    return s;
}

static void psx_init_gl(void) {
    const char* vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec2 uv;\n"
        "out vec2 v_uv;\n"
        "void main() { gl_Position = vec4(pos, 0, 1); v_uv = uv; }\n";
    const char* fs =
        "#version 330 core\n"
        "in vec2 v_uv;\n"
        "out vec4 fragColor;\n"
        "uniform sampler2D tex;\n"
        "void main() { fragColor = vec4(texture(tex, v_uv).rgb, 1.0); }\n";

    GLuint v = psx_compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = psx_compile_shader(GL_FRAGMENT_SHADER, fs);
    psx_shader = glCreateProgram();
    glAttachShader(psx_shader, v);
    glAttachShader(psx_shader, f);
    glLinkProgram(psx_shader);
    glDeleteShader(v);
    glDeleteShader(f);

    float quad[] = {
        -1, -1,  0, 1,
         1, -1,  1, 1,
         1,  1,  1, 0,
        -1, -1,  0, 1,
         1,  1,  1, 0,
        -1,  1,  0, 0,
    };
    glGenVertexArrays(1, &psx_vao);
    glGenBuffers(1, &psx_vbo);
    glBindVertexArray(psx_vao);
    glBindBuffer(GL_ARRAY_BUFFER, psx_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glBindVertexArray(0);

    psx_tex_uniform = glGetUniformLocation(psx_shader, "tex");

    glGenTextures(1, &psx_texture);
    glBindTexture(GL_TEXTURE_2D, psx_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void pc_psx_render(void) {
    pc_gx_draw_pending();
    if (!psx_shader) psx_init_gl();

    unsigned w, h, pitch;
    int fmt;
    const void* frame = pc_libretro_video(&w, &h, &pitch, &fmt);

    int win_w = g_pc_window_w;
    int win_h = g_pc_window_h;

    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (frame == NULL) return; /* nothing decoded yet — black frame */

    glBindTexture(GL_TEXTURE_2D, psx_texture);
    if (fmt == PC_LR_PIXFMT_XRGB8888) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 4));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, (GLsizei)w, (GLsizei)h, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frame);
    } else if (fmt == PC_LR_PIXFMT_RGB565) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 2));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)w, (GLsizei)h, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, frame);
    } else { /* 0RGB1555 */
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / 2));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)w, (GLsizei)h, 0,
                     GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, frame);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    /* 0 = stretch, 1 = centered 4:3 (same setting as the NES emulator). */
    int vp_w, vp_h, vp_x, vp_y;
    if (g_pc_settings.nes_aspect == 0) {
        vp_w = win_w; vp_h = win_h; vp_x = 0; vp_y = 0;
    } else if (win_w * 3 > win_h * 4) {
        vp_h = win_h;
        vp_w = (win_h * 4) / 3;
        vp_x = (win_w - vp_w) / 2;
        vp_y = 0;
    } else {
        vp_w = win_w;
        vp_h = (win_w * 3) / 4;
        vp_x = 0;
        vp_y = (win_h - vp_h) / 2;
    }

    glViewport(vp_x, vp_y, vp_w, vp_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(psx_shader);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, psx_texture);
    glUniform1i(psx_tex_uniform, 0);
    glBindVertexArray(psx_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glViewport(0, 0, win_w, win_h);
}

void pc_psx_save(void) {
    pc_libretro_save_sram();
}

void pc_psx_shutdown(void) {
    pc_libretro_unload();
    pc_gx_restore_after_nes();
}
