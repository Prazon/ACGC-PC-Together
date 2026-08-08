#ifndef M_FTR_TABLES_H
#define M_FTR_TABLES_H

/* Build-time length checks for the furniture tables that are indexed by the
 * `enum ftr_name` value.
 *
 * Most such tables are declared `[FTR_NUM]`, so the declaration already
 * guarantees the length and a short initialiser merely zero-fills. Two are
 * declared `[]` and take their length from the initialiser instead:
 *
 *   furniture_quality[]  (src/actor/ac_furniture_profile_data.c_inc)
 *       Indexed with no bounds check -- m_catalog_ovl.c:170 does
 *           item->profile = furniture_quality[item->ftr_actor.name];
 *       and dereferences profile->vtable on the next line. A short table is a
 *       wild pointer dereference, not a missing item.
 *
 *   ftr_price_table[]    (src/data/item/ftr_price.c)
 *       Walked to a trailing -1 sentinel by mSP_CountPriceTableElement, so it
 *       is length FTR_NUM + 1 and reads are bounds-checked. A short table is
 *       therefore not unsafe, but it silently prices furniture at 0.
 *
 * Adding furniture means appending to both, after FTR_DUMMY. These assertions
 * turn "forgot one" from a runtime corruption into a build error.
 */

#include "m_ftr_def.h"

#define FTR_TABLE_ASSERT_N(tbl, expect)                                     \
    _Static_assert(sizeof(tbl) / sizeof((tbl)[0]) == (expect),              \
                   #tbl " must have exactly " #expect " entries -- see include/m_ftr_tables.h")

/* Exactly one entry per `enum ftr_name` value. */
#define FTR_TABLE_ASSERT(tbl) FTR_TABLE_ASSERT_N(tbl, FTR_NUM)

/* One entry per furniture, plus the trailing -1 sentinel. */
#define FTR_TABLE_ASSERT_SENTINEL(tbl) FTR_TABLE_ASSERT_N(tbl, FTR_NUM + 1)

#endif /* M_FTR_TABLES_H */
