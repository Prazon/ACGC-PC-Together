/* Checks the mod item registry against the REAL furniture table.
 *
 * This links src/data/furniture/ftr_profile_table.c, so it exercises the actual
 * 1266-entry table the game reads, the actual growth path, and the actual
 * pointer store -- not a mock. That matters because the whole point of P6 is
 * that stock reads keep working unchanged, and only the real table can show
 * that.
 *
 * The behaviours asserted here are the ones whose failure is silent: a stock
 * item quietly resolving to the wrong profile, a swap becoming visible before
 * the safe point, or a reset shrinking the table under an item already placed
 * in a room.
 */

#include "ac_furniture.h"
#include "m_ftr_def.h"
#include "m_ftr_profile_table.h"
#include "pc_mod_arena.h"
#include "pc_mod_registry.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* Stand-ins. Only their addresses matter here. */
static aFTR_PROFILE placeholder_a;
static aFTR_PROFILE placeholder_b;
static aFTR_PROFILE real_a;

int main(void) {
    uint16_t handles[2] = { 0x3001, 0x3002 };
    aFTR_PROFILE* bases[2] = { &placeholder_a, &placeholder_b };
    aFTR_PROFILE* before_stock;
    size_t stock_count;
    uint16_t slot_a;
    uint16_t slot_b;

    expect(pc_mod_arena_init(1024 * 1024) == 1, "arena opens");

    /* The table starts as the stock one. */
    stock_count = furniture_quality_count;
    expect(stock_count == FTR_NUM, "table starts at FTR_NUM");
    before_stock = furniture_quality[FTR_NUM - 1];
    expect(before_stock != NULL, "a stock entry is readable before growth");

    expect(pc_mod_registry_install(handles, bases, 2) == 2, "two items install");
    expect(furniture_quality_count == stock_count + 2, "table grew by two");

    /* Growth must not disturb a single stock entry -- this is the property the
     * ~11 unchecked readers depend on. */
    expect(furniture_quality[FTR_NUM - 1] == before_stock, "stock entry survives growth");
    expect(furniture_quality[0] != NULL, "first stock entry still readable");

    slot_a = pc_mod_registry_slot(handles[0]);
    slot_b = pc_mod_registry_slot(handles[1]);
    expect(slot_a == FTR_NUM, "first mod item sits just past the stock table");
    expect(slot_b == FTR_NUM + 1, "second follows it");
    expect(pc_mod_registry_slot(0x9999) == 0, "an unknown handle has no slot");

    /* Each item gets its OWN placeholder, not the first one's. */
    expect(furniture_quality[slot_a] == &placeholder_a, "item A shows its placeholder");
    expect(furniture_quality[slot_b] == &placeholder_b, "item B shows its placeholder");
    expect(!pc_mod_registry_is_resident(handles[0]), "nothing is resident yet");

    /* Marking resident must NOT take effect immediately: a swap under a live
     * actor risks a torn read of the profile the catalogue cached. */
    expect(pc_mod_registry_mark_resident(handles[0], &real_a) == 1, "mark succeeds");
    expect(furniture_quality[slot_a] == &placeholder_a,
           "the swap is not visible before the safe point");
    expect(!pc_mod_registry_is_resident(handles[0]), "and is not reported resident yet");

    expect(pc_mod_registry_apply_pending() == 1, "one swap applied at the safe point");
    expect(furniture_quality[slot_a] == &real_a, "item A now resolves to its real profile");
    expect(pc_mod_registry_is_resident(handles[0]), "item A is resident");
    /* Its neighbour is untouched. */
    expect(furniture_quality[slot_b] == &placeholder_b, "item B still shows its placeholder");
    expect(pc_mod_registry_apply_pending() == 0, "a second apply is a no-op");

    /* Bad input is refused rather than half-applied. */
    expect(pc_mod_registry_mark_resident(0x9999, &real_a) == 0, "unknown handle refused");
    expect(pc_mod_registry_mark_resident(handles[1], NULL) == 0, "null profile refused");
    expect(furniture_quality[slot_b] == &placeholder_b, "B untouched by refused marks");

    /* Duplicate handles are refused outright: two items sharing a slot would
     * make one silently render as the other. */
    {
        uint16_t dupes[2] = { 0x4001, 0x4001 };
        const size_t count_before = furniture_quality_count;
        pc_mod_registry_reset();
        expect(pc_mod_registry_install(dupes, bases, 2) == 0, "duplicate handles refused");
        expect(furniture_quality_count == count_before, "and nothing was grown");
    }

    /* reset() must not shrink the table. An item already placed in a room holds
     * an index into the grown part, and shrinking under it is exactly the wild
     * dereference this design exists to prevent. */
    {
        const size_t grown = furniture_quality_count;
        pc_mod_registry_reset();
        expect(furniture_quality_count == grown, "reset does not shrink the table");
        expect(pc_mod_registry_count() == 0, "but the registry is empty");
        expect(furniture_quality[FTR_NUM - 1] == before_stock, "stock entries still intact");
    }

    /* A table that cannot grow leaves everything exactly as it was. */
    {
        const size_t grown = furniture_quality_count;
        uint16_t many[1] = { 0x5001 };
        aFTR_PROFILE* one[1] = { &placeholder_a };
        /* Restore before the arena goes: a grown table lives in it, and leaving
         * furniture_quality pointing at unmapped memory segfaults on the next
         * read of ANY entry. The first version of this test crashed here, which
         * is how the missing restore was found. */
        ftr_profile_table_restore_base();
        pc_mod_arena_shutdown();
        expect(furniture_quality_count == FTR_NUM, "restore returns the table to stock");
        expect(furniture_quality[FTR_NUM - 1] == before_stock,
               "stock entries readable after the arena is gone");
        expect(pc_mod_registry_install(many, one, 1) == 0, "install fails without an arena");
        expect(furniture_quality_count == FTR_NUM, "table stays stock after a failed install");
        expect(furniture_quality[FTR_NUM - 1] == before_stock, "stock entries unchanged");
        (void)grown;
    }

    printf(failures == 0 ? "mod_registry: PASS\n" : "mod_registry: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
