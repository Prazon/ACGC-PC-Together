/* pc_mod_loadscreen.h - optional download progress UI (P8).
 *
 * Optional by design. A missing asset never blocks a join
 * (docs/MODLOADER_PLAN.md sec 8.1), so this is for players who would rather
 * wait than watch placeholders fill in. "Play now" is always available.
 */
#ifndef PC_MOD_LOADSCREEN_H
#define PC_MOD_LOADSCREEN_H

#include "pc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

struct game_s;

extern int g_pc_loadscreen_visible;

/* Call once after a manifest has been diffed. Shows the screen only if the
 * remaining download is large enough to be worth mentioning. */
void pc_mod_loadscreen_consider(void);

void pc_mod_loadscreen_dismiss(void);
void pc_mod_loadscreen_reset(void);

/* 1 if consumed. */
int pc_mod_loadscreen_handle_event(const SDL_Event* event);

/* Appends to the font display list. Dismisses itself when nothing is
 * outstanding. */
void pc_mod_loadscreen_draw(struct game_s* game);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_LOADSCREEN_H */
