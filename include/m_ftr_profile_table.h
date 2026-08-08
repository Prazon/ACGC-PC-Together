#ifndef M_FTR_PROFILE_TABLE_H
#define M_FTR_PROFILE_TABLE_H

/* The furniture profile table, as a single definition.
 *
 * It used to be a `static` array inside ac_furniture_profile_data.c_inc, which
 * is #included into two translation units (ac_my_room.c, via
 * ac_furniture_data.c_inc, and m_catalog_ovl.c). Each therefore carried its own
 * ~10 KB copy of an identical table -- harmless, but it also made the table
 * impossible to grow at runtime: the moment the symbol stops being `static` so
 * it can be repointed, two definitions of one global become a duplicate-symbol
 * link error.
 *
 * Defining it once in src/data/furniture/ftr_profile_table.c and declaring it
 * here removes both problems. `furniture_quality` is now a pointer, so
 * pc_mod_tables_grow can swap in a larger copy at load time and every existing
 * `furniture_quality[i]` read keeps working untouched -- which is the whole
 * point of the approach (docs/MODLOADER_PLAN.md sec 7.5, Option A).
 *
 * Nothing indexed sizeof() or ARRAY_COUNT() on it, so the change from array to
 * pointer is invisible to every reader.
 */

#include "ac_furniture.h"
#include "m_ftr_def.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Points at the static base table until a mod grows it. A build with no mods
 * never reallocates and pays nothing. Indexed without bounds checks
 * (m_catalog_ovl.c, ac_my_room.c), so its length is asserted at build time --
 * see include/m_ftr_tables.h. */
extern aFTR_PROFILE** furniture_quality;
extern size_t furniture_quality_count;

#ifdef __cplusplus
}
#endif

#endif /* M_FTR_PROFILE_TABLE_H */
