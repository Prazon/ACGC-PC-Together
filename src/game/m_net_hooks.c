#include "m_net_hooks.h"

#ifdef NETCODE_ENABLED

#include "ac_net_remote_player.h"
#include "ac_my_room.h"
#include "acnet/c_api.h"
#include "m_common_data.h"
#include "m_demo.h"
#include "m_field_info.h"
#include "m_field_make.h"
#include "m_kankyo.h"
#include "m_name_table.h"
#include "m_notice.h"
#include "m_npc.h"
#include "m_time.h"
#include "m_collision_bg.h"
#include "m_home_h.h"
#include "m_mail.h"
#include "m_private.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_scene_table.h"
#include "m_submenu.h"
#include "m_submenu_ovl.h"
#include "m_hand_ovl.h"
#include "m_room_type.h"
#include "padmgr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Island zone and door identifiers. These must match acnet::kIslandZone and
 * friends in net/include/acnet/zone.hpp -- the C boundary carries plain
 * integers, so the two tables are kept in step by hand. */
#define NET_ZONE_ISLAND 300
#define NET_ZONE_ISLAND_CABIN 301
#define NET_ZONE_ISLAND_NPC_HOUSE 302
#define NET_DOOR_FERRY_TO_ISLAND 60
#define NET_DOOR_FERRY_TO_TOWN 61
#define NET_DOOR_CABIN_ENTER 70
#define NET_DOOR_CABIN_LEAVE 71
#define NET_DOOR_ISLAND_HUT_ENTER 80
#define NET_DOOR_ISLAND_HUT_LEAVE 81
/* acnet::kSharedHouseSlot: the original_slot a house with no resident owner
 * reports, which today is only the island cabin. */
#define NET_SHARED_HOUSE_SLOT 0xFF

static int last_status = -1;
/* Counts whole baselines, so a delta of any other kind no longer forces the
 * entire interest chunk to be rewritten. Monotonic across zones, which is why
 * the town and island no longer need separate watermarks: crossing between them
 * always arrives on a serial this has not seen. */
static u32 last_baseline_serial = 0;
static u32 last_inventory_revision = 0;
/* The bank ledger moves on its own: a deposit changes the inventory too, but an
 * operator gift changes only the balance, so it needs its own watermark. */
static u32 last_ledger_revision = 0;
/* Mail likewise: a letter arriving touches neither the inventory nor the bank. */
static u32 last_mail_revision = 0;
/* Which authoritative letter each projected array slot holds, so a UI action on
 * slot N can name the letter the server knows about. */
static u64 projected_mailbox_ids[HOME_MAILBOX_SIZE];
static u64 projected_carried_ids[mPr_INVENTORY_MAIL_COUNT];
static u32 last_house_revision = 0;
static u32 last_shop_revision = 0;
static u64 last_house_id = 0;
static u64 house_candidate_hash = 0;
static u64 house_submitted_hash = 0;
static int house_candidate_frames = 0;
static int house_update_pending = FALSE;
/* A successful optimistic update has a transaction result before the
 * resulting baseline reaches this client. Keep the predicted room intact
 * during that gap; applying the previous revision can tear down the
 * furniture actor while the original PUSH/PULL is still settling. */
static u32 house_wait_authoritative_revision = 0;
static int house_reconcile_delay_frames = 0;
static int force_house_reconcile = FALSE;
static AcNetHouseFurniture house_furniture[3 * 4 * 16 * 16];
static mActor_name_t house_authoritative_items[mHm_ROOM_NUM][mCoBG_LAYER_NUM][UT_Z_NUM][UT_X_NUM];
typedef struct net_furniture_motion_s {
    int valid;
    u8 floor;
    u8 layer;
    u8 source_x;
    u8 source_z;
    u8 destination_x;
    u8 destination_z;
} NetFurnitureMotion;
static NetFurnitureMotion house_motion;
/* The four resident gyroids are projections of authoritative state. The two
 * hashes watch the local player's own block (items and message only -- bells
 * move exclusively through the Take/Collect transactions): projected is what
 * the last projection wrote, submitted is what was last sent, and a block that
 * matches neither is an edit the owner made in the submenu and settles as one
 * whole-gyroid update when it closes. */
static u32 last_gyroid_serial = 0;
static u64 gyroid_projected_hash = 0;
static u64 gyroid_submitted_hash = 0;
static u32 pending_destination_zone = 0;
static int zone_transfer_phase = 0;
static u64 pending_transfer_token_high = 0;
static u64 pending_transfer_token_low = 0;
static int zone_arrival_stream_frames = 0;
static int zone_retry_frames = 0;
static int gameplay_ready_frames = 0;
static int gameplay_ready = FALSE;
static int gameplay_ready_reported = FALSE;
static int appearance_sent = FALSE;
static AcNetPlayerAppearance last_sent_appearance;
static int encounter_pending = 0;
/* Hold swaps awaiting a result. While any is outstanding the local hand and the
 * authoritative one are expected to disagree, and reconciling that disagreement
 * again would issue a second swap. */
static int hold_requests_pending = 0;
static int quickstart_enabled = FALSE;
static int quickstart_gender = mPr_SEX_MALE;
static u8 quickstart_name[PLAYER_NAME_LEN];

static u32 Net_SceneZone(int scene);

static int Net_CaptureAppearance(AcNetPlayerAppearance* appearance) {
    int design;
    if (appearance == NULL || Now_Private == NULL) return FALSE;
    memset(appearance, 0, sizeof(*appearance));
    memcpy(appearance->name, Now_Private->player_ID.player_name, sizeof(appearance->name));
    appearance->gender = Now_Private->gender;
    appearance->face = Now_Private->face;
    appearance->clothing = Now_Private->cloth.item;
    appearance->clothing_index = Now_Private->cloth.idx;
    design = (int)Now_Private->cloth.idx - (CLOTH_NUM + 1);
    if (mPr_ORIGINAL_DESIGN_IDX_VALID(design)) {
        appearance->pattern_present = TRUE;
        appearance->pattern_palette = Now_Private->my_org[design].palette;
        memcpy(appearance->pattern_texture, Now_Private->my_org[design].design.data,
               sizeof(appearance->pattern_texture));
    }
    return TRUE;
}

static void Net_UpdateAppearance(void) {
    AcNetPlayerAppearance appearance;
    if (!gameplay_ready || acnet_client_status() != ACNET_CONNECTED ||
        !Net_CaptureAppearance(&appearance)) return;
    if (appearance_sent && memcmp(&appearance, &last_sent_appearance, sizeof(appearance)) == 0) return;
    if (acnet_client_update_appearance(&appearance)) {
        last_sent_appearance = appearance;
        appearance_sent = TRUE;
    }
}

static MY_ROOM_ACTOR* Net_FindMyRoom(GAME_PLAY* play) {
    ACTOR* actor;
    if (play == NULL) return NULL;
    actor = play->actor_info.list[ACTOR_PART_BG].actor;
    while (actor != NULL) {
        if (actor->id == mAc_PROFILE_MY_ROOM) return (MY_ROOM_ACTOR*)actor;
        actor = actor->next_actor;
    }
    return NULL;
}

