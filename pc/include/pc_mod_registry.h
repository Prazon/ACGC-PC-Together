/* pc_mod_registry.h - binds mod item handles to furniture table slots (P6).
 *
 * Three states per mod item, and the whole design is about moving safely
 * between them:
 *
 *   not a mod item   -> the stock table answers, nothing here is consulted
 *   placeholder      -> the slot holds its `base` vanilla profile
 *   resident         -> the slot holds the mod's real profile
 *
 * The transition placeholder -> resident is a **single pointer store into
 * furniture_quality[slot]**. That matters because m_catalog_ovl.c and
 * ac_my_room.c both read that table without a bounds check, and
 * mCL_dma_furniture_program caches the pointer it reads into
 * mCL_Item_c::profile and dereferences it on a later line. A reader therefore
 * sees either the placeholder or the finished profile, never a half-built one.
 *
 * Three defences, because one is not enough:
 *
 *   1. Publish atomically. The profile is fully populated in the mod arena
 *      before its pointer is stored.
 *   2. Publish at safe points only. Swaps are queued and applied by
 *      pc_mod_registry_apply_pending, which the caller runs at a scene
 *      transition or when the submenu closes -- both points where nothing is
 *      holding a cached profile.
 *   3. Never free a base profile. It is static, so even a stale cached pointer
 *      stays valid; the item simply keeps rendering as its placeholder until
 *      something re-reads. That is what makes the worst case cosmetic.
 */
#ifndef PC_MOD_REGISTRY_H
#define PC_MOD_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A town may declare more items than this only by raising it; the cap exists so
 * a manifest cannot make the client grow an unbounded table. */
#define PC_MOD_REGISTRY_MAX_ITEMS 256

struct ftr_profile_s;

/* Grows the furniture table by `count` slots and records a handle for each.
 * Every new slot starts as `base_profiles[i]`, the vanilla profile the item
 * renders as until its asset arrives. Returns the number registered; 0 means
 * the table could not grow and no mod item exists (placeholders everywhere,
 * never a failure). Call once, after the arena is up. */
size_t pc_mod_registry_install(const uint16_t* handles,
                               struct ftr_profile_s* const* base_profiles,
                               size_t count);

/* The furniture index a handle occupies, or 0 if unknown. Mod items live above
 * FTR_NUM, so 0 is never a valid mod slot. */
uint16_t pc_mod_registry_slot(uint16_t handle);

/* Queues `profile` to replace the handle's placeholder. Takes effect at the
 * next pc_mod_registry_apply_pending, not immediately -- see the header comment
 * for why. Returns 0 for an unknown handle or a null profile. */
int pc_mod_registry_mark_resident(uint16_t handle, struct ftr_profile_s* profile);

/* Applies queued swaps. Call at a scene transition or when the submenu closes.
 * Returns how many were applied. */
size_t pc_mod_registry_apply_pending(void);

/* 1 once the handle's real profile is live. */
int pc_mod_registry_is_resident(uint16_t handle);

size_t pc_mod_registry_count(void);
void pc_mod_registry_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_REGISTRY_H */
