/* The one definition of the furniture profile table.
 *
 * ac_furniture_profile_data.c_inc holds the 1266 entries and is included here
 * and nowhere else. It previously went into two translation units, giving each
 * a private static copy; see include/m_ftr_profile_table.h for why that had to
 * change before the table could grow at runtime.
 */

#include "ac_furniture.h"
#include "f_furniture.h"
#include "m_ftr_profile_table.h"
#include "m_ftr_tables.h"

#include "../src/actor/ac_furniture_profile_data.c_inc"

/* What every reader indexes. Repointed by the modloader when a town adds
 * furniture; otherwise it is the static table and costs nothing. */
aFTR_PROFILE** furniture_quality = furniture_quality_base;
size_t furniture_quality_count = FTR_NUM;
