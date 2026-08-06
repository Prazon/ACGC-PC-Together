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
#include "m_collision_bg.h"
#include "m_home_h.h"
#include "m_mail.h"
#include "m_private.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_scene_table.h"
#include "m_submenu.h"
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
static u32 last_world_revision = 0;
/* The island baseline arrives under its own zone, so its tiles need a
 * watermark of their own or a town revision would suppress an island apply. */
static u32 last_island_revision = 0;
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
    appearance->equipped_item = Now_Private->equipment;
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

static size_t Net_CaptureHouse(const NetRoomBinding* room,
                               const AcNetHouseState* baseline,
                               MY_ROOM_ACTOR* my_room,
                               int16_t music_tracks[3],
                               uint64_t switches[12],
                               u64* hash_out) {
    size_t count = 0;
    int floor;
    int layer;
    int z;
    int x;
    u64 hash = 1469598103934665603ULL;
    if (room == NULL || !room->valid || baseline == NULL) return 0;
    memcpy(music_tracks, baseline->music_tracks, sizeof(baseline->music_tracks));
    if (my_room != NULL) {
        const int active_floor = mFI_GetPlayerHouseFloorNo(my_room->scene);
        aMR_NetFlushSwitches(my_room);
        if (active_floor >= 0 && active_floor < room->floor_count)
            music_tracks[active_floor] = (int16_t)aMR_NetCurrentMusic(my_room);
    }
    for (floor = 0; floor < room->floor_count; ++floor) {
        mHm_lyr_c* layers = &room->floors[floor].layer_main;
        for (layer = 0; layer < mCoBG_LAYER_NUM; ++layer) {
            switches[floor * mCoBG_LAYER_NUM + layer] = layers[layer].ftr_switch;
            hash = Net_HashBytes(hash, &layers[layer].ftr_switch, sizeof(layers[layer].ftr_switch));
            for (z = 0; z < UT_Z_NUM; ++z) {
                for (x = 0; x < UT_X_NUM; ++x) {
                    const mActor_name_t item = layers[layer].items[z][x];
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
        count = Net_CaptureHouse(&room, &state, my_room, music_tracks, switches, &hash);
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
        Net_CaptureHouse(&room, &state, my_room, music_tracks, switches, &house_submitted_hash);
        return;
    }
    if (house_update_pending) return;
    count = Net_CaptureHouse(&room, &state, my_room, music_tracks, switches, &hash);
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
                                         music_tracks, switches, house_furniture, count)) {
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
    transform->action = (u16)player->now_main_index;
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
                }
            }
        }
    }
    island_count = Net_CaptureIslandBootstrap(island_tiles, ARRAY_COUNT(island_tiles), island_block_x);
    return acnet_client_submit_town_bootstrap(Save_Get(land_info).name,
                                               Save_Get(land_info).id,
                                               &appearance,
                                               tiles,
                                               index,
                                               island_count != 0 ? island_tiles : NULL,
                                               island_count,
                                               (u8)island_block_x[0],
                                               (u8)island_block_x[1]);
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

int Net_RequestPickup(const xyz_t* position, mActor_name_t item) {
    AcNetTileState tile;
    const u32 zone = Net_TileZone();
    int ut_x;
    int ut_z;
    if (position == NULL || zone == 0 ||
        !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile) || tile.item != item) return FALSE;
    last_inventory_revision = 0;
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
    last_inventory_revision = 0;
    return acnet_client_request_world_auto(0, zone, (s16)ut_x, (s16)ut_z, tile.revision,
                                            acnet_client_inventory_revision(), (u8)slot, item);
}