static u64 Net_HashBytes(u64 hash, const void* data, size_t size) {
    const u8* bytes = (const u8*)data;
    size_t i;
    for (i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Which save-side room a networked room scene refers to. A resident house is
 * three floors of Save_t.homes[slot]; the island cabin is the single floor of
 * Save_t.island.cottage, which belongs to the town rather than to any player,
 * so it has no owner, no upgrade path and no indexed light switch pair. */
typedef struct net_room_binding_s {
    int valid;
    int shared;
    int slot;         /* resident slot, or -1 for the shared cabin */
    int floor_count;
    mHm_flr_c* floors;
    u8 upgrade_level;
} NetRoomBinding;

static int Net_ResolveRoom(int scene, const AcNetHouseState* state, NetRoomBinding* room) {
    memset(room, 0, sizeof(*room));
    room->slot = -1;
    if (state == NULL || state->zone_id != Net_SceneZone(scene)) return FALSE;
    if (mSc_IS_SCENE_PLAYER_ROOM(scene) && state->original_slot < PLAYER_NUM) {
        mHm_hs_c* home = &Save_Get(homes[state->original_slot]);
        room->valid = TRUE;
        room->slot = state->original_slot;
        room->floor_count = mHm_ROOM_NUM;
        room->floors = home->floors;
        room->upgrade_level = home->size_info.size;
        return TRUE;
    }
    if (scene == SCENE_COTTAGE_MY && state->original_slot == NET_SHARED_HOUSE_SLOT) {
        room->valid = TRUE;
        room->shared = TRUE;
        room->floor_count = 1;
        room->floors = &Save_Get(island).cottage.room;
        room->upgrade_level = 0;
        return TRUE;
    }
    return FALSE;
}

/* The room surfaces and the exterior, gathered from the same save the furniture
 * grid comes from. A shared cabin has no exterior of its own -- mHm_cottage_c
 * has no palette or door -- so those four stay zero for it. */
static void Net_CaptureHouseSurfaces(const NetRoomBinding* room, AcNetHouseSurfaces* surfaces) {
    int floor;

    memset(surfaces, 0, sizeof(*surfaces));
    if (room == NULL || !room->valid) return;
    for (floor = 0; floor < room->floor_count && floor < 3; ++floor) {
        const mHm_flr_c* f = &room->floors[floor];
        surfaces->wallpaper[floor] = f->wall_floor.wallpaper_idx;
        surfaces->flooring[floor] = f->wall_floor.flooring_idx;
        surfaces->pattern_bits[floor] =
            (u8)((f->floor_bit_info.wall_original ? 1 : 0) | (f->floor_bit_info.floor_original ? 2 : 0));
    }
    if (!room->shared) {
        const mHm_hs_c* home = &Save_Get(homes[room->slot]);
        surfaces->exterior_palette = home->outlook_pal;
        surfaces->ordered_exterior_palette = home->ordered_outlook_pal;
        surfaces->next_exterior_palette = home->next_outlook_pal;
        surfaces->door_design = home->door_original;
        surfaces->music_box[0] = home->music_box[0];
        surfaces->music_box[1] = home->music_box[1];
    } else {
        /* The cabin keeps its own stereo, separate from any resident's. */
        const mHm_cottage_c* cottage = &Save_Get(island).cottage;
        surfaces->music_box[0] = cottage->music_box[0];
        surfaces->music_box[1] = cottage->music_box[1];
    }
}

static size_t Net_CaptureHouse(const NetRoomBinding* room,
                               const AcNetHouseState* baseline,
                               MY_ROOM_ACTOR* my_room,
                               int16_t music_tracks[3],
                               uint64_t switches[12],
                               AcNetHouseSurfaces* surfaces,
                               u64* hash_out) {
    size_t count = 0;
    int floor;
    int layer;
    int z;
    int x;
    u64 hash = 1469598103934665603ULL;
    /* A push/pull/rotate lifts whatever sits on top of the furniture out of the
     * room grid until its animation settles, so the grid alone would describe a
     * room those items had been deleted from. Capture from a patched copy of the
     * affected layer instead: the same bytes the room will hold once the move
     * finishes, which keeps the submitted candidate complete and keeps the hash
     * stable across the settle. */
    aMR_net_fitted_item_c fitted[4];
    mActor_name_t fitted_items[UT_Z_NUM][UT_X_NUM];
    int fitted_floor = -1;
    int fitted_count = 0;
    if (room == NULL || !room->valid || baseline == NULL) return 0;
    memcpy(music_tracks, baseline->music_tracks, sizeof(baseline->music_tracks));
    if (my_room != NULL) {
        const int active_floor = mFI_GetPlayerHouseFloorNo(my_room->scene);
        aMR_NetFlushSwitches(my_room);
        if (active_floor >= 0 && active_floor < room->floor_count) {
            music_tracks[active_floor] = (int16_t)aMR_NetCurrentMusic(my_room);
            fitted_count = aMR_NetFittedItems(my_room, fitted, (int)ARRAY_COUNT(fitted));
            if (fitted_count > 0) {
                int i;
                memcpy(fitted_items, (&room->floors[active_floor].layer_main)[mCoBG_LAYER1].items,
                       sizeof(fitted_items));
                for (i = 0; i < fitted_count; ++i) {
                    if (fitted[i].layer != mCoBG_LAYER1) continue;
                    /* Never overwrite a cell the room already claims: the grid
                     * is authoritative for everything still in it. */
                    if (fitted_items[fitted[i].z][fitted[i].x] != EMPTY_NO) continue;
                    fitted_items[fitted[i].z][fitted[i].x] = fitted[i].item;
                }
                fitted_floor = active_floor;
            }
        }
    }
    for (floor = 0; floor < room->floor_count; ++floor) {
        mHm_lyr_c* layers = &room->floors[floor].layer_main;
        for (layer = 0; layer < mCoBG_LAYER_NUM; ++layer) {
            const mActor_name_t (*items)[UT_X_NUM] =
                (floor == fitted_floor && layer == mCoBG_LAYER1)
                    ? (const mActor_name_t(*)[UT_X_NUM])fitted_items
                    : (const mActor_name_t(*)[UT_X_NUM])layers[layer].items;
            switches[floor * mCoBG_LAYER_NUM + layer] = layers[layer].ftr_switch;
            hash = Net_HashBytes(hash, &layers[layer].ftr_switch, sizeof(layers[layer].ftr_switch));
            for (z = 0; z < UT_Z_NUM; ++z) {
                for (x = 0; x < UT_X_NUM; ++x) {
                    const mActor_name_t item = items[z][x];
                    mActor_name_t canonical_item = item;
                    u8 orientation = 0;
                    hash = Net_HashBytes(hash, &item, sizeof(item));
                    if (item == EMPTY_NO || count >= ARRAY_COUNT(house_furniture)) continue;
                    if (ITEM_IS_FTR(item)) {
                        canonical_item = mRmTp_FtrItemNo2Item1ItemNo(item, TRUE);
                        orientation = (u8)(item & 3);
                        if (ITEM_IS_FTR(canonical_item)) canonical_item &= ~3;
                    }
                    house_furniture[count].x = (u8)x;
                    house_furniture[count].z = (u8)z;
                    house_furniture[count].floor = (u8)floor;
                    house_furniture[count].layer = (u8)layer;
                    house_furniture[count].item = canonical_item;
                    house_furniture[count].condition = orientation;
                    ++count;
                }
            }
        }
    }
    hash = Net_HashBytes(hash, &room->upgrade_level, sizeof(room->upgrade_level));
    hash = Net_HashBytes(hash, music_tracks, sizeof(baseline->music_tracks));
    /* Repainting a wall or a house changes nothing in the furniture grid, so
     * without this the candidate hash would not move and the submit would never
     * fire. */
    Net_CaptureHouseSurfaces(room, surfaces);
    hash = Net_HashBytes(hash, surfaces, sizeof(*surfaces));
    /* The cabin has no light switch pair of its own -- the indexed switches are
     * per resident house -- so it contributes nothing here rather than hashing
     * some other room's lights. */
    if (!room->shared) {
        const int main_light = mRmTp_Index2LightSwitchStatus(room->slot * 2);
        const int basement_light = mRmTp_Index2LightSwitchStatus(room->slot * 2 + 1);
        hash = Net_HashBytes(hash, &main_light, sizeof(main_light));
        hash = Net_HashBytes(hash, &basement_light, sizeof(basement_light));
    }
    if (hash_out != NULL) *hash_out = hash;
    return count;
}

int Net_HouseMainLightOn(int house_index) {
    if (!Net_IsConnected() || house_index < 0 || house_index >= PLAYER_NUM) return FALSE;
    return (acnet_client_house_light_mask() & (1U << house_index)) != 0;
}

int Net_ApplyHouseStateBeforeRoom(GAME_PLAY* play) {
    AcNetHouseState state;
    AcNetHouseFurniture authoritative[3 * 4 * 16 * 16];
    NetRoomBinding room;
    size_t count;
    size_t i;
    int floor;
    int layer;
    int changed = FALSE;
    int can_interpolate;
    memset(&house_motion, 0, sizeof(house_motion));
    if (play == NULL || !Net_IsConnected() || !acnet_client_house(&state) || !state.initialized ||
        !Net_ResolveRoom(play->scene_id, &state, &room)) return FALSE;
    if (last_house_id != state.house_id) {
        last_house_id = state.house_id;
        last_house_revision = 0;
        house_candidate_hash = 0;
        house_submitted_hash = 0;
        house_candidate_frames = 0;
        house_update_pending = FALSE;
        house_wait_authoritative_revision = 0;
        house_reconcile_delay_frames = 0;
        force_house_reconcile = FALSE;
    }
    if (last_house_revision == state.revision && !force_house_reconcile) return FALSE;
    can_interpolate = last_house_revision != 0;
    memset(house_authoritative_items, 0, sizeof(house_authoritative_items));
    count = acnet_client_house_furniture(authoritative, ARRAY_COUNT(authoritative));
    for (i = 0; i < count; ++i) {
        mActor_name_t presentation_item;
        if (authoritative[i].floor >= room.floor_count || authoritative[i].layer >= mCoBG_LAYER_NUM ||
            authoritative[i].x >= UT_X_NUM || authoritative[i].z >= UT_Z_NUM) continue;
        presentation_item = authoritative[i].item;
        if ((presentation_item & 0xF000) != 0xF000) {
            presentation_item = mRmTp_Item1ItemNo2FtrItemNo_AtPlayerRoom(presentation_item, TRUE);
            if (ITEM_IS_FTR(presentation_item))
                presentation_item = (presentation_item & ~3) | (authoritative[i].condition & 3);
        }
        house_authoritative_items[authoritative[i].floor][authoritative[i].layer]
                                 [authoritative[i].z][authoritative[i].x] = presentation_item;
    }

    /* A single adjacent layer-zero move is the common push/pull case. Keep
     * enough information to animate an accepted remote move (or an owner-side
     * correction) after the authoritative grid has replaced the old one. */
    if (can_interpolate) {
        int source_count = 0;
        int destination_count = 0;
        mActor_name_t source_item = EMPTY_NO;
        mActor_name_t destination_item = EMPTY_NO;
        for (floor = 0; floor < room.floor_count; ++floor) {
            mActor_name_t (*old_items)[UT_X_NUM] = room.floors[floor].layer_main.items;
            for (i = 0; i < UT_X_NUM * UT_Z_NUM; ++i) {
                const int x = (int)(i % UT_X_NUM);
                const int z = (int)(i / UT_X_NUM);
                const mActor_name_t old_item = ((mActor_name_t*)old_items)[i];
                const mActor_name_t new_item = house_authoritative_items[floor][mCoBG_LAYER0][z][x];
                if (old_item == new_item) continue;
                if (ITEM_IS_FTR(old_item)) {
                    ++source_count;
                    source_item = old_item;
                    house_motion.floor = (u8)floor;
                    house_motion.layer = mCoBG_LAYER0;
                    house_motion.source_x = (u8)x;
                    house_motion.source_z = (u8)z;
                }
                if (ITEM_IS_FTR(new_item)) {
                    ++destination_count;
                    destination_item = new_item;
                    house_motion.destination_x = (u8)x;
                    house_motion.destination_z = (u8)z;
                }
            }
        }
        if (source_count == 1 && destination_count == 1 &&
            (source_item & ~3) == (destination_item & ~3) &&
            (source_item & 3) == (destination_item & 3)) {
            const int dx = (int)house_motion.destination_x - (int)house_motion.source_x;
            const int dz = (int)house_motion.destination_z - (int)house_motion.source_z;
            house_motion.valid = (ABS(dx) + ABS(dz)) == 1;
        }
    }

    /* The cabin cannot be upgraded, so its authoritative level stays 0 and
     * there is no size field to write it into. */
    if (!room.shared && Save_Get(homes[room.slot]).size_info.size != state.upgrade_level) {
        Save_Set(homes[room.slot].size_info.size, state.upgrade_level);
        changed = TRUE;
    }
    for (floor = 0; floor < room.floor_count; ++floor) {
        mHm_lyr_c* layers = &room.floors[floor].layer_main;
        for (layer = 0; layer < mCoBG_LAYER_NUM; ++layer) {
            if (layers[layer].ftr_switch != state.furniture_switches[floor * mCoBG_LAYER_NUM + layer]) changed = TRUE;
            layers[layer].ftr_switch = state.furniture_switches[floor * mCoBG_LAYER_NUM + layer];
            if (memcmp(layers[layer].items, house_authoritative_items[floor][layer],
                       sizeof(layers[layer].items)) != 0) changed = TRUE;
            memcpy(layers[layer].items, house_authoritative_items[floor][layer],
                   sizeof(layers[layer].items));
        }
    }
    /* The surfaces the whole room sees. A visitor gets the owner's wallpaper and
     * carpet instead of their own, and a repaint reaches everyone standing in
     * the house rather than only whoever applied it. Written before the room
     * actor reloads, since that is what re-reads them into the wall and floor
     * texture banks. */
    for (floor = 0; floor < room.floor_count && floor < 3; ++floor) {
        mHm_flr_c* f = &room.floors[floor];
        const u8 wall_original = (u8)(state.pattern_bits[floor] & 1);
        const u8 floor_original = (u8)((state.pattern_bits[floor] >> 1) & 1);
        if (f->wall_floor.wallpaper_idx != state.wallpaper[floor] ||
            f->wall_floor.flooring_idx != state.flooring[floor] ||
            f->floor_bit_info.wall_original != wall_original ||
            f->floor_bit_info.floor_original != floor_original) changed = TRUE;
        f->wall_floor.wallpaper_idx = state.wallpaper[floor];
        f->wall_floor.flooring_idx = state.flooring[floor];
        f->floor_bit_info.wall_original = wall_original;
        f->floor_bit_info.floor_original = floor_original;
    }
    if (!room.shared) {
        /* The cabin has no exterior of its own; only a resident house does. */
        mHm_hs_c* home = &Save_Get(homes[room.slot]);
        if (home->outlook_pal != state.exterior_palette ||
            home->ordered_outlook_pal != state.ordered_exterior_palette ||
            home->next_outlook_pal != state.next_exterior_palette ||
            home->door_original != state.door_design ||
            home->music_box[0] != state.music_box[0] ||
            home->music_box[1] != state.music_box[1]) changed = TRUE;
        home->outlook_pal = state.exterior_palette;
        home->ordered_outlook_pal = state.ordered_exterior_palette;
        home->next_outlook_pal = state.next_exterior_palette;
        home->door_original = state.door_design;
        home->music_box[0] = state.music_box[0];
        home->music_box[1] = state.music_box[1];
    } else {
        mHm_cottage_c* cottage = &Save_Get(island).cottage;
        if (cottage->music_box[0] != state.music_box[0] ||
            cottage->music_box[1] != state.music_box[1]) changed = TRUE;
        cottage->music_box[0] = state.music_box[0];
        cottage->music_box[1] = state.music_box[1];
    }
    if (!room.shared) {
        if (state.main_light_on) mRmTp_IndexLightSwitchON(room.slot * 2);
        else mRmTp_IndexLightSwitchOFF(room.slot * 2);
        if (state.basement_light_on) mRmTp_IndexLightSwitchON(room.slot * 2 + 1);
        else mRmTp_IndexLightSwitchOFF(room.slot * 2 + 1);
    }
    last_house_revision = state.revision;
    force_house_reconcile = FALSE;
    return changed;
}

static void Net_UpdateHouseState(GAME_PLAY* play) {
    AcNetHouseState state;
    AcNetHouseUpdateResult result;
    MY_ROOM_ACTOR* my_room;
    NetRoomBinding room;
    int16_t music_tracks[3];
    uint64_t switches[12];
    AcNetHouseSurfaces surfaces;
    u64 hash;
    size_t count;
    int floor;
    int changed;
    int is_editor;
    int furniture_move_active;
    while (acnet_client_take_house_update_result(&result)) {
        house_update_pending = FALSE;
        if (result.result_code == 0) {
            house_submitted_hash = house_candidate_hash;
            house_wait_authoritative_revision = result.house_revision;
        }
        else {
            house_candidate_frames = 0;
            house_wait_authoritative_revision = 0;
            /* The result can arrive before the original player state has
             * entered PUSH/PULL for the next simulation frame. Hold the old
             * baseline through that startup and settle window as well. */
            house_reconcile_delay_frames = 60;
            /* A rejected optimistic local edit must be overwritten even when
             * the authoritative room revision did not advance. */
            force_house_reconcile = TRUE;
        }
    }
    if (!acnet_client_house(&state) || !Net_ResolveRoom(play->scene_id, &state, &room)) return;
    /* The island cabin belongs to the town, so every occupant is an editor and
     * runs the owner path. Two of them editing at once is resolved the same way
     * a stale owner edit is: the server refuses the second against the moved
     * revision and the loser reconciles. */
    is_editor = room.shared || state.owner_account_id == acnet_client_account();
    my_room = Net_FindMyRoom(play);
    if (is_editor) {
        /* HouseUpdateResult is delivered before the resulting baseline
         * broadcast. Never roll back the local prediction to the previous
         * revision during that window. */
        if (house_update_pending) return;
        if (house_wait_authoritative_revision != 0) {
            if (state.revision < house_wait_authoritative_revision) return;
            house_wait_authoritative_revision = 0;
        }
        if (house_reconcile_delay_frames > 0) {
            --house_reconcile_delay_frames;
            return;
        }
    }
    furniture_move_active = mPlib_check_player_actor_main_index_Furniture_Move((GAME*)play) ||
                            aMR_NetFurnitureMoveActive(my_room);
    if (!is_editor && last_house_revision == state.revision && !force_house_reconcile) {
        count = Net_CaptureHouse(&room, &state, my_room, music_tracks, switches, &surfaces, &hash);
        (void)count;
        if (house_submitted_hash == 0) {
            house_submitted_hash = hash;
        } else if (hash != house_submitted_hash) {
            /* Visitors may run the original local interaction for responsive
             * presentation, but they cannot authoritatively decorate someone
             * else's house. Correct their prediction after its keyframe. */
            if (furniture_move_active) return;
            force_house_reconcile = TRUE;
        }
    }
    /* The original push/pull owns the local actor until its keyframe settles.
     * Rebuilding the furniture list mid-action invalidates that handshake and
     * can leave the player applying root motion forever. The local result is
     * already predicted in Save_t, so wait to compare/correct until it ends. */
    if (furniture_move_active &&
        (last_house_revision != state.revision || force_house_reconcile)) return;
    changed = Net_ApplyHouseStateBeforeRoom(play);
    floor = mFI_GetPlayerHouseFloorNo(play->scene_id);
    if (changed && my_room != NULL) {
        if (house_motion.valid && house_motion.floor == floor) {
            aMR_NetReloadFurnitureMotion((ACTOR*)my_room, (GAME*)play,
                                         house_motion.source_x, house_motion.source_z,
                                         house_motion.destination_x, house_motion.destination_z,
                                         house_motion.layer, 28.0f);
        } else {
            aMR_NetReloadFurniture((ACTOR*)my_room, (GAME*)play);
        }
    }
    if (state.initialized && my_room != NULL && floor >= 0 && floor < 3)
        aMR_NetSetMusic(my_room, state.music_tracks[floor]);
    if (!is_editor) {
        Net_CaptureHouse(&room, &state, my_room, music_tracks, switches, &surfaces, &house_submitted_hash);
        return;
    }
    if (house_update_pending) return;
    count = Net_CaptureHouse(&room, &state, my_room, music_tracks, switches, &surfaces, &hash);
    if (hash != house_candidate_hash) {
        house_candidate_hash = hash;
        if (furniture_move_active) {
            /* Push/pull has already written its predicted destination into the
             * room grid. Submit that complete candidate immediately so the
             * server can validate it while the original keyframe is playing
             * and peers can begin their interpolated presentation promptly. */
            house_candidate_frames = 30;
        } else {
            house_candidate_frames = 0;
            return;
        }
    }
    if (house_candidate_frames < 30) {
        ++house_candidate_frames;
        return;
    }
    if (state.initialized && hash == house_submitted_hash) return;
    if (acnet_client_submit_house_update(state.house_id, state.revision, room.upgrade_level,
                                         room.shared ? 0 : (u8)mRmTp_Index2LightSwitchStatus(room.slot * 2),
                                         room.shared ? 0 : (u8)mRmTp_Index2LightSwitchStatus(room.slot * 2 + 1),
                                         music_tracks, switches, &surfaces, house_furniture, count)) {
        house_update_pending = TRUE;
    }
}

static u32 Net_PlayerRoomZone(void) {
    mActor_name_t field_id = mFI_GetFieldId();
    mActor_name_t owner;
    if (mFI_IS_PLAYER_ROOM(field_id)) return 100 + mFI_GET_PLAYER_ROOM_NO(field_id);
    owner = Common_Get(house_owner_name);
    if (owner < PLAYER_NUM) return 100 + owner;
    return 100 + Common_Get(player_no);
}

/* The island is two acres of SCENE_FG, so the scene number alone cannot say
 * whether the player is in the town or on the island. This tracks it from the
 * acre kind while outdoors and from the scene while in an island interior, and
 * is read when computing the zone of a scene the player has not entered yet --
 * leaving the cabin, the next SCENE_FG is the island, not the town. */
static int net_on_island = FALSE;

static void Net_UpdateIslandResidency(GAME_PLAY* play) {
    if (play == NULL) return;
    if (play->scene_id == SCENE_COTTAGE_MY || play->scene_id == SCENE_COTTAGE_NPC) {
        net_on_island = TRUE;
    } else if (play->scene_id == SCENE_FG) {
        net_on_island = mFI_CheckInJustIslandOutdoor();
    } else {
        /* Every other scene is a town interior, which the island cannot reach.
         * Clearing here is what stops a stale flag from routing a shop exit
         * into the island zone. */
        net_on_island = FALSE;
    }
}

static u32 Net_SceneZone(int scene) {
    if (scene == SCENE_FG) return net_on_island ? NET_ZONE_ISLAND : 1;
    if (scene == SCENE_COTTAGE_MY) return NET_ZONE_ISLAND_CABIN;
    if (scene == SCENE_COTTAGE_NPC) return NET_ZONE_ISLAND_NPC_HOUSE;
    if (scene == SCENE_SHOP0 || scene == SCENE_CONVENI || scene == SCENE_SUPER ||
        scene == SCENE_DEPART || scene == SCENE_DEPART_2) return 2;
    if (scene == SCENE_POST_OFFICE) return 3;
    if (mSc_IS_SCENE_MUSEUM_ROOM(scene)) return 4;
    if (scene == SCENE_NEEDLEWORK) return 5;
    if (scene == SCENE_POLICE_BOX) return 6;
    /* A room scene identifies its size, not its owner. The owner lives in the
     * field id/house_owner_name. Using the local resident slot placed two
     * visitors to the same physical house in different network zones. */
    if (mSc_IS_SCENE_PLAYER_ROOM(scene)) return Net_PlayerRoomZone();
    return 0;
}

static u32 Net_DoorForZones(u32 source, u32 destination) {
    if (source == 1) {
        if (destination == 2) return 10;
        if (destination == 3) return 20;
        if (destination == 4) return 30;
        if (destination == 5) return 40;
        if (destination == 6) return 50;
        /* The Kapp'n ferry never changes scene, so no door animation announces
         * it. Net_EnsureSceneZone notices the acre kind changed and asks for
         * this door; the server checks the source zone, not proximity. */
        if (destination == NET_ZONE_ISLAND) return NET_DOOR_FERRY_TO_ISLAND;
        if (destination >= 100 && destination <= 103) return destination;
    } else if (destination == 1) {
        if (source == 2) return 11;
        if (source == 3) return 21;
        if (source == 4) return 31;
        if (source == 5) return 41;
        if (source == 6) return 51;
        if (source == NET_ZONE_ISLAND) return NET_DOOR_FERRY_TO_TOWN;
        if (source >= 100 && source <= 103) return source + 100;
    } else if (source == NET_ZONE_ISLAND) {
        if (destination == NET_ZONE_ISLAND_CABIN) return NET_DOOR_CABIN_ENTER;
        if (destination == NET_ZONE_ISLAND_NPC_HOUSE) return NET_DOOR_ISLAND_HUT_ENTER;
    } else if (destination == NET_ZONE_ISLAND) {
        if (source == NET_ZONE_ISLAND_CABIN) return NET_DOOR_CABIN_LEAVE;
        if (source == NET_ZONE_ISLAND_NPC_HOUSE) return NET_DOOR_ISLAND_HUT_LEAVE;
    }
    return 0;
}

void Net_BeginSceneTransition(GAME_PLAY* play, int next_scene) {
    u32 source;
    u32 destination;
    u32 door;
    if (play == NULL || !Net_IsConnected() || zone_transfer_phase != 0) return;
    source = Net_SceneZone(play->scene_id);
    destination = Net_SceneZone(next_scene);
    if (source == 0 || destination == 0 || source == destination ||
        acnet_client_baseline_zone() != source) return;
    door = Net_DoorForZones(source, destination);
    if (door != 0 && acnet_client_request_zone_transfer(door)) {
        pending_destination_zone = destination;
        zone_transfer_phase = 1;
    }
}

/* What the local skeleton is doing, in the same indices the original player
 * uses: Player_actor_InitAnimation_Base* stores them on the actor, so nothing
 * has to be intercepted -- they are read once a frame alongside the transform.
 * Every field is clamped, because an out-of-range index would be indexed into
 * an animation table on someone else's machine. */
typedef struct net_animation_s {
    u8 body;
    u8 overlay;
    u8 part_table;
    u8 item_state;
    u8 looping;
    u8 reversed;
} NET_ANIMATION;

static void Net_CapturePlayerAnimation(const PLAYER_ACTOR* player, NET_ANIMATION* animation) {
    const cKF_FrameControl_c* frame_control;
    memset(animation, 0, sizeof(*animation));
    if (player == NULL) return;
    if (player->animation0_idx >= 0 && player->animation0_idx < mPlayer_ANIM_NUM)
        animation->body = (u8)player->animation0_idx;
    if (player->animation1_idx >= 0 && player->animation1_idx < mPlayer_ANIM_NUM)
        animation->overlay = (u8)player->animation1_idx;
    if (player->part_table_idx >= 0 && player->part_table_idx < mPlayer_PART_TABLE_NUM)
        animation->part_table = (u8)player->part_table_idx;
    if (mPlayer_ITEM_MAIN_VALID(player->now_item_main_index))
        animation->item_state = (u8)player->now_item_main_index;
    frame_control = &player->keyframe0.frame_control;
    animation->looping = frame_control->mode == cKF_FRAMECONTROL_REPEAT;
    /* cKF_SkeletonInfo_R_init_reverse_* starts at the last frame and ends at
     * the first, which is the only thing that distinguishes it. */
    animation->reversed = frame_control->start_frame > frame_control->end_frame;
}

/* The resource selectors a viewer cannot derive: which face texture and palette
 * to load, whether the umbrella is mid-open, and what is in the hand during a
 * pickup or scoop. Everything here is read from the local player's own state
 * once a frame and latched; the next frame command carries it.
 *
 * The bee-sting and decoy flags live in the town-common block rather than on
 * the actor because the original only ever has one player, so Common_Get is the
 * acting player's own state here -- see the sole-player classification in
 * ARCHITECTURE_AUDIT.md. Nothing reads them for a remote. */
static void Net_CapturePlayerAppearanceBits(const PLAYER_ACTOR* player) {
    mActor_name_t carried = EMPTY_NO;
    int sunburn = 0;
    int umbrella = 0;
    int change_color = FALSE;

    if (player != NULL) {
        umbrella = player->umbrella_state;
        change_color = player->change_color_flag != 0;
        /* Only meaningful while the matching animation is the one running --
         * main_data is a union of every main state, so reading pickup.item
         * during a scoop would report another state's bytes. */
        if (player->now_main_index == mPlayer_INDEX_PICKUP)
            carried = player->main_data.pickup.item;
        else if (player->now_main_index == mPlayer_INDEX_GET_SCOOP)
            carried = player->main_data.get_scoop.item;
    }
    if (Now_Private != NULL && Now_Private->sunburn.rank > 0) sunburn = Now_Private->sunburn.rank;

    acnet_client_set_appearance_bits((uint8_t)(Common_Get(player_bee_swell_flag) == TRUE),
                                     (uint8_t)(Common_Get(player_decoy_flag) == TRUE),
                                     (uint8_t)change_color,
                                     (uint8_t)sunburn,
                                     (uint8_t)(umbrella >= 0 && umbrella < aTOL_ACTION_NUM ? umbrella : 0),
                                     (uint16_t)(carried == EMPTY_NO ? 0 : carried));
}

static int Net_CapturePlayerTransform(GAME_PLAY* play, AcNetTransform* transform) {
    PLAYER_ACTOR* player;
    if (play == NULL || transform == NULL) return FALSE;
    player = (PLAYER_ACTOR*)play->actor_info.list[ACTOR_PART_PLAYER].actor;
    if (player == NULL || player->actor_class.id != mAc_PROFILE_PLAYER) return FALSE;
    transform->x = player->actor_class.world.position.x;
    transform->y = player->actor_class.world.position.y;
    transform->z = player->actor_class.world.position.z;
    transform->velocity_x = player->actor_class.position_speed.x;
    transform->velocity_y = player->actor_class.position_speed.y;
    transform->velocity_z = player->actor_class.position_speed.z;
    transform->yaw = player->actor_class.shape_info.rotation.y;
    /* The server refuses an input command whose action is out of range, and
     * that would drop the movement in the same packet, so an unexpected value
     * degrades to "idle" rather than to "frozen". */
    transform->action = mPlayer_MAIN_INDEX_VALID(player->now_main_index) ? (u16)player->now_main_index : 0;
    return TRUE;
}

static void Net_ResetZoneTransfer(void) {
    pending_destination_zone = 0;
    pending_transfer_token_high = 0;
    pending_transfer_token_low = 0;
    zone_transfer_phase = 0;
}

static void Net_UpdateZoneTransfer(GAME_PLAY* play) {
    AcNetTransferOffer offer;
    if (zone_transfer_phase != 0 && acnet_client_baseline_zone() == pending_destination_zone) {
        zone_arrival_stream_frames = 120;
        Net_ResetZoneTransfer();
        (void)acnet_client_take_transfer_offer(&offer);
        return;
    }
    if (zone_transfer_phase == 0) return;
    if (zone_transfer_phase == 3) {
        if (acnet_client_take_transfer_offer(&offer) && offer.result_code != 0) {
            Net_ResetZoneTransfer();
            zone_retry_frames = 30;
        }
        return;
    }
    if (zone_transfer_phase == 1) {
        if (!acnet_client_take_transfer_offer(&offer)) return;
        if (offer.result_code != 0 || offer.destination_zone != pending_destination_zone) {
            extern int g_pc_verbose;
            if (g_pc_verbose) {
                printf("[NET] zone transfer rejected result=%u source=%u destination=%u expected=%u; will retry\n",
                       offer.result_code,
                       offer.source_zone,
                       offer.destination_zone,
                       pending_destination_zone);
                fflush(stdout);
            }
            Net_ResetZoneTransfer();
            zone_retry_frames = 30;
            return;
        }
        pending_transfer_token_high = offer.token_high;
        pending_transfer_token_low = offer.token_low;
        zone_transfer_phase = 2;
    }
    if (zone_transfer_phase == 2 && Net_SceneZone(play->scene_id) == pending_destination_zone) {
        AcNetTransform transform;
        if (!Net_CapturePlayerTransform(play, &transform)) return;
        if (!acnet_client_zone_ready(pending_transfer_token_high, pending_transfer_token_low, &transform)) {
            Net_ResetZoneTransfer();
            zone_retry_frames = 30;
        } else zone_transfer_phase = 3;
    }
}

static void Net_EnsureSceneZone(GAME_PLAY* play) {
    u32 source;
    u32 destination;
    u32 door;
    if (zone_retry_frames > 0) {
        zone_retry_frames--;
        return;
    }
    if (play == NULL || !Net_IsConnected() || zone_transfer_phase != 0) return;
    source = acnet_client_baseline_zone();
    destination = Net_SceneZone(play->scene_id);
    if (source == 0 || destination == 0 || source == destination) return;
    door = Net_DoorForZones(source, destination);
    if (door != 0 && acnet_client_request_zone_transfer(door)) {
        extern int g_pc_verbose;
        pending_destination_zone = destination;
        zone_transfer_phase = 1;
        if (g_pc_verbose) {
            printf("[NET] reconciling scene zone source=%u destination=%u door=%u\n",
                   source, destination, door);
            fflush(stdout);
        }
    } else {
        zone_retry_frames = 30;
    }
}

int Net_IsConnected(void) {
    return acnet_client_status() == ACNET_CONNECTED;
}

int Net_IsOnline(void) {
    return acnet_client_status() != ACNET_OFFLINE;
}

int Net_ConfigureQuickstart(const char* name, int gender) {
    size_t length;
    size_t i;
    if (name == NULL || (gender != mPr_SEX_MALE && gender != mPr_SEX_FEMALE)) return FALSE;
    length = strlen(name);
    if (length == 0 || length > PLAYER_NAME_LEN) return FALSE;
    for (i = 0; i < length; ++i) {
        const unsigned char character = (unsigned char)name[i];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9'))) return FALSE;
    }
    memset(quickstart_name, ' ', sizeof(quickstart_name));
    memcpy(quickstart_name, name, length);
    quickstart_gender = gender;
    quickstart_enabled = TRUE;
    return TRUE;
}

int Net_QuickstartEnabled(void) {
    return quickstart_enabled;
}

int Net_ResidentSlot(void) {
    const u8 slot = acnet_client_resident_slot();
    return Net_IsConnected() && slot < PLAYER_NUM ? (int)slot : -1;
}

/* AcNetResident.name is 8 bytes and acnet::kOriginalResidentSlots is 4, matching
 * PLAYER_NAME_LEN and PLAYER_NUM. Like the zone identifiers above, the C
 * boundary carries plain integers and the two definitions are kept in step by
 * hand. */
int Net_ResidentIdentity(int slot, u8* name, s8* gender) {
    AcNetResident roster[PLAYER_NUM];
    size_t count;
    if (!Net_IsConnected() || slot < 0 || slot >= PLAYER_NUM) return -1;
    count = acnet_client_residents(roster, PLAYER_NUM);
    if (count != PLAYER_NUM) return -1; /* no roster has arrived yet */
    if (!roster[slot].occupied) return 0;
    if (name != NULL) memcpy(name, roster[slot].name, PLAYER_NAME_LEN);
    if (gender != NULL) *gender = (s8)roster[slot].gender;
    return 1;
}

int Net_PrefillQuickstartName(void) {
    if (!quickstart_enabled || !Net_IsConnected() || Now_Private == NULL) return FALSE;
    memcpy(Now_Private->player_ID.player_name, quickstart_name, sizeof(quickstart_name));
    Net_ApplyTownIdentity();
    return TRUE;
}

void Net_ApplyTownIdentity(void) {
    u8 name[LAND_NAME_SIZE];
    if (!Net_IsConnected() || acnet_client_town_name(name, sizeof(name)) != sizeof(name)) return;
    memcpy(Save_Get(land_info).name, name, sizeof(name));
    Save_Set(land_info.id, acnet_client_town_land_id());
    Save_Set(land_info.exists, TRUE);
    if (Now_Private != NULL) {
        memcpy(Now_Private->player_ID.land_name, name, sizeof(name));
        Now_Private->player_ID.land_id = acnet_client_town_land_id();
    }
}

void Net_RandomizeInitialAppearance(void) {
    u32 seed;
    u32 slot;
    u32 face_roll;
    u32 cloth_roll;
    mActor_name_t clothing;
    if (!Net_IsConnected() || Now_Private == NULL) return;
    slot = acnet_client_resident_slot();
    if (slot >= PLAYER_NUM) return;
    seed = acnet_client_town_seed();

    /* A stable town/slot roll gives the four resident slots distinct starter
     * faces and outfits while keeping a character unchanged across reconnects.
     * The 61-shirt stride is coprime with the 255 stock designs, so the four
     * resident slots cannot collide within one town. */
    face_roll = (seed ^ (seed >> 8) ^ (seed >> 16)) + slot * 5U;
    cloth_roll = ((seed >> 3) ^ (seed >> 13) ^ (seed >> 23)) + slot * 61U;
    Now_Private->face = (s8)(face_roll & (mPr_FACE_TYPE_NUM - 1));
    clothing = ITM_CLOTH000 + cloth_roll % CLOTH_NUM;
    mPlib_change_player_cloth_info_lv2(Now_Private, clothing);
}

int Net_ApplyQuickstartIdentity(void) {
    if (!quickstart_enabled || !Net_IsConnected() || Now_Private == NULL) return FALSE;
    memcpy(Now_Private->player_ID.player_name, quickstart_name, sizeof(quickstart_name));
    Now_Private->gender = quickstart_gender;
    Net_ApplyTownIdentity();
    Net_RandomizeInitialAppearance();
    return TRUE;
}

/* The island's two acres, read out of Save_t.island rather than the town
 * foreground: mFM_SetFgUtPtoSaveData swaps that storage in for the island
 * blocks, so the town arrays never hold island items. Also reports which acre
 * columns those blocks occupy, which the server needs because the island is
 * placed by the town's acre layout rather than at a fixed coordinate.
 *
 * Returns 0 when the field has not been built yet and the acre kinds cannot be
 * read. The server then leaves the island uninitialized and adopts it from a
 * later login rather than installing an island at the wrong coordinates. */
/* Defined further down, beside the projection it feeds. */
static void Net_CaptureVillagers(void);

static size_t Net_CaptureIslandBootstrap(AcNetTownBootstrapTile* out,
                                         size_t capacity,
                                         int* island_block_x) {
    size_t index = 0;
    int block;
    int unit_z;
    int unit_x;
    island_block_x[0] = 0;
    island_block_x[1] = 0;
    if (capacity < mISL_FG_BLOCK_X_NUM * UT_X_NUM * UT_Z_NUM) return 0;
    mFI_GetIslandBlockNumX(island_block_x);
    /* mFI_GetIslandBlockNumX leaves both entries at zero when the acre kind
     * table is not available; a real island never starts at column zero,
     * because the leftmost column is ocean. */
    if (island_block_x[0] <= 0 || island_block_x[1] <= island_block_x[0] ||
        island_block_x[1] >= BLOCK_X_NUM) return 0;
    for (block = 0; block < mISL_FG_BLOCK_X_NUM; ++block) {
        u16* deposit = Save_Get(island).deposit[block];
        for (unit_z = 0; unit_z < UT_Z_NUM; ++unit_z) {
            for (unit_x = 0; unit_x < UT_X_NUM; ++unit_x, ++index) {
                out[index].item = Save_Get(island).fgblock[0][block].items[unit_z][unit_x];
                out[index].buried = (u8)mFI_GetBlockDeposit(deposit, unit_x, unit_z);
            }
        }
    }
    return index;
}

int Net_SubmitInitialTown(void) {
    static AcNetTownBootstrapTile tiles[5 * 6 * 16 * 16];
    static AcNetTownBootstrapTile island_tiles[mISL_FG_BLOCK_X_NUM * UT_X_NUM * UT_Z_NUM];
    AcNetPlayerAppearance appearance;
    int island_block_x[mISL_FG_BLOCK_X_NUM];
    size_t island_count;
    size_t index = 0;
    size_t populated = 0;
    int block_z;
    int block_x;
    int unit_z;
    int unit_x;
    if (!Net_IsConnected() || Now_Private == NULL) return FALSE;
    Net_ApplyTownIdentity();
    if (!Net_CaptureAppearance(&appearance)) return FALSE;
    for (block_z = 0; block_z < FG_BLOCK_Z_NUM; ++block_z) {
        for (block_x = 0; block_x < FG_BLOCK_X_NUM; ++block_x) {
            u16* deposit = Save_Get(deposit[block_z * FG_BLOCK_X_NUM + block_x]);
            for (unit_z = 0; unit_z < UT_Z_NUM; ++unit_z) {
                for (unit_x = 0; unit_x < UT_X_NUM; ++unit_x, ++index) {
                    tiles[index].item = Save_Get(fg[block_z][block_x]).items[unit_z][unit_x];
                    tiles[index].buried = (u8)mFI_GetBlockDeposit(deposit, unit_x, unit_z);
                    if (tiles[index].item != EMPTY_NO) ++populated;
                }
            }
        }
    }
    /* Never install an empty foreground. The bootstrap is a one-shot -- the
     * server records the town as created and will not import another -- so a
     * submission made before the save's field exists would leave the town
     * permanently blank, and its baselines would then erase every client's own
     * field an acre at a time. */
    if (populated == 0) return FALSE;
    island_count = Net_CaptureIslandBootstrap(island_tiles, ARRAY_COUNT(island_tiles), island_block_x);
    Net_CaptureVillagers();
    return acnet_client_submit_town_bootstrap(Save_Get(land_info).name,
                                               Save_Get(land_info).id,
                                               (u16)Save_Get(fruit),
                                               &appearance,
                                               tiles,
                                               index,
                                               island_count != 0 ? island_tiles : NULL,
                                               island_count,
                                               (u8)island_block_x[0],
                                               (u8)island_block_x[1]);
}

/* The town's foreground only reaches the server through Net_SubmitInitialTown,
 * and its only callers are the two guide-NPC scripts that run when a resident
 * creates a town from the intro. A quickstart login skips those, and so does
 * every login into a town that was restored without a foreground. The server
 * then answers each interest window with an all-empty chunk and
 * Net_ApplyAuthoritativeState writes it over the client's field, so trees,
 * rocks and the bulletin board vanish as the player walks up to them. Offer the
 * world whenever the server says the town is still uninitialised; the capture
 * refuses to send an empty one, so a client whose save is not ready yet simply
 * tries again. */
static void Net_SubmitTownIfUninitialized(GAME_PLAY* play) {
    static int retry_delay = 0;
    if (play == NULL || !Net_IsConnected() || acnet_client_town_initialized()) {
        retry_delay = 0;
        return;
    }
    if (play->scene_id != SCENE_FG) return;
    if (retry_delay > 0) {
        retry_delay--;
        return;
    }
    retry_delay = 60;
    (void)Net_SubmitInitialTown();
}

static int Net_GameplayReadyNow(GAME_PLAY* play) {
    PLAYER_ACTOR* player;
    mDemo_Clip_c* demo_clip;
    u32 zone;
    if (play == NULL || !Net_IsConnected() || !acnet_client_town_initialized()) return FALSE;
    zone = Net_SceneZone(play->scene_id);
    if (zone == 0 || acnet_client_baseline_zone() != zone) return FALSE;
    demo_clip = Common_Get(clip).demo_clip;
    if (demo_clip != NULL &&
        (demo_clip->type == mDemo_CLIP_TYPE_INTRO_DEMO ||
         demo_clip->type == mDemo_CLIP_TYPE_RIDE_OFF_DEMO)) return FALSE;
    player = GET_PLAYER_ACTOR(play);
    if (player == NULL) return FALSE;
    switch (player->now_main_index) {
        case mPlayer_INDEX_INTRO:
        case mPlayer_INDEX_RETURN_DEMO:
        case mPlayer_INDEX_DEMO_GETON_TRAIN:
        case mPlayer_INDEX_DEMO_GETON_TRAIN_WAIT:
        case mPlayer_INDEX_DEMO_GETOFF_TRAIN:
        case mPlayer_INDEX_DEMO_STANDING_TRAIN:
            return FALSE;
    }
    return TRUE;
}

static void Net_UpdateGameplayReadiness(GAME_PLAY* play) {
    if (!Net_GameplayReadyNow(play)) {
        gameplay_ready_frames = 0;
        gameplay_ready = FALSE;
        gameplay_ready_reported = FALSE;
        return;
    }
    if (gameplay_ready_frames < 60) gameplay_ready_frames++;
    if (gameplay_ready_frames >= 60) {
        gameplay_ready = TRUE;
        if (!gameplay_ready_reported) {
            extern int g_pc_verbose;
            if (g_pc_verbose) {
                printf("[NET] gameplay ready scene=%d zone=%u\n",
                       play->scene_id,
                       Net_SceneZone(play->scene_id));
                fflush(stdout);
            }
            gameplay_ready_reported = TRUE;
        }
    }
}

static void Net_ApplyAuthoritativeClock(void) {
    const s64 town_seconds = acnet_client_town_time();
    time_t raw;
    struct tm civil;
    int weather;
    int intensity;
    if (town_seconds <= 0) return;
    raw = (time_t)town_seconds;
    memset(&civil, 0, sizeof(civil));
#ifdef _WIN32
    if (gmtime_s(&civil, &raw) != 0) return;
#else
    if (gmtime_r(&raw, &civil) == NULL) return;
#endif
    Common_Set(time.rtc_time.year, civil.tm_year + 1900);
    Common_Set(time.rtc_time.month, civil.tm_mon + 1);
    Common_Set(time.rtc_time.day, civil.tm_mday);
    Common_Set(time.rtc_time.weekday, civil.tm_wday);
    Common_Set(time.rtc_time.hour, civil.tm_hour);
    Common_Set(time.rtc_time.min, civil.tm_min);
    Common_Set(time.rtc_time.sec, civil.tm_sec);
    Common_Set(time.now_sec, civil.tm_hour * 3600 + civil.tm_min * 60 + civil.tm_sec);
    Common_Set(time.rad_min, ((civil.tm_min * 60 + civil.tm_sec) / 3600.0f) * 65536.0f);
    Common_Set(time.rad_hour,
               ((civil.tm_hour * 3600 + civil.tm_min * 60 + civil.tm_sec) / 43200.0f) * 65536.0f);
    weather = acnet_client_weather();
    if (weather == 2) weather = mEnv_WEATHER_RAIN;
    else if (weather == 3) weather = mEnv_WEATHER_SNOW;
    else weather = mEnv_WEATHER_CLEAR; /* The original client has no separate cloudy precipitation type. */
    intensity = acnet_client_weather_intensity();
    if (intensity < mEnv_WEATHER_INTENSITY_NONE) intensity = mEnv_WEATHER_INTENSITY_NONE;
    if (intensity >= mEnv_WEATHER_INTENSITY_NUM) intensity = mEnv_WEATHER_INTENSITY_HEAVY;
    Common_Set(weather, weather);
    Common_Set(weather_intensity, intensity);
}

/* The zone whose tile table owns the player's current outdoor position: the
 * town or the island. Both address tiles by global unit coordinate -- the
 * island acres simply sit outside the town's rectangle -- which is what lets
 * mFI_UtNumtoFGSet_common route an island write into Save_t.island.fgblock
 * with no translation at either end. Returns 0 when no zone owns tiles here. */
static u32 Net_TileZone(void) {
    const u32 zone = acnet_client_baseline_zone();
    if (!Net_IsConnected()) return 0;
    return (zone == 1 || zone == NET_ZONE_ISLAND) ? zone : 0;
}

int Net_WorldTilesAuthoritative(void) {
    /* The town exterior and the island are the two zones the server stores
     * tiles for. Testing the authoritative baseline rather than the local
     * scene also keeps a mid-transfer frame, where the two disagree, out of
     * the tile path -- which matters more on the island, whose ferry crossing
     * is reconciled a frame or two after the player has already arrived. */
    return Net_TileZone() != 0;
}

/* A tile whose next visible change is being animated locally rather than
 * painted in by the authoritative projection. While a claim is held nothing
 * else writes that cell: the drop actor writes the field itself when it lands
 * (bIT_actor_drop_move), and projecting the item early would put it on the
 * ground while it is still in the air.
 *
 * A claim resolves on what the field actually shows, not on a timer: once the
 * cell holds the predicted item the animation has finished, and the mirror then
 * says whether the server agreed. The frame budget is only the backstop for an
 * animation that never lands -- the longest player drop is a 26-unit fall plus
 * one bounce, about 65 frames. */
#define NET_TILE_CLAIM_MAX 8
#define NET_TILE_CLAIM_FRAMES 90

typedef struct Net_TileClaim {
    u32 zone; /* 0 when the slot is free */
    s16 x;
    s16 z;
    mActor_name_t item; /* what the cell should hold once the animation lands */
    u32 revision;       /* tile revision observed when the claim was made */
    int frames;
    int refused; /* the server said no; correct as soon as the animation lands */
} Net_TileClaim;

static Net_TileClaim net_tile_claims[NET_TILE_CLAIM_MAX];

static int Net_TileClaimed(u32 zone, s16 x, s16 z) {
    int i;
    for (i = 0; i < NET_TILE_CLAIM_MAX; i++) {
        if (net_tile_claims[i].zone == zone && net_tile_claims[i].x == x && net_tile_claims[i].z == z) return TRUE;
    }
    return FALSE;
}

static int Net_ClaimTile(u32 zone, s16 x, s16 z, mActor_name_t item, u32 revision) {
    int i;
    if (Net_TileClaimed(zone, x, z)) return FALSE;
    for (i = 0; i < NET_TILE_CLAIM_MAX; i++) {
        if (net_tile_claims[i].zone != 0) continue;
        net_tile_claims[i].zone = zone;
        net_tile_claims[i].x = x;
        net_tile_claims[i].z = z;
        net_tile_claims[i].item = item;
        net_tile_claims[i].revision = revision;
        net_tile_claims[i].frames = NET_TILE_CLAIM_FRAMES;
        net_tile_claims[i].refused = FALSE;
        return TRUE;
    }
    return FALSE;
}

static void Net_ReleaseTileClaim(u32 zone, s16 x, s16 z) {
    int i;
    for (i = 0; i < NET_TILE_CLAIM_MAX; i++) {
        if (net_tile_claims[i].zone == zone && net_tile_claims[i].x == x && net_tile_claims[i].z == z) {
            net_tile_claims[i].zone = 0;
            return;
        }
    }
}

/* Writes the authoritative cell, overriding whatever the prediction left there.
 * Used when the server refused, superseded, or never answered a claim. */
static void Net_ForceTile(const AcNetTileState* tile) {
    mFI_UtNumtoFGSet_common((mActor_name_t)tile->item, tile->x, tile->z, TRUE);
    if (tile->buried) mFI_UtNum2DepositON(tile->x, tile->z);
    else mFI_UtNum2DepositOFF(tile->x, tile->z);
}

/* A refusal is otherwise invisible to the claim: the tile revision does not
 * move and the item never appears, so the claim would sit out its whole budget
 * showing an item the server rejected. The result says so directly. Nothing
 * else consumes it, and missing one -- the client keeps only the newest --
 * costs no more than falling back to the budget. */
static void Net_ExpireRefusedClaims(void) {
    AcNetWorldResult result;
    int i;

    while (acnet_client_take_world_result(&result)) {
        if (result.result_code == 0) continue;
        for (i = 0; i < NET_TILE_CLAIM_MAX; i++) {
            if (net_tile_claims[i].zone != result.zone_id || net_tile_claims[i].x != result.x ||
                net_tile_claims[i].z != result.z) {
                continue;
            }
            /* Not resolved here: an animation still in the air would land
             * after the correction and paint the refused item straight back.
             * The reconciler acts the moment it touches down. */
            net_tile_claims[i].refused = TRUE;
            break;
        }
    }
}

static void Net_ReconcileTileClaims(void) {
    AcNetTileState tile;
    mActor_name_t* fg_p;
    int landed;
    int expired;
    int i;

    Net_ExpireRefusedClaims();
    for (i = 0; i < NET_TILE_CLAIM_MAX; i++) {
        Net_TileClaim* claim = &net_tile_claims[i];
        if (claim->zone == 0) continue;
        if (claim->frames > 0) claim->frames--;
        expired = claim->frames == 0;

        /* The claim resolves on what the field shows: while the item is in the
         * air the cell holds the drop's RSV_NO reservation, and it holds the
         * item itself only once the drop actor has landed and written it. */
        fg_p = mFI_UtNum2UtFG(claim->x, claim->z);
        landed = fg_p != NULL && *fg_p == claim->item;
        if (!landed && !expired) continue;

        if (!acnet_client_tile(claim->zone, claim->x, claim->z, &tile)) {
            /* The interest chunk moved out from under the claim, so there is no
             * authoritative value left to compare against. The next baseline
             * covering this tile settles it. */
            if (expired) claim->zone = 0;
            continue;
        }
        if (tile.item == claim->item) {
            claim->zone = 0; /* committed, and the animation already wrote it */
            continue;
        }
        if (claim->refused || tile.revision != claim->revision || expired) {
            /* Refused, superseded, or never answered. The item the prediction
             * put on the ground is not there, and the pocket change that went
             * with it has to come back. */
            Net_ForceTile(&tile);
            last_inventory_revision = 0;
            claim->zone = 0;
        }
    }
}

int Net_BeginPredictedDrop(int ut_x, int ut_z, mActor_name_t item) {
    AcNetTileState tile;
    const u32 zone = Net_TileZone();
    if (zone == 0 || !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    return Net_ClaimTile(zone, (s16)ut_x, (s16)ut_z, item, tile.revision);
}

void Net_CancelPredictedTile(int ut_x, int ut_z) {
    const u32 zone = Net_TileZone();
    if (zone != 0) Net_ReleaseTileClaim(zone, (s16)ut_x, (s16)ut_z);
}

int Net_BeginPredictedPickup(const xyz_t* position) {
    AcNetTileState tile;
    const u32 zone = Net_TileZone();
    int ut_x;
    int ut_z;
    if (position == NULL || zone == 0 || !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    return Net_ClaimTile(zone, (s16)ut_x, (s16)ut_z, EMPTY_NO, tile.revision);
}

int Net_RequestPickup(const xyz_t* position, mActor_name_t item) {
    AcNetTileState tile;
    const u32 zone = Net_TileZone();
    int ut_x;
    int ut_z;
    if (position == NULL || zone == 0 ||
        !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile) || tile.item != item) return FALSE;
    /* No forced inventory reprojection here. It used to be the way a rejected
     * request got the pocket back, but it also undid the caller's optimistic
     * change on the very next frame -- which is the whole of the prediction.
     * Net_ReconcileTileClaims rolls back instead, and only on an actual
     * refusal. */
    return acnet_client_request_world_auto(1, zone, (s16)ut_x, (s16)ut_z, tile.revision,
                                            acnet_client_inventory_revision(), 0, item);
}

int Net_RequestDrop(int ut_x, int ut_z, mActor_name_t item) {
    AcNetTileState tile;
    AcNetItemSlot slots[15];
    const u32 zone = Net_TileZone();
    size_t count;
    size_t slot;
    if (zone == 0 || !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    for (slot = 0; slot < count; ++slot) {
        if (slots[slot].item == item) break;
    }
    if (slot == count) return FALSE;
    return acnet_client_request_world_auto(0, zone, (s16)ut_x, (s16)ut_z, tile.revision,
                                            acnet_client_inventory_revision(), (u8)slot, item);
}

/* Whether the authoritative hand holds a tool of this kind. The server asks the
 * same question of its own inventory, so a mismatch here only costs a rejected
 * request -- it can never let a request through that the server would refuse. */
static int Net_HoldingToolKind(int tool_kind) {
    const mActor_name_t item = (mActor_name_t)acnet_client_equipped_item();
    switch (tool_kind) {
        case 1: return ITEM_IS_SCOOP(item) || ITEM_IS_GOLD_SCOOP(item);
        case 2: return IS_ITEM_AXE(item) || item == ITM_GOLDEN_AXE;
        case 3: return item == ITM_ROD || item == ITM_GOLDEN_ROD;
        case 4: return item == ITM_NET || item == ITM_GOLDEN_NET;
        default: return TRUE;
    }
}

static int Net_RequestTerrain(u8 operation_type,
                              const xyz_t* position,
                              mActor_name_t item,
                              int inventory_slot,
                              int tool_kind) {
    AcNetTileState tile;
    AcNetItemSlot slots[15];
    const u32 zone = Net_TileZone();
    size_t count;
    int ut_x;
    int ut_z;
    if (position == NULL || zone == 0 ||
        !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    if (inventory_slot >= 0 && ((size_t)inventory_slot >= count || slots[inventory_slot].item != item)) return FALSE;
    if (tool_kind != 0 && !Net_HoldingToolKind(tool_kind)) return FALSE;
    last_inventory_revision = 0;
    return acnet_client_request_world_auto(operation_type, zone, (s16)ut_x, (s16)ut_z,
                                            tile.revision, acnet_client_inventory_revision(),
                                            inventory_slot < 0 ? 0 : (u8)inventory_slot, item);
}

int Net_RequestDig(const xyz_t* position) {
    return Net_RequestTerrain(2, position, EMPTY_NO, -1, 1);
}

int Net_RequestFillHole(const xyz_t* position) {
    return Net_RequestTerrain(8, position, EMPTY_NO, -1, 1);
}

int Net_RequestBury(const xyz_t* position, mActor_name_t item, int inventory_slot) {
    return Net_RequestTerrain(3, position, item, inventory_slot, 1);
}

int Net_RequestPlant(const xyz_t* position, mActor_name_t item, int inventory_slot) {
    AcNetItemSlot slots[15];
    int tool_kind = 0;
    if ((item >= ITM_FOOD_APPLE && item <= ITM_FOOD_ORANGE) || item == ITM_FOOD_COCONUT) tool_kind = 1;
    (void)slots;
    return Net_RequestTerrain(4, position, item, inventory_slot, tool_kind);
}

int Net_RequestChop(const xyz_t* position) {
    return Net_RequestTerrain(5, position, EMPTY_NO, -1, 2);
}

int Net_RequestEncounter(int kind, mActor_name_t species) {
    if (!Net_IsConnected() || (kind != 0 && kind != 1)) return FALSE;
    if (!Net_HoldingToolKind(kind == 0 ? 3 : 4)) return FALSE;
    last_inventory_revision = 0;
    if (!acnet_client_request_encounter_auto((u8)kind, (u16)species, acnet_client_inventory_revision()))
        return FALSE;
    encounter_pending++;
    return TRUE;
}

int Net_EquipmentAuthoritative(void) {
    return Net_IsConnected();
}

int Net_RequestHoldItem(int inventory_slot, mActor_name_t item) {
    if (!Net_IsConnected() || inventory_slot < 0 || inventory_slot >= mPr_POCKETS_SLOT_COUNT) return FALSE;
    if (!acnet_client_request_hold_item((u8)inventory_slot, (u16)item)) return FALSE;
    /* Whatever the server decides, the next projection must run: an accepted
     * swap arrives with a new revision, and a refusal has to undo the local
     * move the player already saw. Clearing the watermark forces both. */
    last_inventory_revision = 0;
    hold_requests_pending++;
    return TRUE;
}

/* Mirrors acnet::EconomyOpType. The wire refuses anything above HoldItem. */
#define NET_ECONOMY_BUY 0
#define NET_ECONOMY_SELL 1
#define NET_ECONOMY_DEPOSIT 2
#define NET_ECONOMY_WITHDRAW 3
#define NET_ECONOMY_PAY_DEBT 4
#define NET_ECONOMY_DONATE 5
#define NET_ECONOMY_ATTACH_MAIL 6
#define NET_ECONOMY_HOLD_ITEM 10

/* Nothing else in the game reads economy results, so they are drained here to
 * learn when a hold swap has been decided. A refusal is not reported to the
 * player: the projection simply puts the item back where the server has it. */
static void Net_UpdateHoldResults(void) {
    AcNetEconomyResult result;
    while (acnet_client_take_economy_result(&result)) {
        if (result.operation_type != NET_ECONOMY_HOLD_ITEM) continue;
        if (hold_requests_pending > 0) hold_requests_pending--;
        last_inventory_revision = 0;
    }
}

/* The original writes Private_c::equipment from several places -- the L/R tool
 * cycle, the pocket submenu's drag onto the player, closing the menu with a
 * tool in the cursor -- and each one also rewrites the pocket array. Rather
 * than hooking every one of those UI paths, the single net effect is detected
 * here and reported as the one transaction that describes it: a swap between
 * the hand and a pocket slot. That keeps the submenu state machine untouched
 * and cannot miss a path.
 *
 * Local writes are left to stand so the menu stays responsive; they are
 * optimistic, and Net_ApplyAuthoritativeState reconciles them either way. */
static void Net_ReconcileEquipment(void) {
    AcNetItemSlot slots[15];
    const mActor_name_t authoritative = (mActor_name_t)acnet_client_equipped_item();
    mActor_name_t local;
    size_t count;
    size_t i;
    if (!Net_IsConnected() || Now_Private == NULL || acnet_client_inventory_revision() == 0) return;
    /* A request already in flight is what the difference describes. Acting on
     * it again would swap twice. */
    if (hold_requests_pending != 0) return;
    local = Now_Private->equipment;
    if (local == authoritative) return;

    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    if (local != EMPTY_NO) {
        /* Equipping or swapping: the item now in hand must have come out of
         * whichever pocket the server still believes holds it. Duplicates are
         * not ambiguous -- swapping with either slot yields the same pockets. */
        for (i = 0; i < count; ++i) {
            if (slots[i].item == local) {
                (void)Net_RequestHoldItem((int)i, local);
                return;
            }
        }
        /* The server has no such item to give: the local write was not a move
         * out of a pocket at all. Let the projection put the hand back. */
        last_inventory_revision = 0;
        return;
    }
    /* Putting away: find the slot the player dropped it into, which is one the
     * server still reports as empty. */
    for (i = 0; i < count; ++i) {
        if (slots[i].item == EMPTY_NO && Now_Private->inventory.pockets[i] == authoritative) {
            (void)Net_RequestHoldItem((int)i, EMPTY_NO);
            return;
        }
    }
    for (i = 0; i < count; ++i) {
        if (slots[i].item == EMPTY_NO) {
            (void)Net_RequestHoldItem((int)i, EMPTY_NO);
            return;
        }
    }
    /* No room to stow it, so it stays in the hand. */
    last_inventory_revision = 0;
}

int Net_EncounterRecordsPending(void) {
    return Net_IsConnected() && encounter_pending > 0;
}

/* The original records the catch in the encyclopedia the moment the animation
 * starts. Online the server still has to accept the species and find pocket
 * room, so the record waits here for the accepted result and is written from
 * the item the server actually committed. A refusal leaves the encyclopedia
 * untouched rather than crediting a fish the player never received. */
static void Net_UpdateEncounters(void) {
    AcNetEncounterResult result;
    while (acnet_client_take_encounter_result(&result)) {
        if (encounter_pending > 0) encounter_pending--;
        if (result.result_code != 0 || !result.caught) continue;
        if (ITEM_IS_FISH(result.item)) mSM_COLLECT_FISH_SET(result.item - ITM_FISH_START);
        else if (ITEM_IS_INSECT(result.item)) mSM_COLLECT_INSECT_SET(result.item - ITM_INSECT_START);
    }
}

int Net_BankingAuthoritative(void) {
    return Net_IsConnected() && acnet_client_bank_revision() != 0;
}

static int Net_RequestLedgerOperation(u8 operation_type, u64 amount) {
    if (!Net_BankingAuthoritative() || amount == 0) return FALSE;
    /* Force the next authoritative apply: the accepted result is what moves the
     * money, and the overlay has already closed by then. */
    last_inventory_revision = 0;
    last_ledger_revision = 0;
    return acnet_client_request_economy_auto(operation_type, acnet_client_inventory_revision(),
                                             acnet_client_bank_revision(), 0, 0, 0, amount, 0, 0);
}

int Net_RequestBankTransfer(int amount) {
    if (amount == 0) return FALSE;
    return amount > 0 ? Net_RequestLedgerOperation(NET_ECONOMY_DEPOSIT, (u64)amount)
                      : Net_RequestLedgerOperation(NET_ECONOMY_WITHDRAW, (u64)(-(s64)amount));
}

int Net_RequestPayDebt(u32 amount) {
    return Net_RequestLedgerOperation(NET_ECONOMY_PAY_DEBT, amount);
}

int Net_MailAuthoritative(void) {
    return Net_IsConnected() && acnet_client_mailbox_revision() != 0 && Net_ResidentSlot() >= 0;
}

u64 Net_MailboxMailId(int index) {
    return index >= 0 && index < HOME_MAILBOX_SIZE ? projected_mailbox_ids[index] : 0;
}

u64 Net_CarriedMailId(int index) {
    return index >= 0 && index < mPr_INVENTORY_MAIL_COUNT ? projected_carried_ids[index] : 0;
}

/* The accepted result is what moves a letter, so the projections have to be
 * rebuilt from it rather than from the state that was current when the player
 * pressed the button. */
static int Net_MailRequestReady(u64 mail_id) {
    if (!Net_MailAuthoritative() || mail_id == 0) return FALSE;
    last_inventory_revision = 0;
    last_mail_revision = 0;
    return TRUE;
}

int Net_RequestTakeMail(u64 mail_id) {
    return Net_MailRequestReady(mail_id) && acnet_client_take_mail(mail_id);
}

int Net_RequestClaimMail(u64 mail_id) {
    return Net_MailRequestReady(mail_id) && acnet_client_claim_mail(mail_id);
}

int Net_RequestDiscardMail(u64 mail_id) {
    return Net_MailRequestReady(mail_id) && acnet_client_discard_mail(mail_id);
}

/* One authoritative letter becomes one original Mail_c. The text fields are
 * carried as opaque bytes in the game's own encoding, and sender_name is a raw
 * Mail_nm_c, so nothing here reinterprets content -- it is a straight copy into
 * the layout the letter UI already reads. */
static void Net_ProjectMail(Mail_c* destination, const AcNetMailRecord* source) {
    mMl_clear_mail(destination);
    destination->present = (mActor_name_t)source->attachment;
    destination->content.font = source->font;
    destination->content.mail_type = source->mail_type;
    destination->content.paper_type = source->paper_type;
    destination->content.header_back_start = source->header_back_start;
    memcpy(destination->content.header, source->header, MAIL_HEADER_LEN);
    memcpy(destination->content.body, source->body, MAIL_BODY_LEN);
    memcpy(destination->content.footer, source->footer, MAIL_FOOTER_LEN);
    memcpy(&destination->header.sender, source->sender_name, sizeof(Mail_nm_c));
    if (Now_Private != NULL) mMl_set_to_plname(destination, &Now_Private->player_ID);
}

/* The house mailbox and the carried mail array are windows onto server state.
 * They are rebuilt whole whenever the mail revision moves, so a letter cannot
 * be taken, emptied, or thrown away except through a transaction. */
static void Net_ApplyAuthoritativeMail(void) {
    AcNetMailRecord letters[ACNET_MAILBOX_CAPACITY + ACNET_CARRIED_MAIL_CAPACITY];
    size_t count;
    size_t i;
    int mailbox_used = 0;
    int carried_used = 0;
    const int slot = Net_ResidentSlot();

    if (slot < 0 || Now_Private == NULL) return;
    count = acnet_client_mail(letters, ARRAY_COUNT(letters));
    memset(projected_mailbox_ids, 0, sizeof(projected_mailbox_ids));
    memset(projected_carried_ids, 0, sizeof(projected_carried_ids));
    mMl_clear_mail_box(Save_Get(homes[slot]).mailbox, HOME_MAILBOX_SIZE);
    mMl_clear_mail_box(Now_Private->mail, mPr_INVENTORY_MAIL_COUNT);
    for (i = 0; i < count; ++i) {
        if (letters[i].location == ACNET_MAIL_CARRIED) {
            if (carried_used >= mPr_INVENTORY_MAIL_COUNT) continue;
            Net_ProjectMail(&Now_Private->mail[carried_used], &letters[i]);
            projected_carried_ids[carried_used] = letters[i].id;
            ++carried_used;
        } else {
            if (mailbox_used >= HOME_MAILBOX_SIZE) continue;
            Net_ProjectMail(&Save_Get(homes[slot]).mailbox[mailbox_used], &letters[i]);
            projected_mailbox_ids[mailbox_used] = letters[i].id;
            ++mailbox_used;
        }
    }
}

/* The town baseline is windowed to a 16x16 interest chunk; the island is only
 * two acres and arrives whole, at exactly 2 * UT_X_NUM * UT_Z_NUM tiles. The
 * mirror also carries tiles that arrived as deltas from outside the window, so
 * this must hold the client's whole mirror (ClientRuntime::kMaxMirroredTiles)
 * and not just a baseline: sized to the island alone, a single out-of-window
 * delta would push the newest entries past the end and they would be dropped
 * without a word. */
static AcNetTileState net_baseline_tiles[1024];

/* Drained per frame. Small on purpose: the drain removes exactly what it copies,
 * so a burst just takes several frames to work through. */
static AcNetTileChange net_tile_changes[32];

/* One tile change that arrived as a delta. Returns TRUE when it was consumed by
 * an animation and must not also be painted in. */
static int Net_AnimateTileChange(GAME_PLAY* play, const AcNetTileChange* change) {
    AcNetRemotePlayer states[16];
    size_t count;
    size_t i;
    xyz_t source;

    /* Only a drop has an animation to play, and only someone else's: the local
     * player's own drop was already animated at request time. */
    if (change->cause != ACNET_TILE_CAUSE_DROP || change->tile.item == EMPTY_NO ||
        change->actor_account == 0 || change->actor_account == acnet_client_account()) {
        return FALSE;
    }
    if (Common_Get(clip).bg_item_clip == NULL || Common_Get(clip).bg_item_clip->net_remote_drop_entry_proc == NULL) {
        return FALSE;
    }
    /* The arc has to come from somewhere. If the dropper is not in this
     * viewer's interest set there is no hand to throw from, so the item simply
     * appears -- which is what happens today for every drop. */
    count = acnet_client_remote_players(states, ARRAY_COUNT(states));
    for (i = 0; i < count; ++i) {
        if (states[i].account_id != change->actor_account) continue;
        if (states[i].zone_id != Net_SceneZone(play->scene_id)) return FALSE;
        source.x = states[i].transform.x;
        source.y = states[i].transform.y + 50.0f; /* the hand, as the local drop path measures it */
        source.z = states[i].transform.z;
        if (!Net_ClaimTile(change->tile.zone_id, change->tile.x, change->tile.z,
                           (mActor_name_t)change->tile.item, change->tile.revision)) {
            return FALSE;
        }
        if (Common_Get(clip).bg_item_clip->net_remote_drop_entry_proc((mActor_name_t)change->tile.item, &source,
                                                                     change->tile.x, change->tile.z)) {
            return TRUE;
        }
        /* The drop table was full. Release the claim so the projection places
         * the item instead of leaving the cell empty for the whole budget. */
        Net_ReleaseTileClaim(change->tile.zone_id, change->tile.x, change->tile.z);
        return FALSE;
    }
    return FALSE;
}

int Net_EconomyAuthoritative(void) {
    return Net_IsConnected() && acnet_client_inventory_revision() != 0;
}

/* Sell the submenu's selected items as one server transaction. The counter
 * sells a whole selection and quotes a single total, so the slots go over as a
 * mask rather than one request each -- separate requests would all quote the
 * same inventory revision and every one after the first would be refused as
 * stale.
 *
 * Nothing is mutated locally. Net_ApplyAuthoritativeState projects the wallet,
 * the emptied slots, and any overflow money bags once the result lands. */
int Net_RequestSellItems(GAME_PLAY* play, int count) {
    Submenu_Item_c* item_p;
    u16 slot_mask = 0;
    int i;
    if (!Net_EconomyAuthoritative() || play == NULL || count <= 0) return FALSE;
    item_p = play->submenu.item_p;
    if (item_p == NULL) return FALSE;
    for (i = 0; i < count; ++i) {
        int slot = item_p[i].slot_no;
        if (slot < 0 || slot >= mPr_POCKETS_SLOT_COUNT) continue;
        if (Now_Private->inventory.pockets[slot] == EMPTY_NO) continue;
        slot_mask |= (u16)(1U << slot);
    }
    if (slot_mask == 0) return FALSE;
    if (!acnet_client_request_sell(slot_mask)) return FALSE;
    /* Force the next projection: the accepted result is what moves the money,
     * and the counter has already closed by the time it arrives. */
    last_inventory_revision = 0;
    return TRUE;
}

/* Buy `item` off the shelf. The server names a row by index, so the index is
 * recovered from the projected shelf -- which is the server's own list, so the
 * lookup cannot drift. Duplicated rows are interchangeable, hence first match.
 *
 * Only the payment is redirected. The item still arrives through the usual
 * hand-over actor, optimistically, and Net_ApplyAuthoritativeState reconciles
 * the pockets and the wallet when the result lands. */
int Net_RequestBuyItem(mActor_name_t item) {
    mActor_name_t* items;
    int i;
    if (!Net_ShopStockAuthoritative() || !Net_EconomyAuthoritative() || item == EMPTY_NO) return FALSE;
    items = Save_Get(shop).items;
    for (i = 0; i < mSP_GOODS_COUNT; ++i) {
        if (items[i] != item) continue;
        if (!acnet_client_request_economy_auto(NET_ECONOMY_BUY, acnet_client_inventory_revision(),
                                               acnet_client_shop_revision(), (u32)i, 0, (u16)item, 0, 0, 0))
            return FALSE;
        last_inventory_revision = 0;
        return TRUE;
    }
    return FALSE;
}

/* Donate the pocket item at `slot` to the museum. One town has one collection,
 * so the server decides whether the species is already displayed; the local
 * pocket clear is skipped and the projection does it once the result lands. */
int Net_RequestDonate(int inventory_slot, mActor_name_t item) {
    if (!Net_EconomyAuthoritative() || inventory_slot < 0 || inventory_slot >= mPr_POCKETS_SLOT_COUNT ||
        item == EMPTY_NO)
        return FALSE;
    if (!acnet_client_request_economy_auto(NET_ECONOMY_DONATE, acnet_client_inventory_revision(),
                                           acnet_client_museum_revision(), 0, (u8)inventory_slot, (u16)item, 0, 0, 0))
        return FALSE;
    last_inventory_revision = 0;
    return TRUE;
}

int Net_ShopStockAuthoritative(void) {
    return Net_IsConnected() && acnet_client_shop_revision() != 0;
}

/* Copy the server's shelf into Save_t so every UI that reads Save_Get(shop)
 * -- the counter, the catalogue check, the sale report -- sees the same rows
 * the server will validate a purchase against. The row index is the contract:
 * NET_ECONOMY_BUY names a position in this array.
 *
 * Rows past the server's count are cleared rather than left alone, so a
 * shrinking shelf cannot leave yesterday's item buyable at the end. */
void Net_ApplyAuthoritativeShopStock(void) {
    AcNetShopEntry stock[mSP_GOODS_COUNT];
    mActor_name_t* items;
    size_t count;
    size_t i;
    if (!Net_ShopStockAuthoritative()) return;
    count = acnet_client_shop_stock(stock, ARRAY_COUNT(stock));
    items = Save_Get(shop).items;
    for (i = 0; i < ARRAY_COUNT(stock); ++i) {
        items[i] = i < count ? (mActor_name_t)stock[i].item : EMPTY_NO;
    }
    Save_Set(shop.rare_item, (mActor_name_t)acnet_client_shop_rare_item());
    /* The store's level and the lifetime total that earned it. Both are
     * projections online: mSP_PlusSales is refused locally (see below), so
     * without this the level would never move and Nook's Cranny would never
     * upgrade -- and the level drives the shelf size, the closing time, and
     * which building the scene loads.
     *
     * mSP_RenewShopLevel is not called: it recomputes the level from the local
     * total through mSP_GetRealShopLevel, which is exactly the derivation the
     * server now owns. Writing shop_level directly is the projection. */
    Save_Set(shop.sales_sum, acnet_client_shop_sales_sum());
    Save_Set(shop.shop_info.shop_level, (u8)acnet_client_shop_tier());
}

/* Whether a server tile may be written into the local field.
 *
 * Houses, boards, gyroids, props and misc actors live in the foreground grid
 * only as spawn records: ac_birth_control reads the name, spawns the actor, and
 * clears the cell to RSV_NO or EMPTY_NO until the actor is deleted
 * (actor->restore_fg puts the name back). The server's copy holds the names --
 * it was bootstrapped from a save -- and it has no transaction that ever
 * changes them. Writing one back while the actor is alive makes birth control
 * spawn a duplicate; enough duplicates exhaust the fixed structure slot pool,
 * and a failed spawn leaves setup_actor_flag set so the whole scan reruns every
 * frame. That was the doubled house gyroids and the hitching.
 *
 * RSV_NO itself is local bookkeeping (multi-tile reserves and actor claims),
 * never an authoritative state, in either direction. */
static int Net_TileProjectable(const mActor_name_t* local_cell, u16 incoming) {
    if (incoming == RSV_NO) return FALSE;
    if (local_cell != NULL && *local_cell == RSV_NO) return FALSE;
    switch (ITEM_NAME_GET_TYPE((mActor_name_t)incoming)) {
        case NAME_TYPE_STRUCT:
        case NAME_TYPE_PROPS:
        case NAME_TYPE_ITEM2:
        case NAME_TYPE_ACTOR:
            return FALSE;
    }
    return TRUE;
}

int Net_GyroidAuthoritative(void) {
    return Net_IsConnected() && acnet_client_gyroid_serial() != 0;
}

int Net_TownTuneAuthoritative(void) {
    return Net_IsConnected() && acnet_client_town_tune_revision() != 0;
}

int Net_RequestTownTune(u64 notes) {
    if (!Net_TownTuneAuthoritative()) return FALSE;
    return acnet_client_request_town_tune((uint64_t)notes);
}

/* The town's tune, projected into the save the original plays from. Also drains
 * the request result, which matters only for a refusal: a stale-revision
 * rejection carries the tune that won, and the projection below writes it. */
void Net_ApplyAuthoritativeTownTune(void) {
    uint16_t code = 0;
    uint64_t notes = 0;
    u64 authoritative;

    if (!Net_IsConnected()) return;
    while (acnet_client_take_town_tune_result(&code, &notes)) {
    }
    if (acnet_client_town_tune_revision() == 0) return;
    authoritative = (u64)acnet_client_town_tune();
    if (Save_Get(melody) != authoritative) Save_Set(melody, authoritative);
}

/* The locally generated villagers, handed to a town that has none yet. The
 * server holds no name, species or personality tables -- and is not allowed to
 * -- so the first resident's town generation is the only possible source. Once
 * the server owns a roster this is ignored, and the projection below is what
 * every client reads instead. */
/* One villager, save form to wire form. Shared by the bootstrap capture and the
 * move-in offer so the two cannot describe the same villager differently. */
static int Net_VillagerToWire(const Animal_c* animal, AcNetVillager* wire) {
    memset(wire, 0, sizeof(*wire));
    /* mNpc_LOOKS_UNSET in the personality, or no character at all, is how the
     * original marks an empty slot. */
    if (animal->id.npc_id == EMPTY_NO || animal->id.looks >= mNpc_LOOKS_UNSET) return FALSE;
    wire->occupied = 1;
    wire->npc_id = (uint16_t)animal->id.npc_id;
    wire->land_id = animal->id.land_id;
    memcpy(wire->land_name, animal->id.land_name, ACNET_VILLAGER_NAME_BYTES);
    wire->name_id = animal->id.name_id;
    wire->looks = animal->id.looks;
    wire->home_block_x = animal->home_info.block_x;
    wire->home_block_z = animal->home_info.block_z;
    wire->home_ut_x = animal->home_info.ut_x;
    wire->home_ut_z = animal->home_info.ut_z;
    memcpy(wire->catchphrase, animal->catchphrase, ACNET_VILLAGER_CATCHPHRASE_BYTES);
    wire->cloth = (uint16_t)animal->cloth;
    wire->present_cloth = (uint16_t)animal->present_cloth;
    wire->cloth_original_id = animal->cloth_original_id;
    wire->umbrella_id = animal->umbrella_id;
    wire->mood = animal->mood;
    wire->mood_time = animal->mood_time;
    wire->is_home = animal->is_home;
    wire->moved_in = animal->moved_in;
    wire->removing = animal->removing;
    wire->previous_land_id = animal->previous_land_id;
    memcpy(wire->previous_land_name, animal->anmuni.previous_land_name, ACNET_VILLAGER_NAME_BYTES);
    memcpy(wire->parent_name, animal->parent_name, ACNET_VILLAGER_NAME_BYTES);
    memcpy(wire->relations, animal->animal_relations, ACNET_VILLAGER_SLOTS);
    return TRUE;
}

/* The roster slot an Animal_c occupies, by its address inside Save_t.animals[].
 * The NPC actor holds a pointer into that array, so the slot is derivable
 * without a lookup or an added field. */
int Net_VillagerSlotOf(const void* animal) {
    const Animal_c* base = &Save_Get(animals[0]);
    const Animal_c* target = (const Animal_c*)animal;
    ptrdiff_t index;

    if (animal == NULL) return -1;
    index = target - base;
    if (index < 0 || index >= ANIMAL_NUM_MAX) return -1;
    return (int)index;
}

int Net_VillagerBusy(int slot) {
    u64 owner;

    if (!Net_VillagersAuthoritative() || slot < 0 || slot >= ACNET_VILLAGER_SLOTS) return FALSE;
    owner = (u64)acnet_client_villager_conversation_owner((uint8_t)slot);
    /* Our own lease is not busy -- the original re-enters the talk state for a
     * continued conversation, and refusing that would end it a line in. */
    return owner != 0 && owner != (u64)acnet_client_account();
}

void Net_BeginVillagerTalk(int slot) {
    if (!Net_VillagersAuthoritative() || slot < 0 || slot >= ACNET_VILLAGER_SLOTS) return;
    (void)acnet_client_begin_villager_conversation((uint8_t)slot);
}

void Net_EndVillagerTalk(int slot) {
    if (!Net_VillagersAuthoritative() || slot < 0 || slot >= ACNET_VILLAGER_SLOTS) return;
    (void)acnet_client_end_villager_conversation((uint8_t)slot);
}

int Net_VillagerMoveInPending(u8* slot, u32* seed) {
    uint8_t wire_slot = 0;
    uint32_t wire_seed = 0;

    if (!Net_VillagersAuthoritative()) return FALSE;
    if (!acnet_client_villager_move_in(&wire_slot, &wire_seed)) return FALSE;
    if (slot != NULL) *slot = (u8)wire_slot;
    if (seed != NULL) *seed = (u32)wire_seed;
    return TRUE;
}

int Net_OfferVillagerMoveIn(int slot, int local_index) {
    AcNetVillager wire;

    if (!Net_VillagersAuthoritative() || slot < 0 || slot >= ACNET_VILLAGER_SLOTS ||
        local_index < 0 || local_index >= ANIMAL_NUM_MAX) return FALSE;
    if (!Net_VillagerToWire(&Save_Get(animals[local_index]), &wire)) return FALSE;
    /* Whatever the server does with this -- accept it into its own slot, or
     * refuse it because another client got there first -- the next projection
     * settles where the newcomer actually lives, so the local roll's placement
     * does not have to be undone here. */
    return acnet_client_request_villager_move_in((uint8_t)slot, &wire);
}

static void Net_CaptureVillagers(void) {
    AcNetVillager wire[ACNET_VILLAGER_SLOTS];
    int i;

    memset(wire, 0, sizeof(wire));
    for (i = 0; i < ANIMAL_NUM_MAX && i < ACNET_VILLAGER_SLOTS; ++i) {
        (void)Net_VillagerToWire(&Save_Get(animals[i]), &wire[i]);
    }
    (void)acnet_client_submit_villagers(wire);
}

int Net_VillagersAuthoritative(void) {
    return Net_IsConnected() && acnet_client_villager_revision() != 0;
}

/* The town's neighbours, projected into the save the original reads through.
 *
 * Only the roster is projected -- who lives here, what they look like, where
 * their house is, what they are wearing. Animal_c::memories is deliberately
 * left alone: it is the per-player relationship record, seven eighths of the
 * struct, and it is account-scoped rather than town-scoped. Overwriting it from
 * a town-wide roster would hand every player the same friendships. */
void Net_ApplyAuthoritativeVillagers(void) {
    static u32 last_villager_revision = 0;
    AcNetVillager wire[ACNET_VILLAGER_SLOTS];
    u32 revision;
    uint16_t code = 0;
    int i;

    if (!Net_IsConnected()) return;
    /* A refusal changes nothing authoritative, so it would not move the
     * revision and the optimistic local roll would sit there uncorrected.
     * Forcing a reprojection is what takes it back. */
    while (acnet_client_take_villager_result(&code)) {
        last_villager_revision = 0;
    }
    revision = acnet_client_villager_revision();
    if (revision == 0) return;
    if (!acnet_client_villagers(wire)) return;

    /* A villager the local game has decided is leaving, whom the server does
     * not know about yet. Detected by diffing rather than by hooking the
     * dialogue: the original sets `removing` from several places -- a
     * town-transfer, a conversation -- and a diff catches all of them without
     * having to find each one. The server empties the slot at its next daily
     * turnover; nothing is removed here. */
    for (i = 0; i < ANIMAL_NUM_MAX && i < ACNET_VILLAGER_SLOTS; ++i) {
        if (!wire[i].occupied || wire[i].removing) continue;
        if (Save_Get(animals[i]).removing != 0 &&
            Save_Get(animals[i]).id.npc_id == (mActor_name_t)wire[i].npc_id) {
            (void)acnet_client_request_villager_move_out((uint8_t)i);
        }
    }

    if (revision == last_villager_revision) return;

    for (i = 0; i < ANIMAL_NUM_MAX && i < ACNET_VILLAGER_SLOTS; ++i) {
        Animal_c* animal = &Save_Get(animals[i]);
        const mActor_name_t incoming = wire[i].occupied ? (mActor_name_t)wire[i].npc_id : EMPTY_NO;

        /* A different character in this slot means the previous occupant is
         * gone. Everything else in the entry -- the memories of who has spoken
         * to them, the contest quest, the stored mail -- belongs to *them*, not
         * to whoever moves in next, so it goes with them. mNpc_ClearAnimalInfo
         * is what the original calls for exactly this, and using it keeps the
         * cleared state identical to a local move-out rather than an
         * approximation of one.
         *
         * Only on a change: doing it every projection would wipe a player's
         * relationships every time any villager anywhere altered the roster. */
        if (animal->id.npc_id != incoming) {
            mNpc_ClearAnimalInfo(animal);
        }
        if (!wire[i].occupied) continue;

        animal->id.npc_id = (mActor_name_t)wire[i].npc_id;
        animal->id.land_id = wire[i].land_id;
        memcpy(animal->id.land_name, wire[i].land_name, LAND_NAME_SIZE);
        animal->id.name_id = wire[i].name_id;
        animal->id.looks = wire[i].looks;
        animal->home_info.block_x = wire[i].home_block_x;
        animal->home_info.block_z = wire[i].home_block_z;
        animal->home_info.ut_x = wire[i].home_ut_x;
        animal->home_info.ut_z = wire[i].home_ut_z;
        memcpy(animal->catchphrase, wire[i].catchphrase, ANIMAL_CATCHPHRASE_LEN);
        animal->cloth = (mActor_name_t)wire[i].cloth;
        animal->present_cloth = (mActor_name_t)wire[i].present_cloth;
        animal->cloth_original_id = wire[i].cloth_original_id;
        animal->umbrella_id = wire[i].umbrella_id;
        animal->mood = wire[i].mood;
        animal->mood_time = wire[i].mood_time;
        animal->is_home = wire[i].is_home;
        animal->moved_in = wire[i].moved_in;
        animal->removing = wire[i].removing;
        animal->previous_land_id = wire[i].previous_land_id;
        memcpy(animal->anmuni.previous_land_name, wire[i].previous_land_name, LAND_NAME_SIZE);
        memcpy(animal->parent_name, wire[i].parent_name, PLAYER_NAME_LEN);
        memcpy(animal->animal_relations, wire[i].relations, ANIMAL_NUM_MAX);
    }
    {
        /* mNpc_CheckGrow and the roster UI both read this, and it is derived
         * rather than independent state, so it follows the projection. */
        u8 occupied = 0;
        for (i = 0; i < ANIMAL_NUM_MAX && i < ACNET_VILLAGER_SLOTS; ++i) {
            if (wire[i].occupied) ++occupied;
        }
        Save_Set(now_npc_max, occupied);
    }
    last_villager_revision = revision;
}

int Net_NoticeBoardAuthoritative(void) {
    return Net_IsConnected() && acnet_client_notice_revision() != 0;
}

/* The post is handed over as raw bytes because m_net_hooks.c is the only file
 * that may see acnet types and m_notice.c is the only one that may see
 * mNtc_board_post_c. Both agree on the layout -- 192 message bytes then the
 * 8-byte lbRTC_time_c -- and the size is checked rather than assumed. */
int Net_RequestNoticePost(const void* post, u32 size) {
    AcNetNoticePost wire;

    if (!Net_NoticeBoardAuthoritative() || post == NULL ||
        size != ACNET_NOTICE_MESSAGE_BYTES + ACNET_NOTICE_TIME_BYTES) return FALSE;
    memcpy(wire.message, post, ACNET_NOTICE_MESSAGE_BYTES);
    memcpy(wire.posted_time, (const u8*)post + ACNET_NOTICE_MESSAGE_BYTES, ACNET_NOTICE_TIME_BYTES);
    return acnet_client_request_notice_post(&wire);
}

/* The board, projected into the save the original reads. The authoritative list
 * is dense and oldest-first; the original marks the end of the board with a
 * post whose timestamp equals the clear code, so the tail is filled with that
 * rather than zeroed -- mNtc_notice_write_num counts until it finds one. */
void Net_ApplyAuthoritativeNotices(void) {
    static u32 last_notice_revision = 0;
    AcNetNoticePost posts[ACNET_NOTICE_POSTS];
    mNtc_board_post_c* board;
    u32 revision;
    size_t count;
    size_t i;
    uint16_t code = 0;

    if (!Net_IsConnected()) return;
    while (acnet_client_take_notice_result(&code)) {
    }
    revision = acnet_client_notice_revision();
    if (revision == 0 || revision == last_notice_revision) return;

    count = acnet_client_notices(posts, ARRAY_COUNT(posts));
    board = Save_Get(noticeboard);
    for (i = 0; i < ACNET_NOTICE_POSTS; ++i) {
        if (i < count) {
            memcpy(board[i].message, posts[i].message, sizeof(board[i].message));
            memcpy(&board[i].post_time, posts[i].posted_time, sizeof(board[i].post_time));
        } else {
            memset(board[i].message, 0, sizeof(board[i].message));
            board[i].post_time = mTM_rtcTime_clear_code;
        }
    }
    last_notice_revision = revision;
}

int Net_TurnipMarketAuthoritative(void) {
    return Net_IsConnected() && acnet_client_has_turnip_market();
}

u32 Net_TurnipSellPrice(mActor_name_t item) {
    if (!Net_TurnipMarketAuthoritative()) return 0;
    return acnet_client_turnip_price((uint16_t)item);
}

/* The town's stalk market, projected into the save the original reads through.
 * Kabu_get_price indexes daily_price by today's weekday, and the shop dialogue
 * quotes Sunday's entry when Joan is selling, so writing the whole week here
 * means neither has to learn about the network.
 *
 * Only the prices are projected. trade_market is the server's business -- it
 * decides next week's trend from this week's -- and update_time exists to tell
 * a local roller whether a reroll is due, which is precisely what must not
 * happen while connected. */
void Net_ApplyAuthoritativeTurnipMarket(void) {
    uint16_t schedule[7];
    int day;

    if (!Net_IsConnected() || !acnet_client_turnip_schedule(schedule)) return;
    for (day = 0; day < lbRTC_WEEKDAYS_MAX && day < 7; ++day) {
        Save_Set(kabu_price_schedule.daily_price[day], schedule[day]);
    }
}

/* A guest takes display slot `item_slot` from house `house_idx`'s gyroid,
 * paying its price if it is for sale. The submenu's local mutation runs
 * optimistically; forcing both projections makes the server's verdict land
 * either way -- the accepted delta repaints the taken state, and a refusal
 * repaints the untouched one. */
int Net_RequestGyroidTake(int house_idx, int item_slot, mActor_name_t item) {
    if (!Net_GyroidAuthoritative() || !Net_EconomyAuthoritative()) return FALSE;
    if (house_idx < 0 || house_idx >= PLAYER_NUM || item_slot < 0 || item_slot >= HANIWA_ITEM_HOLD_NUM) return FALSE;
    if (!acnet_client_request_gyroid_take((u32)house_idx, (u32)item_slot, (u16)item)) return FALSE;
    last_inventory_revision = 0;
    last_gyroid_serial = 0;
    return TRUE;
}

int Net_RequestGyroidCollect(int house_idx) {
    if (!Net_GyroidAuthoritative() || !Net_EconomyAuthoritative()) return FALSE;
    if (house_idx < 0 || house_idx >= PLAYER_NUM) return FALSE;
    if (!acnet_client_request_gyroid_collect((u32)house_idx)) return FALSE;
    last_inventory_revision = 0;
    last_gyroid_serial = 0;
    return TRUE;
}

static u64 Net_HashGyroidBlock(const Haniwa_c* haniwa) {
    u64 hash = 14695981039346656037ULL;
    int i;
    for (i = 0; i < HANIWA_ITEM_HOLD_NUM; ++i) {
        hash = Net_HashBytes(hash, &haniwa->items[i].item, sizeof(haniwa->items[i].item));
        hash = Net_HashBytes(hash, &haniwa->items[i].exchange_type, sizeof(haniwa->items[i].exchange_type));
        hash = Net_HashBytes(hash, &haniwa->items[i].extra_data, sizeof(haniwa->items[i].extra_data));
    }
    return Net_HashBytes(hash, haniwa->message, HANIWA_MESSAGE_LEN);
}

/* Never written; hashed as the shape of a gyroid the server has no data for. */
static Haniwa_c net_virgin_haniwa;

/* Copy the server's four gyroids into Save_t.homes[].haniwa, where the gyroid
 * actor, the tag overlay and the message board all read them. Skipped while a
 * submenu is open: the owner may be mid-edit, and repainting under the drag
 * hand is the same hazard the inventory projection avoids. */
static void Net_ApplyAuthoritativeGyroids(GAME_PLAY* play) {
    AcNetGyroidState state;
    u32 serial;
    int slot;
    int my_slot;
    serial = acnet_client_gyroid_serial();
    if (serial == 0 || serial == last_gyroid_serial || play->submenu.open_flag) return;
    my_slot = Net_ResidentSlot();
    for (slot = 0; slot < PLAYER_NUM; ++slot) {
        Haniwa_c* haniwa;
        int i;
        int virgin;
        if (!acnet_client_gyroid((u32)slot, &state)) continue;
        haniwa = &Save_Get(homes[slot]).haniwa;
        virgin = state.revision == 1 && state.bells == 0;
        for (i = 0; virgin && i < HANIWA_ITEM_HOLD_NUM; ++i) virgin = state.items[i].item == 0;
        for (i = 0; virgin && i < HANIWA_MESSAGE_LEN; ++i) virgin = state.message[i] == 0;
        if (slot == my_slot && virgin) {
            /* First contact with a gyroid the server has never seen written.
             * Projecting it would blank the game's default greeting on every
             * client; keeping the local block and marking the virgin shape as
             * "projected" makes the watch below upload it instead. */
            gyroid_projected_hash = Net_HashGyroidBlock(&net_virgin_haniwa);
            continue;
        }
        for (i = 0; i < HANIWA_ITEM_HOLD_NUM; ++i) {
            haniwa->items[i].item = (mActor_name_t)state.items[i].item;
            haniwa->items[i].exchange_type = (s16)state.items[i].exchange;
            haniwa->items[i].extra_data = state.items[i].price;
        }
        memcpy(haniwa->message, state.message, HANIWA_MESSAGE_LEN);
        haniwa->bells = state.bells;
        if (slot == my_slot) gyroid_projected_hash = Net_HashGyroidBlock(haniwa);
    }
    gyroid_submitted_hash = 0;
    last_gyroid_serial = serial;
}

/* Submit the local player's own gyroid whole once its block no longer matches
 * what was projected or last sent. Every mutation path -- the tag overlay, the
 * drag hand, the message editor -- settles into the save before the submenu
 * closes, so one falling-edge watch covers them all without touching the
 * overlay code. Bells are excluded from the hash and from the update. */
static void Net_SubmitGyroidIfEdited(GAME_PLAY* play) {
    AcNetGyroidItem items[HANIWA_ITEM_HOLD_NUM];
    const Haniwa_c* haniwa;
    u64 hash;
    int i;
    int my_slot = Net_ResidentSlot();
    if (my_slot < 0 || my_slot >= PLAYER_NUM || play->submenu.open_flag || !Net_GyroidAuthoritative()) return;
    haniwa = &Save_Get(homes[my_slot]).haniwa;
    hash = Net_HashGyroidBlock(haniwa);
    if (hash == gyroid_projected_hash || hash == gyroid_submitted_hash) return;
    for (i = 0; i < HANIWA_ITEM_HOLD_NUM; ++i) {
        items[i].item = (u16)haniwa->items[i].item;
        items[i].exchange = (u8)haniwa->items[i].exchange_type;
        items[i].price = haniwa->items[i].extra_data;
        /* Normalise what the codec would refuse: an emptied slot keeps its old
         * terms locally, the UI maps a zero price to "free" itself, and the
         * unused fourth trade type defaults to display-only so nothing is
         * given away by accident. */
        if (items[i].item == 0) {
            items[i].exchange = 0;
            items[i].price = 0;
        } else if (items[i].exchange > 2 || (items[i].exchange == 2 && items[i].price == 0)) {
            items[i].exchange = items[i].exchange == 2 ? 0 : 1;
            items[i].price = 0;
        } else if (items[i].exchange != 2) {
            items[i].price = 0;
        }
    }
    if (acnet_client_request_gyroid_update((u32)my_slot, items, haniwa->message)) {
        gyroid_submitted_hash = hash;
        last_inventory_revision = 0;
    }
}

/* Whether the item submenu's drag hand is holding something. While it is, the
 * grabbed item exists in neither the hand slot nor any pocket -- it lives only
 * on the cursor -- so both the reconciler and the inventory projection would
 * misread the state. The reconciler would stow the "put away" tool into an
 * arbitrary slot mid-drag, and the projection would paint the same item back
 * into a pocket while the player is still carrying it on the cursor; dropping
 * it then makes two. Grabbing your own fishing rod off the character portrait
 * and putting it in the pockets produced exactly that pair. Both paths wait
 * until the cursor is empty; the game always settles the drag into a pocket or
 * back into the hand, and the projection reconciles from that stable state. */
static int Net_InventoryDragActive(GAME_PLAY* play) {
    mHD_Ovl_c* hand_ovl;
    if (play == NULL || play->submenu.overlay == NULL) return FALSE;
    hand_ovl = play->submenu.overlay->hand_ovl;
    return hand_ovl != NULL && hand_ovl->info.item != EMPTY_NO;
}

static void Net_ApplyAuthoritativeState(GAME_PLAY* play) {
    AcNetItemSlot slots[15];
    u32 baseline_serial;
    u32 inventory_revision;
    u32 ledger_revision;
    u32 mail_revision;
    u32 shop_revision;
    u32 tile_zone;
    size_t count;
    size_t i;
    int outdoor;
    int dirty;

    if (acnet_client_status() != ACNET_CONNECTED) return;
    baseline_serial = acnet_client_baseline_serial();
    tile_zone = acnet_client_baseline_zone();
    outdoor = play->scene_id == SCENE_FG && (tile_zone == 1 || tile_zone == NET_ZONE_ISLAND);
    if (outdoor) Net_ReconcileTileClaims();
    /* Both outdoor zones apply the same way: the island acres are part of the
     * same unit grid, and mFI_UtNumtoFGSet_common already routes a write at an
     * island acre into Save_t.island.fgblock, because mFM_SetFgUtPtoSaveData
     * pointed that block's item array there when the field was built.
     *
     * Keyed on the baseline serial, not on the replication revision: the
     * revision moves for every delta of every kind, so a nearby player changing
     * animation used to rewrite the whole chunk and rebuild the field's draw
     * and collision tables with it. A dropped change is the other reason to
     * reproject -- the queue overflowed, so the individual changes are gone. */
    if (outdoor && baseline_serial != 0 &&
        (baseline_serial != last_baseline_serial || acnet_client_tile_changes_overflowed())) {
        count = acnet_client_baseline_tiles(net_baseline_tiles, ARRAY_COUNT(net_baseline_tiles));
        dirty = FALSE;
        for (i = 0; i < count; ++i) {
            mActor_name_t* cell;
            if (net_baseline_tiles[i].zone_id != tile_zone) continue;
            if (Net_TileClaimed(net_baseline_tiles[i].zone_id, net_baseline_tiles[i].x,
                                net_baseline_tiles[i].z)) continue;
            cell = mFI_UtNum2UtFG(net_baseline_tiles[i].x, net_baseline_tiles[i].z);
            if (!Net_TileProjectable(cell, net_baseline_tiles[i].item)) continue;
            /* The baseline follows the player, so most of it repeats what the
             * field already shows. Only a real change may set the update flag:
             * it rebuilds the draw and collision tables, and doing that on
             * every re-baseline made walking hitch. */
            if (cell == NULL || *cell != (mActor_name_t)net_baseline_tiles[i].item) {
                mFI_UtNumtoFGSet_common(net_baseline_tiles[i].item, net_baseline_tiles[i].x,
                                        net_baseline_tiles[i].z, FALSE);
                dirty = TRUE;
            }
            if ((net_baseline_tiles[i].buried != 0) !=
                (mFI_UtNum2DepositGet(net_baseline_tiles[i].x, net_baseline_tiles[i].z) != 0)) {
                if (net_baseline_tiles[i].buried) mFI_UtNum2DepositON(net_baseline_tiles[i].x,
                                                                      net_baseline_tiles[i].z);
                else mFI_UtNum2DepositOFF(net_baseline_tiles[i].x, net_baseline_tiles[i].z);
                dirty = TRUE;
            }
        }
        if (dirty) mFI_SetFGUpData();
        last_baseline_serial = baseline_serial;
        /* The drain both clears the overflow flag and discards changes the
         * reprojection just superseded. */
        while (acnet_client_drain_tile_changes(net_tile_changes, ARRAY_COUNT(net_tile_changes)) != 0) {
        }
    } else if (outdoor) {
        dirty = FALSE;
        count = acnet_client_drain_tile_changes(net_tile_changes, ARRAY_COUNT(net_tile_changes));
        for (i = 0; i < count; ++i) {
            if (net_tile_changes[i].tile.zone_id != tile_zone) continue;
            if (Net_TileClaimed(net_tile_changes[i].tile.zone_id, net_tile_changes[i].tile.x,
                                net_tile_changes[i].tile.z)) continue;
            if (Net_AnimateTileChange(play, &net_tile_changes[i])) continue;
            {
                mActor_name_t* cell = mFI_UtNum2UtFG(net_tile_changes[i].tile.x, net_tile_changes[i].tile.z);
                if (!Net_TileProjectable(cell, net_tile_changes[i].tile.item)) continue;
                if (cell == NULL || *cell != (mActor_name_t)net_tile_changes[i].tile.item) {
                    mFI_UtNumtoFGSet_common(net_tile_changes[i].tile.item, net_tile_changes[i].tile.x,
                                            net_tile_changes[i].tile.z, FALSE);
                    dirty = TRUE;
                }
            }
            if ((net_tile_changes[i].tile.buried != 0) !=
                (mFI_UtNum2DepositGet(net_tile_changes[i].tile.x, net_tile_changes[i].tile.z) != 0)) {
                if (net_tile_changes[i].tile.buried) mFI_UtNum2DepositON(net_tile_changes[i].tile.x,
                                                                         net_tile_changes[i].tile.z);
                else mFI_UtNum2DepositOFF(net_tile_changes[i].tile.x, net_tile_changes[i].tile.z);
                dirty = TRUE;
            }
        }
        if (dirty) mFI_SetFGUpData();
    }
    inventory_revision = acnet_client_inventory_revision();
    if (Now_Private != NULL && inventory_revision != 0 && inventory_revision != last_inventory_revision &&
        !Net_InventoryDragActive(play)) {
        u32 conditions = 0;
        count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
        for (i = 0; i < count; ++i) {
            Now_Private->inventory.pockets[i] = slots[i].item;
            conditions = mPr_SET_ITEM_COND(conditions, i, slots[i].condition);
        }
        Now_Private->inventory.item_conditions = conditions;
        Now_Private->inventory.wallet = acnet_client_bells();
        /* The hand is part of the same authoritative inventory. Projecting the
         * pockets without it is what let a tool exist in both at once: the
         * local equip cleared a pocket slot that the next projection filled
         * back in while the tool was still held. */
        Now_Private->equipment = (mActor_name_t)acnet_client_equipped_item();
        last_inventory_revision = inventory_revision;
    }
    /* The bank is watched separately so an operator gift, which touches the
     * ledger and nothing else, still reaches the save. */
    ledger_revision = acnet_client_bank_revision();
    if (Now_Private != NULL && ledger_revision != 0 && ledger_revision != last_ledger_revision) {
        Now_Private->inventory.loan = (u32)MIN(acnet_client_debt(), 0xFFFFFFFFULL);
        Now_Private->bank_account = (u32)MIN(acnet_client_bank_balance(), 0xFFFFFFFFULL);
        last_ledger_revision = ledger_revision;
    }
    mail_revision = acnet_client_mailbox_revision();
    if (mail_revision != 0 && mail_revision != last_mail_revision) {
        Net_ApplyAuthoritativeMail();
        last_mail_revision = mail_revision;
    }
    /* The shelf changes when anyone buys and again at the day boundary, both
     * of which arrive as a town-wide delta rather than a new baseline. */
    shop_revision = acnet_client_shop_revision();
    if (shop_revision != 0 && shop_revision != last_shop_revision) {
        Net_ApplyAuthoritativeShopStock();
        last_shop_revision = shop_revision;
    }
    /* Cheap and idempotent: seven u16 compared against the projection the
     * client already holds. The schedule only moves on Sunday, but it also
     * arrives with the first baseline, and there is no separate revision to
     * watch it with. */
    Net_ApplyAuthoritativeTurnipMarket();
    Net_ApplyAuthoritativeTownTune();
    Net_ApplyAuthoritativeNotices();
    Net_ApplyAuthoritativeVillagers();
    /* After the projection, so the roll sees the authoritative roster it is
     * adding to rather than a stale one. Cheap: it returns immediately unless
     * the server has actually published an opening. */
    mNpc_NetOfferMoveIn();
    acnet_client_pump_conversations();
    Net_ApplyAuthoritativeGyroids(play);
    Net_SubmitGyroidIfEdited(play);
}

static AC_NET_REMOTE_PLAYER* Net_FindRemote(GAME_PLAY* play, u64 account_id, u64 entity_id) {
    ACTOR* actor = play->actor_info.list[ACTOR_PART_CONTROL].actor;
    while (actor != NULL) {
        if (actor->id == mAc_PROFILE_NET_REMOTE_PLAYER) {
            AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
            if (remote->account_id == account_id && remote->entity_id == entity_id) return remote;
        }
        actor = actor->next_actor;
    }
    return NULL;
}

static int Net_RemoteStillPresent(const AcNetRemotePlayer* states,
                                  size_t count,
                                  u64 account_id,
                                  u64 entity_id) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (states[i].account_id == account_id && states[i].entity_id == entity_id) return TRUE;
    }
    return FALSE;
}

/* How long a remote that has dropped out of the sampled list is kept before its
 * actor is destroyed.
 *
 * Absence is not the same as departure. ClientRuntime::remote_players omits a
 * player whose interpolation history is empty, and the history is cleared for
 * transient reasons -- half a second without a snapshot on the unreliable
 * channel, an entity mismatch, a zone edge -- as well as permanent ones. The
 * actor used to be destroyed on the first such frame and re-created a few
 * frames later, and a fresh actor restarts whatever animation is playing from
 * frame 1: a remote caught mid-cast visibly replayed the whole motion. Holding
 * the actor across the gap costs a briefly frozen pose instead. */
#define NET_REMOTE_ABSENT_FRAMES 30

static void Net_SynchronizeRemoteActors(GAME_PLAY* play, int allow_remote_players) {
    AcNetRemotePlayer states[16];
    size_t count = 0;
    size_t i;
    ACTOR* actor;
    int sampled = FALSE;

    u32 local_zone = Net_SceneZone(play->scene_id);
    if (allow_remote_players && local_zone != 0 && acnet_client_baseline_zone() == local_zone &&
        acnet_client_status() == ACNET_CONNECTED) {
        sampled = TRUE;
        count = acnet_client_remote_players(states, 16);
        for (i = 0; i < count; ++i) {
            ACTOR* created;
            AC_NET_REMOTE_PLAYER* remote;
            if (states[i].zone_id != local_zone ||
                Net_FindRemote(play, states[i].account_id, states[i].entity_id) != NULL)
                continue;
            created = Actor_info_make_actor(&play->actor_info,
                                            (GAME*)play,
                                            mAc_PROFILE_NET_REMOTE_PLAYER,
                                            states[i].transform.x,
                                            states[i].transform.y,
                                            states[i].transform.z,
                                            0,
                                            states[i].transform.yaw,
                                            0,
                                            -1,
                                            -1,
                                            -1,
                                            EMPTY_NO,
                                            0,
                                            -1,
                                            -1);
            if (created == NULL) {
                extern int g_pc_verbose;
                if (g_pc_verbose) {
                    printf("[NET] remote actor creation failed account=%llu entity=%llu zone=%u\n",
                           (unsigned long long)states[i].account_id,
                           (unsigned long long)states[i].entity_id,
                           states[i].zone_id);
                    fflush(stdout);
                }
                continue;
            }
            remote = (AC_NET_REMOTE_PLAYER*)created;
            remote->account_id = states[i].account_id;
            remote->entity_id = states[i].entity_id;
            remote->zone_id = states[i].zone_id;
            memcpy(remote->name, states[i].name, sizeof(remote->name));
            remote->gender = states[i].gender;
            remote->face = states[i].face;
            remote->clothing = states[i].clothing;
            remote->equipped_item = states[i].equipped_item;
            remote->clothing_index = states[i].clothing_index;
            remote->appearance_revision = states[i].appearance_revision;
            remote->pattern_present = states[i].pattern_present;
            remote->pattern_palette = states[i].pattern_palette;
            {
                extern int g_pc_verbose;
                if (g_pc_verbose) {
                    printf("[NET] remote actor created account=%llu entity=%llu zone=%u pos=(%.1f,%.1f,%.1f)\n",
                           (unsigned long long)remote->account_id,
                           (unsigned long long)remote->entity_id,
                           remote->zone_id,
                           states[i].transform.x,
                           states[i].transform.y,
                           states[i].transform.z);
                    fflush(stdout);
                }
            }
        }
    }

    actor = play->actor_info.list[ACTOR_PART_CONTROL].actor;
    while (actor != NULL) {
        ACTOR* next = actor->next_actor;
        if (actor->id == mAc_PROFILE_NET_REMOTE_PLAYER) {
            AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
            /* Not sampled at all, out of this scene, or in another zone: those
             * are decisions, not gaps, and take effect immediately. */
            if (!sampled || local_zone == 0 || remote->zone_id != local_zone) {
                Actor_info_delete(&play->actor_info, actor, (GAME*)play);
            } else if (Net_RemoteStillPresent(states, count, remote->account_id, remote->entity_id)) {
                remote->missing_frames = 0;
            } else if (++remote->missing_frames > NET_REMOTE_ABSENT_FRAMES) {
                Actor_info_delete(&play->actor_info, actor, (GAME*)play);
            }
        }
        actor = next;
    }
}

void Net_PreSimulation(GAME_PLAY* play) {
    int status;
    if (play == NULL) return;
    acnet_client_poll();
    /* Before both of the below: they ask which zone the current scene is, and
     * on the island that answer depends on the acre the player is standing on. */
    Net_UpdateIslandResidency(play);
    Net_UpdateZoneTransfer(play);
    Net_EnsureSceneZone(play);
    status = (int)acnet_client_status();
    if (status != last_status) {
        extern int g_pc_verbose;
        if (g_pc_verbose) {
            printf("[NET] connection status=%d%s%s\n",
                   status,
                   acnet_client_last_error()[0] != '\0' ? " error=" : "",
                   acnet_client_last_error());
        }
        if (status != ACNET_CONNECTED) {
            appearance_sent = FALSE;
            /* Results for anything still in flight will never arrive, and a
             * stuck counter would suppress reconciliation for the rest of the
             * session. */
            hold_requests_pending = 0;
        }
        last_status = status;
    }
    Net_UpdateEncounters();
    Net_UpdateHoldResults();
    Net_ApplyAuthoritativeClock();
    Net_UpdateHouseState(play);
    Net_SubmitTownIfUninitialized(play);
    Net_UpdateGameplayReadiness(play);
    Net_UpdateAppearance();
    Net_SynchronizeRemoteActors(play, gameplay_ready);
    if (gameplay_ready) {
        Net_ApplyAuthoritativeState(play);
        if (!Net_InventoryDragActive(play)) Net_ReconcileEquipment();
    }
}

void Net_PostSimulation(GAME_PLAY* play) {
    ACTOR* player_actor;
    AcNetTransform transform;
    NET_ANIMATION animation;
    pad_t* pad;
    int menu_open;
    if (play == NULL || (!gameplay_ready && zone_arrival_stream_frames == 0) ||
        acnet_client_status() == ACNET_OFFLINE || zone_transfer_phase != 0 ||
        Net_SceneZone(play->scene_id) == 0 || acnet_client_baseline_zone() != Net_SceneZone(play->scene_id)) return;
    if (!Net_CapturePlayerTransform(play, &transform)) return;
    player_actor = play->actor_info.list[ACTOR_PART_PLAYER].actor;
    /* The local keyboard/controller feeds the first game pad. Reading PAD1
     * sent zero movement forever, so prediction was continually corrected
     * back to the server spawn point. */
    pad = &play->game.pads[PAD0];
    menu_open = play->submenu.process_status != mSM_PROCESS_WAIT;
    Net_CapturePlayerAnimation((PLAYER_ACTOR*)player_actor, &animation);
    Net_CapturePlayerAppearanceBits((PLAYER_ACTOR*)player_actor);
    if (acnet_client_frame(menu_open ? 0 : (s16)pad->now.stick_x * 512,
                           menu_open ? 0 : (s16)pad->now.stick_y * 512,
                           menu_open ? 0 : pad->now.button,
                           transform.action,
                           animation.body,
                           animation.overlay,
                           animation.part_table,
                           animation.item_state,
                           animation.looping,
                           animation.reversed,
                           &transform)) {
        player_actor->world.position.x = transform.x;
        player_actor->world.position.y = transform.y;
        player_actor->world.position.z = transform.z;
        player_actor->position_speed.x = transform.velocity_x;
        player_actor->position_speed.y = transform.velocity_y;
        player_actor->position_speed.z = transform.velocity_z;
        player_actor->shape_info.rotation.y = transform.yaw;
    }
    if (zone_arrival_stream_frames > 0) zone_arrival_stream_frames--;
}

void Net_OnActorCreated(ACTOR* actor) {
    if (actor != NULL) acnet_actor_created(actor, actor->id, actor->scene_id);
}

void Net_OnActorDestroyed(ACTOR* actor) {
    if (actor != NULL) acnet_actor_destroyed(actor);
}

void Net_OnSceneLoaded(GAME_PLAY* play) {
    if (play != NULL) {
        last_baseline_serial = 0;
        last_inventory_revision = 0;
        /* Claims name tiles in a field that is about to be rebuilt, and the
         * drop actors backing them do not survive the scene change. */
        memset(net_tile_claims, 0, sizeof(net_tile_claims));
        gameplay_ready_frames = 0;
        gameplay_ready = FALSE;
        gameplay_ready_reported = FALSE;
        zone_arrival_stream_frames = 0;
        encounter_pending = 0;
        hold_requests_pending = 0;
        acnet_scene_loaded(play->scene_id);
    }
}

#endif
