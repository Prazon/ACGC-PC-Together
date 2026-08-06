/* pc_mouse.h - Mouse input for menus and other 2D interfaces.
 *
 * Only the declarations live here so decompiled game code can include this
 * without pulling in SDL. pc_mouse.c owns the SDL side.
 *
 * All state is sampled once per frame by pc_mouse_update(), which runs at the
 * end of pc_platform_poll_events(); everything below reports that snapshot. */
#ifndef PC_MOUSE_H
#define PC_MOUSE_H

#include "pc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Button bitflags returned by pc_mouse_button_*(). */
#define PC_MOUSE_BUTTON_LEFT    (1 << 0)
#define PC_MOUSE_BUTTON_MIDDLE  (1 << 1)
#define PC_MOUSE_BUTTON_RIGHT   (1 << 2)
#define PC_MOUSE_BUTTON_X1      (1 << 3)
#define PC_MOUSE_BUTTON_X2      (1 << 4)
#define PC_MOUSE_WHEEL_UP       (1 << 5)
#define PC_MOUSE_WHEEL_DOWN     (1 << 6)

#ifdef MOUSE_INPUT

/* Wheel accumulator. The SDL event loop adds SDL_MOUSEWHEEL deltas here;
 * pc_mouse_update() latches and clears it. */
extern s32 g_mouse_wheel_delta;

u32 pc_mouse_button_held(void);
u32 pc_mouse_button_pressed(void);
u32 pc_mouse_button_released(void);
void pc_mouse_lock(s32 lock);
int pc_mouse_is_locked(void);
s32 pc_mouse_scroll_wheel(void);
void pc_mouse_get_position(s32* x, s32* y);
void pc_mouse_get_delta(s32* dx, s32* dy);
int pc_mouse_moved(void);
int pc_mouse_active(void);
void pc_mouse_get_native_position(s32* x, s32* y);
void pc_mouse_update(void);

#else

/* Mouse-less build: everything folds away. */
#define g_mouse_wheel_delta 0

static inline u32 pc_mouse_button_held(void) { return 0; }
static inline u32 pc_mouse_button_pressed(void) { return 0; }
static inline u32 pc_mouse_button_released(void) { return 0; }
static inline void pc_mouse_lock(s32 lock) { (void)lock; }
static inline int pc_mouse_is_locked(void) { return 0; }
static inline s32 pc_mouse_scroll_wheel(void) { return 0; }
static inline void pc_mouse_get_position(s32* x, s32* y) { if (x) *x = 0; if (y) *y = 0; }
static inline void pc_mouse_get_delta(s32* dx, s32* dy) { if (dx) *dx = 0; if (dy) *dy = 0; }
static inline int pc_mouse_moved(void) { return 0; }
static inline int pc_mouse_active(void) { return 0; }
static inline void pc_mouse_get_native_position(s32* x, s32* y) { if (x) *x = 0; if (y) *y = 0; }
static inline void pc_mouse_update(void) {}

#endif /* MOUSE_INPUT */

#ifdef __cplusplus
}
#endif

#endif /* PC_MOUSE_H */
