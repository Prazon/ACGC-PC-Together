/* pc_mod_loadscreen.c - optional download progress UI (P8).
 *
 * Deliberately optional. docs/MODLOADER_PLAN.md sec 8.1 is the governing rule:
 * a missing asset never blocks and never fails, so the player is already in
 * their town before the first chunk arrives. This screen exists for people who
 * would rather wait than watch placeholders fill in, and "Play now" is always
 * present and always works.
 *
 * A join gate would turn every network hiccup and every oversized pack into a
 * player who cannot get into their town. That is the failure mode this file
 * exists to avoid, not to implement.
 *
 * Drawn with the PC menu stack (docs/UI_SYSTEMS.md sec 7): pc_text_draw into
 * font_thaga, pc_menu_util for layout, from graph_main next to the pause menu.
 * No decomp change.
 */
#ifdef TARGET_PC

#include "pc_mod_loadscreen.h"

#include "pc_menu_util.h"
#include "pc_mod_fetch.h"
#include "pc_text_draw.h"

#include "m_font.h"
#include "main.h"      /* SCREEN_WIDTH_F */

#include <stdio.h>

/* Offered only when the wait is worth mentioning. Below this the placeholders
 * fill in fast enough that a screen would be more disruptive than the thing it
 * is reporting. */
#define PC_LOADSCREEN_MIN_BYTES (2u * 1024u * 1024u)
#define PC_LOADSCREEN_MIN_ASSETS 20

int g_pc_loadscreen_visible = 0;

static int s_offered;

void pc_mod_loadscreen_reset(void) {
    g_pc_loadscreen_visible = 0;
    s_offered = 0;
}

void pc_mod_loadscreen_consider(void) {
    if (s_offered) return;
    s_offered = 1;
    /* Shown at most once per join, and only when there is a real wait. */
    if (pc_mod_fetch_bytes_total() >= PC_LOADSCREEN_MIN_BYTES ||
        pc_mod_fetch_outstanding() >= PC_LOADSCREEN_MIN_ASSETS) {
        g_pc_loadscreen_visible = 1;
    }
}

void pc_mod_loadscreen_dismiss(void) { g_pc_loadscreen_visible = 0; }

int pc_mod_loadscreen_handle_event(const SDL_Event* event) {
    if (!g_pc_loadscreen_visible) return 0;
    if (event->type == SDL_KEYDOWN && !event->key.repeat) {
        /* Any of the usual confirm/cancel keys dismisses. There is nothing to
         * choose between: the only action is to stop watching. */
        switch (event->key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
            case SDLK_ESCAPE:
                pc_mod_loadscreen_dismiss();
                return 1;
            default:
                break;
        }
        return 1;   /* swallow the rest while it is up */
    }
    if (event->type == SDL_CONTROLLERBUTTONDOWN) {
        pc_mod_loadscreen_dismiss();
        return 1;
    }
    return 0;
}

static void draw_bar(struct game_s* game, f32 y, f32 fraction) {
    /* A text bar rather than a textured one: it needs no asset, which matters
     * for a screen whose entire job is to appear before assets exist. */
    char bar[34];
    const int cells = 30;
    int filled = (int)((f32)cells * fraction + 0.5f);
    int i;

    if (filled < 0) filled = 0;
    if (filled > cells) filled = cells;
    bar[0] = '[';
    for (i = 0; i < cells; i++) bar[1 + i] = (i < filled) ? '=' : '-';
    bar[1 + cells] = ']';
    bar[2 + cells] = '\0';
    pc_menu_draw_centered(game, bar, y, 220, 220, 220, 255, 1.0f);
}

void pc_mod_loadscreen_draw(struct game_s* game) {
    char line[64];
    uint64_t total;
    uint64_t done;

    if (!g_pc_loadscreen_visible || game == NULL || game->graph == NULL) return;

    /* Close itself the moment there is nothing left, so a player who chose to
     * wait is not left staring at a finished bar. */
    if (pc_mod_fetch_outstanding() == 0) {
        pc_mod_loadscreen_dismiss();
        return;
    }

    total = pc_mod_fetch_bytes_total();
    done = pc_mod_fetch_bytes_done();

    mFont_SetMatrix(game->graph, mFont_MODE_FONT);
    pc_menu_dim_rect(game->graph, 180);

    pc_menu_draw_centered(game, "Downloading town content...", 88.0f, 255, 255, 255, 255, 1.0f);
    draw_bar(game, 112.0f, (total > 0) ? (f32)((double)done / (double)total) : 0.0f);

    snprintf(line, sizeof(line), "%.1f / %.1f MB",
             (double)done / (1024.0 * 1024.0), (double)total / (1024.0 * 1024.0));
    pc_menu_draw_centered(game, line, 132.0f, 200, 200, 200, 255, 1.0f);

    /* Always present, always works. The whole point of the screen being
     * optional is that this is not a courtesy. */
    pc_menu_draw_centered(game, "Play now", 160.0f, 255, 235, 120, 255, PC_MENU_SCALE_SELECTED);
    pc_menu_draw_centered(game, "(items appear as they arrive)", 178.0f, 170, 170, 170, 255, 1.0f);

    mFont_UnSetMatrix(game->graph, mFont_MODE_FONT);
}

#endif /* TARGET_PC */
