/* pc_mod_registry.c - see pc_mod_registry.h */
#ifdef TARGET_PC

#include "pc_mod_registry.h"

#include "pc_modloader.h"

#include "ac_furniture.h"
#include "m_ftr_def.h"
#include "m_ftr_profile_table.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t handle;
    uint16_t slot;              /* index into furniture_quality */
    aFTR_PROFILE* base;         /* the placeholder; never freed, so never stale */
    aFTR_PROFILE* pending;      /* queued real profile, or NULL */
    int resident;
} ModItem;

static ModItem g_items[PC_MOD_REGISTRY_MAX_ITEMS];
static size_t g_count;

static ModItem* find(uint16_t handle) {
    size_t i;
    for (i = 0; i < g_count; i++) {
        if (g_items[i].handle == handle) return &g_items[i];
    }
    return NULL;
}

size_t pc_mod_registry_install(const uint16_t* handles, aFTR_PROFILE* const* base_profiles,
                               size_t count) {
    size_t first_slot;
    size_t i;

    if (!handles || !base_profiles || count == 0) return 0;
    if (count > PC_MOD_REGISTRY_MAX_ITEMS) count = PC_MOD_REGISTRY_MAX_ITEMS;

    /* Duplicate handles would give two items the same slot, so they are
     * rejected before anything is grown -- a half-installed set is worse than
     * none. */
    for (i = 0; i < count; i++) {
        size_t j;
        if (handles[i] == 0 || base_profiles[i] == NULL) {
            fprintf(stderr, "[Mods] item %zu has no handle or no base profile; nothing installed\n", i);
            return 0;
        }
        for (j = i + 1; j < count; j++) {
            if (handles[i] == handles[j]) {
                fprintf(stderr, "[Mods] duplicate item handle %u; nothing installed\n",
                        (unsigned)handles[i]);
                return 0;
            }
        }
    }

    first_slot = furniture_quality_count;
    if (!pc_modloader_grow_furniture(count, base_profiles[0])) return 0;

    for (i = 0; i < count; i++) {
        g_items[i].handle = handles[i];
        g_items[i].slot = (uint16_t)(first_slot + i);
        g_items[i].base = base_profiles[i];
        g_items[i].pending = NULL;
        g_items[i].resident = 0;
        /* grow_furniture filled every new slot with base_profiles[0]; give each
         * its own placeholder now that the table exists. */
        furniture_quality[first_slot + i] = base_profiles[i];
    }
    g_count = count;
    return count;
}

uint16_t pc_mod_registry_slot(uint16_t handle) {
    const ModItem* item = find(handle);
    return item ? item->slot : 0;
}

int pc_mod_registry_mark_resident(uint16_t handle, aFTR_PROFILE* profile) {
    ModItem* item = find(handle);
    if (!item || !profile) return 0;
    /* Queued, not applied. A swap under a live actor risks a torn read of the
     * profile the catalogue cached; apply_pending runs where nothing holds
     * one. */
    item->pending = profile;
    return 1;
}

size_t pc_mod_registry_apply_pending(void) {
    size_t applied = 0;
    size_t i;

    for (i = 0; i < g_count; i++) {
        if (g_items[i].pending == NULL) continue;
        /* The single store the whole design turns on. The profile it publishes
         * was fully populated before this point, so a reader sees either the
         * placeholder or the finished thing. */
        furniture_quality[g_items[i].slot] = g_items[i].pending;
        g_items[i].pending = NULL;
        g_items[i].resident = 1;
        applied++;
    }
    return applied;
}

int pc_mod_registry_is_resident(uint16_t handle) {
    const ModItem* item = find(handle);
    return item ? item->resident : 0;
}

size_t pc_mod_registry_count(void) { return g_count; }

void pc_mod_registry_reset(void) {
    /* Deliberately does not shrink furniture_quality. The arena is not freed
     * until shutdown, and an item that has already been placed in a room holds
     * an index into the grown table -- shrinking under it would be exactly the
     * wild dereference the whole table-growth design exists to avoid. */
    memset(g_items, 0, sizeof(g_items));
    g_count = 0;
}

#endif /* TARGET_PC */
