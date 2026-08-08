/* pc_mod_arena.h - allocator for resident mod assets (P5).
 *
 * Mod geometry, textures and profiles live here for the process lifetime.
 * Deliberately NOT ARAM and NOT the scene object-exchange heap:
 *
 *   - ARAM (pc/src/pc_aram.c) is a 16 MB bump allocator whose free is a no-op,
 *     so anything placed there is permanent and competes with the game's own
 *     resource budget.
 *   - The scene heap is bounded by max_ram_address and reset per scene.
 *
 * Keeping mod content out of both means a mod cannot cause an out-of-ARAM
 * failure in unrelated game code, which would be a very hard bug to attribute.
 *
 * IMPORTANT: the backing memory must come from the high arena
 * (>= 0x10000000, the VirtualAlloc/mmap region described in pc/DOCUMENTATION.md),
 * not plain malloc. emu64::seg2k0 distinguishes N64 segment addresses from host
 * pointers by range, so a low heap pointer inside a display list can be misread
 * as a segment address. pc_mod_arena_init enforces this and refuses a low
 * mapping rather than handing back memory that renders as garbage.
 */
#ifndef PC_MOD_ARENA_H
#define PC_MOD_ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimum address the arena may occupy. Below this, emu64 may mistake a
 * pointer for an N64 segment address. */
#define PC_MOD_ARENA_MIN_ADDRESS ((size_t)0x10000000)

/* Reserves `bytes`. Returns 0 if the reservation failed or landed too low, in
 * which case every allocation returns NULL and the caller renders placeholders.
 * A mod arena that cannot be created must not stop the game. */
int pc_mod_arena_init(size_t bytes);

/* Bump-allocates `bytes` aligned to `align` (a power of two). NULL when full --
 * checked, never assumed, because the caller is about to build a display list
 * out of it. */
void* pc_mod_arena_alloc(size_t bytes, size_t align);

size_t pc_mod_arena_used(void);
size_t pc_mod_arena_capacity(void);

/* Releases everything. Only at shutdown: mod assets are referenced by display
 * lists and profiles that live as long as the process. */
void pc_mod_arena_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_ARENA_H */
