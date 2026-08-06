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
#include "m_player.h"
#include "m_player_lib.h"
#include "m_scene_table.h"
#include "m_room_type.h"
#include "padmgr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int last_status = -1;
static u32 last_world_revision = 0;
static u32 last_inventory_revision = 0;
static u32 last_house_revision = 0;
static u64 last_house_id = 0;
static u64 house_candidate_hash = 0;
static u64 house_submitted_hash = 0;
static int house_candidate_frames = 0;
static int house_update_pending = FALSE;
static AcNetHouseFurniture house_furniture[3 * 4 * 16 * 16];
static u32 pending_destination_zone = 0;
static int zone_transfer_phase = 0;
static int zone_retry_frames = 0;
static int gameplay_ready_frames = 0;
static int gameplay_ready = FALSE;
static int gameplay_ready_reported = FALSE;
static int quickstart_enabled = FALSE;
static int quickstart_gender = mPr_SEX_MALE;
static u8 quickstart_name[PLAYER_NAME_LEN];

static u32 Net_SceneZone(int scene);

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

static size_t Net_CaptureHouse(int slot,
                               const AcNetHouseState* baseline,
                               MY_ROOM_ACTOR* my_room,
                               int16_t music_tracks[3],
                               uint64_t switches[12],
                               u64* hash_out) {
    mHm_hs_c* home;
    size_t count = 0;
    int floor;
    int layer;
    int z;
    int x;
    u64 hash = 1469598103934665603ULL;
    if (slot < 0 || slot >= PLAYER_NUM || baseline == NULL) return 0;
    home = &Save_Get(homes[slot]);
    memcpy(music_tracks, baseline->music_tracks, sizeof(baseline->music_tracks));
    if (my_room != NULL) {
        const int active_floor = mFI_GetPlayerHouseFloorNo(my_room->scene);
        aMR_NetFlushSwitches(my_room);
        if (active_floor >= 0 && active_floor < mHm_ROOM_NUM)
            music_tracks[active_floor] = (int16_t)aMR_NetCurrentMusic(my_room);
    }
    for (floor = 0; floor < mHm_ROOM_NUM; ++floor) {
        mHm_lyr_c* layers = &home->floors[floor].layer_main;
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
    {
        const u8 upgrade_level = home->size_info.size;
        hash = Net_HashBytes(hash, &upgrade_level, sizeof(upgrade_level));
    }
    hash = Net_HashBytes(hash, music_tracks, sizeof(baseline->music_tracks));
    {
        const int main_light = mRmTp_Index2LightSwitchStatus(slot * 2);
        const int basement_light = mRmTp_Index2LightSwitchStatus(slot * 2 + 1);
        hash = Net_HashBytes(hash, &main_light, sizeof(main_light));
        hash = Net_HashBytes(hash, &basement_light, sizeof(basement_light));
    }
    if (hash_out != NULL) *hash_out = hash;
    return count;
}

int Net_HouseOccupied(int house_index) {
    if (!Net_IsConnected() || house_index < 0 || house_index >= PLAYER_NUM) return FALSE;
    return (acnet_client_occupied_house_mask() & (1U << house_index)) != 0;
}

int Net_ApplyHouseStateBeforeRoom(GAME_PLAY* play) {
    AcNetHouseState state;
    AcNetHouseFurniture authoritative[3 * 4 * 16 * 16];
    mHm_hs_c* home;
    size_t count;
    size_t i;
    int floor;
    int layer;
    int changed = FALSE;
    if (play == NULL || !Net_IsConnected() || !mSc_IS_SCENE_PLAYER_ROOM(play->scene_id) ||
        !acnet_client_house(&state) || state.zone_id != Net_SceneZone(play->scene_id) || !state.initialized ||
        state.original_slot >= PLAYER_NUM) return FALSE;
    if (last_house_id != state.house_id) {
        last_house_id = state.house_id;
        last_house_revision = 0;
        house_candidate_hash = 0;
        house_submitted_hash = 0;
        house_candidate_frames = 0;
        house_update_pending = FALSE;
    }
    if (last_house_revision == state.revision) return FALSE;
    home = &Save_Get(homes[state.original_slot]);
    if (home->size_info.size != state.upgrade_level) {
        home->size_info.size = state.upgrade_level;
        changed = TRUE;
    }
    for (floor = 0; floor < mHm_ROOM_NUM; ++floor) {
        mHm_lyr_c* layers = &home->floors[floor].layer_main;
        for (layer = 0; layer < mCoBG_LAYER_NUM; ++layer) {
            if (layers[layer].ftr_switch != state.furniture_switches[floor * mCoBG_LAYER_NUM + layer]) changed = TRUE;
            layers[layer].ftr_switch = state.furniture_switches[floor * mCoBG_LAYER_NUM + layer];
            for (i = 0; i < UT_X_NUM * UT_Z_NUM; ++i) {
                if (((mActor_name_t*)layers[layer].items)[i] != EMPTY_NO) changed = TRUE;
                ((mActor_name_t*)layers[layer].items)[i] = EMPTY_NO;
            }
        }
    }
    count = acnet_client_house_furniture(authoritative, ARRAY_COUNT(authoritative));
    for (i = 0; i < count; ++i) {
        mHm_lyr_c* layers;
        mActor_name_t* item;
        mActor_name_t presentation_item;
        if (authoritative[i].floor >= mHm_ROOM_NUM || authoritative[i].layer >= mCoBG_LAYER_NUM ||
            authoritative[i].x >= UT_X_NUM || authoritative[i].z >= UT_Z_NUM) continue;
        layers = &home->floors[authoritative[i].floor].layer_main;
        item = &layers[authoritative[i].layer].items[authoritative[i].z][authoritative[i].x];
        presentation_item = authoritative[i].item;
        if ((presentation_item & 0xF000) != 0xF000) {
            presentation_item = mRmTp_Item1ItemNo2FtrItemNo_AtPlayerRoom(presentation_item, TRUE);
            if (ITEM_IS_FTR(presentation_item))
                presentation_item = (presentation_item & ~3) | (authoritative[i].condition & 3);
        }
        if (*item != presentation_item) changed = TRUE;
        *item = presentation_item;
    }
    if (state.main_light_on) mRmTp_IndexLightSwitchON(state.original_slot * 2);
    else mRmTp_IndexLightSwitchOFF(state.original_slot * 2);
    if (state.basement_light_on) mRmTp_IndexLightSwitchON(state.original_slot * 2 + 1);
    else mRmTp_IndexLightSwitchOFF(state.original_slot * 2 + 1);
    last_house_revision = state.revision;
    return changed;
}

static void Net_UpdateHouseState(GAME_PLAY* play) {
    AcNetHouseState state;
    AcNetHouseUpdateResult result;
    MY_ROOM_ACTOR* my_room;
    int16_t music_tracks[3];
    uint64_t switches[12];
    u64 hash;
    size_t count;
    int floor;
    int changed;
    while (acnet_client_take_house_update_result(&result)) {
        house_update_pending = FALSE;
        if (result.result_code == 0) house_submitted_hash = house_candidate_hash;
        else {
            house_candidate_frames = 0;
            /* A rejected optimistic local edit must be overwritten even when
             * the authoritative room revision did not advance. */
            last_house_revision = 0;
        }
    }
    if (!mSc_IS_SCENE_PLAYER_ROOM(play->scene_id) || !acnet_client_house(&state) ||
        state.zone_id != Net_SceneZone(play->scene_id)) return;
    my_room = Net_FindMyRoom(play);
    changed = Net_ApplyHouseStateBeforeRoom(play);
    if (changed && my_room != NULL) aMR_NetReloadFurniture((ACTOR*)my_room, (GAME*)play);
    floor = mFI_GetPlayerHouseFloorNo(play->scene_id);
    if (state.initialized && my_room != NULL && floor >= 0 && floor < 3)
        aMR_NetSetMusic(my_room, state.music_tracks[floor]);
    if (state.owner_account_id != acnet_client_account() || state.original_slot >= PLAYER_NUM ||
        house_update_pending) return;
    count = Net_CaptureHouse(state.original_slot, &state, my_room, music_tracks, switches, &hash);
    if (hash != house_candidate_hash) {
        house_candidate_hash = hash;
        house_candidate_frames = 0;
        return;
    }
    if (house_candidate_frames < 30) {
        ++house_candidate_frames;
        return;
    }
    if (state.initialized && hash == house_submitted_hash) return;
    if (acnet_client_submit_house_update(state.house_id, state.revision,
                                         Save_Get(homes[state.original_slot]).size_info.size,
                                         (u8)mRmTp_Index2LightSwitchStatus(state.original_slot * 2),
                                         (u8)mRmTp_Index2LightSwitchStatus(state.original_slot * 2 + 1),
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

static u32 Net_SceneZone(int scene) {
    if (scene == SCENE_FG) return 1;
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
        if (destination >= 100 && destination <= 103) return destination;
    } else if (destination == 1) {
        if (source == 2) return 11;
        if (source == 3) return 21;
        if (source == 4) return 31;
        if (source == 5) return 41;
        if (source == 6) return 51;
        if (source >= 100 && source <= 103) return source + 100;
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

static void Net_UpdateZoneTransfer(void) {
    AcNetTransferOffer offer;
    if (zone_transfer_phase != 0 && acnet_client_baseline_zone() == pending_destination_zone) {
        pending_destination_zone = 0;
        zone_transfer_phase = 0;
        (void)acnet_client_take_transfer_offer(&offer);
        return;
    }
    if (zone_transfer_phase == 0) return;
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
        pending_destination_zone = 0;
        zone_transfer_phase = 0;
        zone_retry_frames = 30;
        return;
    }
    if (zone_transfer_phase == 1) {
        if (!acnet_client_zone_ready(offer.token_high, offer.token_low)) {
            pending_destination_zone = 0;
            zone_transfer_phase = 0;
            zone_retry_frames = 30;
        } else {
            zone_transfer_phase = 2;
        }
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

int Net_SubmitInitialTown(void) {
    static AcNetTownBootstrapTile tiles[5 * 6 * 16 * 16];
    AcNetPlayerAppearance appearance;
    size_t index = 0;
    int block_z;
    int block_x;
    int unit_z;
    int unit_x;
    if (!Net_IsConnected() || Now_Private == NULL) return FALSE;
    Net_ApplyTownIdentity();
    memset(&appearance, 0, sizeof(appearance));
    memcpy(appearance.name, Now_Private->player_ID.player_name, sizeof(appearance.name));
    appearance.gender = Now_Private->gender;
    appearance.face = Now_Private->face;
    appearance.clothing = Now_Private->cloth.item;
    appearance.equipped_item = Now_Private->equipment;
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
    return acnet_client_submit_town_bootstrap(Save_Get(land_info).name,
                                               Save_Get(land_info).id,
                                               &appearance,
                                               tiles,
                                               index);
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

int Net_RequestPickup(const xyz_t* position, mActor_name_t item) {
    AcNetTileState tile;
    int ut_x;
    int ut_z;
    if (position == NULL || !Net_IsConnected() ||
        !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(1, (s16)ut_x, (s16)ut_z, &tile) || tile.item != item) return FALSE;
    last_inventory_revision = 0;
    return acnet_client_request_world_auto(1, 1, (s16)ut_x, (s16)ut_z, tile.revision,
                                            acnet_client_inventory_revision(), 0, item);
}

int Net_RequestDrop(int ut_x, int ut_z, mActor_name_t item) {
    AcNetTileState tile;
    AcNetItemSlot slots[15];
    size_t count;
    size_t slot;
    if (!Net_IsConnected() || !acnet_client_tile(1, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    for (slot = 0; slot < count; ++slot) {
        if (slots[slot].item == item) break;
    }
    if (slot == count) return FALSE;
    last_inventory_revision = 0;
    return acnet_client_request_world_auto(0, 1, (s16)ut_x, (s16)ut_z, tile.revision,
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
    size_t count;
    int tool_slot = 0xFF;
    int ut_x;
    int ut_z;
    if (position == NULL || !Net_IsConnected() || acnet_client_baseline_zone() != 1 ||
        !mFI_Wpos2UtNum(&ut_x, &ut_z, *position) ||
        !acnet_client_tile(1, (s16)ut_x, (s16)ut_z, &tile)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    if (inventory_slot >= 0 && ((size_t)inventory_slot >= count || slots[inventory_slot].item != item)) return FALSE;
    if (tool_kind != 0) {
        tool_slot = Net_FindToolSlot(slots, count, tool_kind);
        if (tool_slot < 0) return FALSE;
    }
    last_inventory_revision = 0;
    return acnet_client_request_world_with_tool_auto(operation_type, 1, (s16)ut_x, (s16)ut_z,
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

int Net_RequestEncounter(int kind) {
    AcNetItemSlot slots[15];
    size_t count;
    int tool_slot;
    if (!Net_IsConnected() || (kind != 0 && kind != 1)) return FALSE;
    count = acnet_client_inventory(slots, ARRAY_COUNT(slots));
    tool_slot = Net_FindToolSlot(slots, count, kind == 0 ? 3 : 4);
    if (tool_slot < 0) return FALSE;
    last_inventory_revision = 0;
    return acnet_client_request_encounter_auto((u8)kind, acnet_client_inventory_revision(), (u8)tool_slot);
}

static void Net_ApplyAuthoritativeState(GAME_PLAY* play) {
    AcNetTileState tiles[256];
    AcNetItemSlot slots[15];
    u32 world_revision;
    u32 inventory_revision;
    size_t count;
    size_t i;

    if (acnet_client_status() != ACNET_CONNECTED) return;
    world_revision = acnet_client_baseline_revision();
    if (play->scene_id == SCENE_FG && acnet_client_baseline_zone() == 1 &&
        world_revision != 0 && world_revision != last_world_revision) {
        count = acnet_client_baseline_tiles(tiles, ARRAY_COUNT(tiles));
        for (i = 0; i < count; ++i) {
            if (tiles[i].zone_id != 1) continue;
            mFI_UtNumtoFGSet_common(tiles[i].item, tiles[i].x, tiles[i].z, FALSE);
            if (tiles[i].buried) mFI_UtNum2DepositON(tiles[i].x, tiles[i].z);
            else mFI_UtNum2DepositOFF(tiles[i].x, tiles[i].z);
        }
        if (count != 0) mFI_SetFGUpData();
        last_world_revision = world_revision;
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
        Now_Private->inventory.loan = (u32)MIN(acnet_client_debt(), 0xFFFFFFFFULL);
        Now_Private->bank_account = (u32)MIN(acnet_client_bank_balance(), 0xFFFFFFFFULL);
        last_inventory_revision = inventory_revision;
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
    Net_UpdateZoneTransfer();
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
        last_status = status;
    }
    Net_ApplyAuthoritativeClock();
    Net_UpdateHouseState(play);
    Net_UpdateGameplayReadiness(play);
    Net_SynchronizeRemoteActors(play, gameplay_ready);
    if (gameplay_ready) Net_ApplyAuthoritativeState(play);
}

void Net_PostSimulation(GAME_PLAY* play) {
    ACTOR* player_actor;
    AcNetTransform transform;
    pad_t* pad;
    int menu_open;
    if (play == NULL || !gameplay_ready || acnet_client_status() == ACNET_OFFLINE || zone_transfer_phase != 0 ||
        Net_SceneZone(play->scene_id) == 0 || acnet_client_baseline_zone() != Net_SceneZone(play->scene_id)) return;
    player_actor = play->actor_info.list[ACTOR_PART_PLAYER].actor;
    if (player_actor == NULL || player_actor->id != mAc_PROFILE_PLAYER) return;
    /* The local keyboard/controller feeds the first game pad. Reading PAD1
     * sent zero movement forever, so prediction was continually corrected
     * back to the server spawn point. */
    pad = &play->game.pads[PAD0];
    menu_open = play->submenu.process_status != mSM_PROCESS_WAIT;
    transform.x = player_actor->world.position.x;
    transform.y = player_actor->world.position.y;
    transform.z = player_actor->world.position.z;
    transform.velocity_x = player_actor->position_speed.x;
    transform.velocity_y = player_actor->position_speed.y;
    transform.velocity_z = player_actor->position_speed.z;
    transform.yaw = player_actor->shape_info.rotation.y;
    transform.action = 0;
    if (acnet_client_frame(menu_open ? 0 : (s16)pad->now.stick_x * 512,
                           menu_open ? 0 : (s16)pad->now.stick_y * 512,
                           menu_open ? 0 : pad->now.button,
                           0,
                           &transform)) {
        player_actor->world.position.x = transform.x;
        player_actor->world.position.y = transform.y;
        player_actor->world.position.z = transform.z;
        player_actor->position_speed.x = transform.velocity_x;
        player_actor->position_speed.y = transform.velocity_y;
        player_actor->position_speed.z = transform.velocity_z;
        player_actor->shape_info.rotation.y = transform.yaw;
    }
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
        acnet_scene_loaded(play->scene_id);
    }
}

#endif