static int Net_FindToolSlot(const AcNetItemSlot* slots, size_t count, int tool_kind) {
    size_t i;
    for (i = 0; i < count; ++i) {
        mActor_name_t item = slots[i].item;
        if ((tool_kind == 1 && (ITEM_IS_SCOOP(item) || ITEM_IS_GOLD_SCOOP(item))) ||
            (tool_kind == 2 && (IS_ITEM_AXE(item) || item == ITM_GOLDEN_AXE)) ||
            (tool_kind == 3 && (item == ITM_ROD || item == ITM_GOLDEN_ROD)) ||
            (tool_kind == 4 && (item == ITM_NET || item == ITM_GOLDEN_NET))) return (int)i;
    }
    return -1;
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
    int tool_slot = 0xFF;
    int ut_x;
    int ut_z;
    if (position == NULL || zone == 0 ||
        !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(zone, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    if (inventory_slot >= 0 && ((size_t)inventory_slot >= count || slots[inventory_slot].item != item)) return FALSE;
    if (tool_kind != 0) {
        tool_slot = Net_FindToolSlot(slots, count, tool_kind);
        if (tool_slot < 0) return FALSE;
    }
    last_inventory_revision = 0;
    return acnet_client_request_world_with_tool_auto(operation_type, zone, (s16)ut_x, (s16)ut_z,
                                                      tile.revision, acnet_client_inventory_revision(),
                                                      inventory_slot < 0 ? 0 : (u8)inventory_slot,
                                                      (u8)tool_slot, item);
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
    AcNetItemSlot slots[15];
    size_t count;
    int tool_slot;
    if (!Net_IsConnected() || (kind != 0 && kind != 1)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    tool_slot = Net_FindToolSlot(slots, count, kind == 0 ? 3 : 4);
    if (tool_slot < 0) return FALSE;
    last_inventory_revision = 0;
    if (!acnet_client_request_encounter_auto((u8)kind, (u16)species, acnet_client_inventory_revision(),
                                             (u8)tool_slot))
        return FALSE;
    encounter_pending++;
    return TRUE;
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

/* Mirrors acnet::EconomyOpType. The wire refuses anything above ClaimMail. */
#define NET_ECONOMY_BUY 0
#define NET_ECONOMY_SELL 1
#define NET_ECONOMY_DEPOSIT 2
#define NET_ECONOMY_WITHDRAW 3
#define NET_ECONOMY_PAY_DEBT 4
#define NET_ECONOMY_DONATE 5
#define NET_ECONOMY_ATTACH_MAIL 6

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
 * two acres and arrives whole, so this has to hold the larger of the two. */
static AcNetTileState net_baseline_tiles[2 * UT_X_NUM * UT_Z_NUM];

static void Net_ApplyAuthoritativeState(GAME_PLAY* play) {
    AcNetItemSlot slots[15];
    u32 world_revision;
    u32 inventory_revision;
    u32 ledger_revision;
    u32 mail_revision;
    u32 tile_zone;
    size_t count;
    size_t i;

    if (acnet_client_status() != ACNET_CONNECTED) return;
    world_revision = acnet_client_baseline_revision();
    tile_zone = acnet_client_baseline_zone();
    /* Both outdoor zones apply the same way: the island acres are part of the
     * same unit grid, and mFI_UtNumtoFGSet_common already routes a write at an
     * island acre into Save_t.island.fgblock, because mFM_SetFgUtPtoSaveData
     * pointed that block's item array there when the field was built. The two
     * zones keep separate revision watermarks so crossing between them cannot
     * suppress the arriving baseline. */
    if (play->scene_id == SCENE_FG && (tile_zone == 1 || tile_zone == NET_ZONE_ISLAND) &&
        world_revision != 0 &&
        world_revision != (tile_zone == 1 ? last_world_revision : last_island_revision)) {
        count = acnet_client_baseline_tiles(net_baseline_tiles, ARRAY_COUNT(net_baseline_tiles));
        for (i = 0; i < count; ++i) {
            if (net_baseline_tiles[i].zone_id != tile_zone) continue;
            mFI_UtNumtoFGSet_common(net_baseline_tiles[i].item, net_baseline_tiles[i].x,
                                    net_baseline_tiles[i].z, FALSE);
            if (net_baseline_tiles[i].buried) mFI_UtNum2DepositON(net_baseline_tiles[i].x, net_baseline_tiles[i].z);
            else mFI_UtNum2DepositOFF(net_baseline_tiles[i].x, net_baseline_tiles[i].z);
        }
        if (count != 0) mFI_SetFGUpData();
        if (tile_zone == 1) last_world_revision = world_revision;
        else last_island_revision = world_revision;
    }
    inventory_revision = acnet_client_inventory_revision();
    if (Now_Private != NULL && inventory_revision != 0 && inventory_revision != last_inventory_revision) {
        u32 conditions = 0;
        count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
        for (i = 0; i < count; ++i) {
            Now_Private->inventory.pockets[i] = slots[i].item;
            conditions = mPr_SET_ITEM_COND(conditions, i, slots[i].condition);
        }
        Now_Private->inventory.item_conditions = conditions;
        Now_Private->inventory.wallet = acnet_client_bells();
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

static void Net_SynchronizeRemoteActors(GAME_PLAY* play, int allow_remote_players) {
    AcNetRemotePlayer states[16];
    size_t count = 0;
    size_t i;
    ACTOR* actor;

    u32 local_zone = Net_SceneZone(play->scene_id);
    if (allow_remote_players && local_zone != 0 && acnet_client_baseline_zone() == local_zone &&
        acnet_client_status() == ACNET_CONNECTED) {
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
            if (local_zone == 0 || remote->zone_id != local_zone ||
                !Net_RemoteStillPresent(states, count, remote->account_id, remote->entity_id)) {
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
        if (status != ACNET_CONNECTED) appearance_sent = FALSE;
        last_status = status;
    }
    Net_UpdateEncounters();
    Net_ApplyAuthoritativeClock();
    Net_UpdateHouseState(play);
    Net_UpdateGameplayReadiness(play);
    Net_UpdateAppearance();
    Net_SynchronizeRemoteActors(play, gameplay_ready);
    if (gameplay_ready) Net_ApplyAuthoritativeState(play);
}

void Net_PostSimulation(GAME_PLAY* play) {
    ACTOR* player_actor;
    AcNetTransform transform;
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
    if (acnet_client_frame(menu_open ? 0 : (s16)pad->now.stick_x * 512,
                           menu_open ? 0 : (s16)pad->now.stick_y * 512,
                           menu_open ? 0 : pad->now.button,
                           transform.action,
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
        last_world_revision = 0;
        last_inventory_revision = 0;
        gameplay_ready_frames = 0;
        gameplay_ready = FALSE;
        gameplay_ready_reported = FALSE;
        zone_arrival_stream_frames = 0;
        encounter_pending = 0;
        acnet_scene_loaded(play->scene_id);
    }
}

#endif
