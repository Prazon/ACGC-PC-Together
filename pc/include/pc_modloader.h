/* pc_modloader.h - Client-side mod discovery and asset override (T1).
 *
 * Scans mods/<id>/overrides/ for files whose names match entries in the
 * generated asset table (pc/src/pc_assets.c) and serves them in place of the
 * disc's copy.
 *
 * This is the override tier only. It replaces existing assets; it cannot add
 * new ones, because every destination is a fixed-size array declared at compile
 * time. See docs/MODLOADER_PLAN.md for the tiers above this.
 *
 * Deliberately independent of the server-side mod framework: a client may
 * install overrides for a town that runs no Lua at all, and a town's Lua mods
 * do not require the player to install anything.
 */
#ifndef PC_MODLOADER_H
#define PC_MODLOADER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scans the mods directory. Call once, before pc_assets_init. Returns the
 * number of overrides registered; 0 is the normal case. */
int pc_modloader_init(void);

/* An override for `bin_path` (the asset table's own name for the asset), or
 * NULL. `expect_size` must match exactly: the destination is a fixed-size
 * array, so a differently-sized replacement is a packaging error rather than
 * something to truncate into place. A mismatch is reported and refused. */
const void* pc_modloader_override(const char* bin_path, unsigned int expect_size);

/* Grows the furniture profile table by `extra` entries, so mod items can be
 * indexed by the same unchecked reads the stock ones are
 * (docs/MODLOADER_PLAN.md sec 7.5, Option A).
 *
 * The new entries start as `base_profile` -- the placeholder an item renders as
 * until its real asset is resident. Callers then publish real profiles into the
 * grown slots with a single pointer store, so a reader sees either the
 * placeholder or the finished profile and never a half-built one.
 *
 * Returns 0 if the arena could not supply the memory, in which case the table
 * is left exactly as it was and no mod item exists. Never partially grows.
 *
 * This is possible at all because the table has one definition
 * (src/data/furniture/ftr_profile_table.c). While it was a `static` inside a
 * .c_inc included by two translation units, each had a private copy and the
 * symbol could not be repointed. */
int pc_modloader_grow_furniture(size_t extra, const void* base_profile);

void pc_modloader_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MODLOADER_H */
