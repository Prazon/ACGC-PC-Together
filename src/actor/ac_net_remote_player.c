#include "ac_net_remote_player.h"

#ifdef NETCODE_ENABLED

#include "acnet/c_api.h"
#include "ac_my_house.h"
#include "ac_shop_goods_h.h"
#include "bg_item_h.h"
#include "c_keyframe.h"
#include "m_common_data.h"
#include "m_field_info.h"
#include "m_malloc.h"
#include "m_name_table.h"
#include "m_needlework.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_private.h"
#include "m_rcp.h"
#include "sys_matrix.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void Net_Remote_Player_ct(ACTOR* actor, GAME* game);
static void Net_Remote_Player_dt(ACTOR* actor, GAME* game);
static void Net_Remote_Player_move(ACTOR* actor, GAME* game);
static void Net_Remote_Player_draw(ACTOR* actor, GAME* game);

/* A remote resident is presentation-only: it has no controller, camera,
 * inventory callbacks, collision, or save ownership. It does, however, use
 * the original player skeleton and resident-specific face/clothing resources
 * so another client appears as their real character instead of a proxy mesh. */
typedef struct net_remote_render_data_s {
    /* Two keyframes combined through a part table, exactly as the original
     * player does: keyframe0 drives the body, keyframe1 the upper body, and
     * mPlayer_PART_TABLE_* selects which joints come from which. Replicating
     * only keyframe0 would leave a remote player swinging an axe with idle
     * arms. */
    cKF_SkeletonInfo_R_c keyframe0;
    cKF_SkeletonInfo_R_c keyframe1;
    s_xyz joint_data0[mPlayer_JOINT_NUM + 1];
    s_xyz morph_data0[mPlayer_JOINT_NUM + 1];
    s_xyz joint_data1[mPlayer_JOINT_NUM + 1];
    s_xyz morph_data1[mPlayer_JOINT_NUM + 1];
    s8 part_table[mPlayer_JOINT_NUM + 1];
    Mtx work_mtx[2][13] ATTRIBUTE_ALIGN(32);
    u8 clothing_texture[mNW_DESIGN_TEX_SIZE] ATTRIBUTE_ALIGN(32);
    u16 clothing_palette[mNW_PALETTE_SIZE / sizeof(u16)] ATTRIBUTE_ALIGN(32);
    u8 face_texture[0xE00] ATTRIBUTE_ALIGN(32);
    u16 face_palette[mNW_PALETTE_SIZE / sizeof(u16)] ATTRIBUTE_ALIGN(32);
    u8 loaded_gender;
    u8 loaded_face;
    u16 loaded_clothing;
    u32 loaded_appearance_revision;
    u8 loaded_pattern_present;
    u8 loaded_pattern_palette;
    /* The replicated animation currently playing, so a repeat of the same
     * state does not restart it every frame. */
    u16 loaded_body;
    u16 loaded_overlay;
    u8 loaded_part_table;
    u8 loaded_looping;
    u8 loaded_reversed;
    u8 animation_loaded;
    /* Captured from the hand joint during the skeleton draw and used to place
     * the held item afterwards. */
    xyz_t hand_pos;
    u8 hand_pos_valid;
    u8 initialized;
} NET_REMOTE_RENDER_DATA;

static int Net_Remote_Player_refresh_appearance(AC_NET_REMOTE_PLAYER* remote,
                                                const AcNetRemotePlayer* state) {
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    mPr_cloth_c cloth;
    int gender;
    int face;

    if (render == NULL) return FALSE;
    gender = remote->gender == mPr_SEX_FEMALE ? mPr_SEX_FEMALE : mPr_SEX_MALE;
    face = remote->face < mPr_FACE_TYPE_NUM ? remote->face : 0;
    if (render->initialized && render->loaded_gender == gender &&
        render->loaded_face == face && render->loaded_clothing == remote->clothing &&
        render->loaded_appearance_revision == remote->appearance_revision &&
        render->loaded_pattern_present == state->pattern_present &&
        render->loaded_pattern_palette == state->pattern_palette) return TRUE;

    if (state->pattern_present && state->clothing_index >= (CLOTH_NUM + 1) &&
        state->clothing_index < (CLOTH_NUM + 1 + mPr_ORIGINAL_DESIGN_COUNT) &&
        state->pattern_palette < mNW_PALETTE_NUM) {
        memcpy(render->clothing_texture, state->pattern_texture, sizeof(render->clothing_texture));
        memcpy(render->clothing_palette, mNW_PaletteIdx2Palette(state->pattern_palette),
               sizeof(render->clothing_palette));
        DCStoreRangeNoSync(render->clothing_texture, sizeof(render->clothing_texture));
        DCStoreRangeNoSync(render->clothing_palette, sizeof(render->clothing_palette));
        {
            extern int g_pc_verbose;
            if (g_pc_verbose) {
                printf("[NET] remote pattern loaded account=%llu revision=%u index=%u\n",
                       (unsigned long long)remote->account_id,
                       remote->appearance_revision,
                       remote->clothing_index);
                fflush(stdout);
            }
        }
    } else {
        memset(&cloth, 0, sizeof(cloth));
        mPlib_change_player_cloth_info(&cloth, remote->clothing);
        cloth.idx = state->clothing_index < (CLOTH_NUM + 1) ? state->clothing_index : cloth.idx;
        mPlib_Load_PlayerTexAndPallet(render->clothing_texture, render->clothing_palette, cloth.idx);
    }
    if (!mPlib_Load_PlayerFaceTexAndPallet(render->face_texture, render->face_palette, gender, face)) return FALSE;

    if (render->initialized) {
        cKF_SkeletonInfo_R_dt(&render->keyframe0);
        cKF_SkeletonInfo_R_dt(&render->keyframe1);
    }
    cKF_SkeletonInfo_R_ct(&render->keyframe0, mPlib_get_player_mdl_for_gender(gender), NULL,
                          render->joint_data0, render->morph_data0);
    cKF_SkeletonInfo_R_ct(&render->keyframe1, mPlib_get_player_mdl_for_gender(gender), NULL,
                          render->joint_data1, render->morph_data1);
    cKF_SkeletonInfo_R_init_standard_repeat(&render->keyframe0, mPlib_Get_Pointer_Animation(mPlayer_ANIM_WAIT1), NULL);
    cKF_SkeletonInfo_R_init_standard_repeat(&render->keyframe1, mPlib_Get_Pointer_Animation(mPlayer_ANIM_WAIT1), NULL);
    mPlib_DMA_player_Part_Table(render->part_table, mPlayer_PART_TABLE_NORMAL);
    render->loaded_gender = (u8)gender;
    render->loaded_face = (u8)face;
    render->loaded_clothing = remote->clothing;
    render->loaded_appearance_revision = remote->appearance_revision;
    render->loaded_pattern_present = state->pattern_present;
    render->loaded_pattern_palette = state->pattern_palette;
    /* The skeletons were rebuilt, so whatever was playing is gone. */
    render->animation_loaded = FALSE;
    render->loaded_part_table = mPlayer_PART_TABLE_NORMAL;
    render->initialized = TRUE;
    return TRUE;
}

/* Drive the two keyframes from the replicated indices. The originating client
 * runs the state machine that chose them; every value has already been range
 * checked by the protocol decoder, so they can index the animation table
 * directly. */
static void Net_Remote_Player_apply_animation(NET_REMOTE_RENDER_DATA* render, const AcNetRemotePlayer* state) {
    if (render->animation_loaded && render->loaded_body == state->animation_body &&
        render->loaded_overlay == state->animation_overlay &&
        render->loaded_looping == state->animation_looping &&
        render->loaded_reversed == state->animation_reversed) {
        if (render->loaded_part_table != state->animation_part_table) {
            mPlib_DMA_player_Part_Table(render->part_table, state->animation_part_table);
            render->loaded_part_table = state->animation_part_table;
        }
        return;
    }

    if (render->loaded_part_table != state->animation_part_table) {
        mPlib_DMA_player_Part_Table(render->part_table, state->animation_part_table);
        render->loaded_part_table = state->animation_part_table;
    }
    if (state->animation_reversed) {
        cKF_SkeletonInfo_R_init_reverse_setspeedandmorphandmode(
            &render->keyframe0, mPlib_Get_Pointer_Animation(state->animation_body), NULL, 1.0f, 0.5f,
            state->animation_looping ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP);
        cKF_SkeletonInfo_R_init_reverse_setspeedandmorphandmode(
            &render->keyframe1, mPlib_Get_Pointer_Animation(state->animation_overlay), NULL, 1.0f, 0.5f,
            state->animation_looping ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP);
    } else {
        cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(
            &render->keyframe0, mPlib_Get_Pointer_Animation(state->animation_body), NULL, 1.0f, 1.0f, 0.5f,
            state->animation_looping ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP);
        cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(
            &render->keyframe1, mPlib_Get_Pointer_Animation(state->animation_overlay), NULL, 1.0f, 1.0f, 0.5f,
            state->animation_looping ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP);
    }
    render->loaded_body = state->animation_body;
    render->loaded_overlay = state->animation_overlay;
    render->loaded_looping = state->animation_looping;
    render->loaded_reversed = state->animation_reversed;
    render->animation_loaded = TRUE;
}

ACTOR_PROFILE Net_Remote_Player_Profile = {
    mAc_PROFILE_NET_REMOTE_PLAYER,
    ACTOR_PART_CONTROL,
    ACTOR_STATE_NO_CULL,
    EMPTY_NO,
    ACTOR_OBJ_BANK_KEEP,
    sizeof(AC_NET_REMOTE_PLAYER),
    Net_Remote_Player_ct,
    Net_Remote_Player_dt,
    Net_Remote_Player_move,
    Net_Remote_Player_draw,
    NULL,
};

static void Net_Remote_Player_ct(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    NET_REMOTE_RENDER_DATA* render;
    (void)game;
    /* Server movement is intentionally horizontal-only. Project the remote
     * presentation actor onto this client's real foreground height instead
     * of drawing the server's placeholder Y=0 below rolling terrain. */
    actor->world.position.y = mCoBG_GetBgY_OnlyCenter_FromWpos2(actor->world.position, 0.0f);
    actor->last_world_position.y = actor->world.position.y;
    actor->ground_y = actor->world.position.y;
    actor->shape_info.ofs_y = 200.0f;
    actor->cull_width = 80.0f;
    actor->cull_height = 100.0f;
    actor->cull_distance = 1200.0f;
    actor->cull_radius = 80.0f;
    render = (NET_REMOTE_RENDER_DATA*)zelda_malloc_align(sizeof(*render), 32);
    if (render != NULL) {
        memset(render, 0, sizeof(*render));
        render->loaded_gender = 0xFF;
        render->loaded_face = 0xFF;
        render->loaded_clothing = 0xFFFF;
        render->loaded_appearance_revision = 0;
        render->loaded_pattern_present = 0xFF;
        render->loaded_pattern_palette = 0xFF;
        render->loaded_part_table = 0xFF;
    }
    remote->render_data = render;
}

static void Net_Remote_Player_dt(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    (void)game;
    if (remote->render_data != NULL) {
        NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
        if (render->initialized) {
            cKF_SkeletonInfo_R_dt(&render->keyframe0);
            cKF_SkeletonInfo_R_dt(&render->keyframe1);
        }
        zelda_free(render);
        remote->render_data = NULL;
    }
}

static void Net_Remote_Player_move(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    AcNetRemotePlayer states[16];
    size_t count;
    size_t i;

    count = acnet_client_remote_players(states, 16);
    for (i = 0; i < count; ++i) {
        if (states[i].account_id == remote->account_id && states[i].entity_id == remote->entity_id) {
            const f32 target_x = states[i].transform.x;
            const f32 target_z = states[i].transform.z;
            const f32 delta_x = target_x - actor->world.position.x;
            const f32 delta_z = target_z - actor->world.position.z;
            const f32 distance_squared = delta_x * delta_x + delta_z * delta_z;
            const f32 dt = game->graph != NULL ? (f32)game->graph->dt_num_60fps_frames : 1.0f;
            const f32 blend = 1.0f - powf(0.60f, MAX(0.0f, dt));
            const s16 yaw_delta = (s16)(states[i].transform.yaw - actor->shape_info.rotation.y);
            s16 visual_yaw;
            if (states[i].transition_phase != remote->transition_phase ||
                states[i].transition_door != remote->transition_door) {
                if (states[i].transition_phase != 0) {
                    if (states[i].transition_door >= 100 && states[i].transition_door <= 103 &&
                        states[i].transition_phase == 1)
                        aMHS_NetDoorAnimation((int)(states[i].transition_door - 100), FALSE);
                    else if (states[i].transition_door >= 200 && states[i].transition_door <= 203 &&
                             states[i].transition_phase == 2)
                        aMHS_NetDoorAnimation((int)(states[i].transition_door - 200), TRUE);
                }
                remote->transition_phase = states[i].transition_phase;
                remote->transition_door = states[i].transition_door;
                remote->transition_expires_tick = states[i].transition_expires_tick;
            }
            xyz_t_move(&actor->last_world_position, &actor->world.position);
            /* The portable client buffers six simulation ticks and advances
             * through snapshot history at render time. A light final ease
             * absorbs packet-arrival clock jitter. Large discontinuities are
             * scene transitions/teleports and must not glide across a room. */
            if (states[i].zone_id != remote->zone_id || distance_squared > 320.0f * 320.0f) {
                actor->world.position.x = target_x;
                actor->world.position.z = target_z;
                visual_yaw = states[i].transform.yaw;
            } else {
                actor->world.position.x += delta_x * blend;
                actor->world.position.z += delta_z * blend;
                visual_yaw = ABS(yaw_delta) < 64
                                 ? states[i].transform.yaw
                                 : (s16)(actor->shape_info.rotation.y + (s16)(yaw_delta * blend));
            }
            actor->world.position.y = mCoBG_GetBgY_OnlyCenter_FromWpos2(actor->world.position, 0.0f);
            actor->ground_y = actor->world.position.y;
            actor->position_speed.x = states[i].transform.velocity_x;
            actor->position_speed.y = 0.0f;
            actor->position_speed.z = states[i].transform.velocity_z;
            actor->world.angle.y = visual_yaw;
            actor->shape_info.rotation.y = visual_yaw;
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
            remote->missing_frames = 0;
            if (Net_Remote_Player_refresh_appearance(remote, &states[i])) {
                NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
                Net_Remote_Player_apply_animation(render, &states[i]);
                cKF_SkeletonInfo_R_combine_play(&render->keyframe0, &render->keyframe1, render->part_table);
            }
            return;
        }
    }
    if (++remote->missing_frames > 180) Actor_delete(actor);
}

/* The hand joint's matrix is only available while the skeleton is being walked,
 * so the position is captured here and the item drawn after the skeleton --
 * the same order Player_actor_draw_After_hand and Player_actor_Item_draw use
 * for the local player. */
static int Net_Remote_Player_draw_after(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp,
                                        u8* work_flag, void* arg, s_xyz* rot, xyz_t* pos) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)arg;
    NET_REMOTE_RENDER_DATA* render;
    (void)game;
    (void)kf;
    (void)gfx_pp;
    (void)work_flag;
    (void)rot;
    (void)pos;
    if (remote == NULL || joint_no != mPlayer_JOINT_HAND) return TRUE;
    render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    if (render == NULL) return TRUE;
    Matrix_Position_Zero(&render->hand_pos);
    render->hand_pos_valid = TRUE;
    return TRUE;
}

/* A presentation actor has no tool sub-actors and no item state machine, so the
 * held item is drawn as a plain model in the hand rather than by reproducing
 * Player_actor_Item_draw_*. That is enough to see who is carrying an axe and
 * who is carrying a net; the swing itself comes from the animation. */
static void Net_Remote_Player_draw_held_item(AC_NET_REMOTE_PLAYER* remote, GAME* game) {
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    const mActor_name_t item = (mActor_name_t)remote->equipped_item;

    if (render == NULL || !render->hand_pos_valid || item == EMPTY_NO) return;
    if (mFI_GET_TYPE(mFI_GetFieldId()) == mFI_FIELD_FG) {
        if (Common_Get(clip).bg_item_clip != NULL && Common_Get(clip).bg_item_clip->single_draw_proc != NULL) {
            Common_Get(clip).bg_item_clip->single_draw_proc(game, item, &render->hand_pos, 1.0f, NULL, NULL, NULL);
        }
    } else if (Common_Get(clip).shop_goods_clip != NULL &&
               Common_Get(clip).shop_goods_clip->single_draw_proc != NULL) {
        Common_Get(clip).shop_goods_clip->single_draw_proc(game, item, &render->hand_pos, 1.0f, 0, FALSE);
    }
}

static void Net_Remote_Player_draw(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    GRAPH* graph = game->graph;

    if (render == NULL || !render->initialized) return;
    _texture_z_light_fog_prim(graph);
    OPEN_POLY_OPA_DISP(graph);
    gSPSegment(POLY_OPA_DISP++, ANIME_1_TXT_SEG, render->face_texture);
    gSPSegment(POLY_OPA_DISP++, ANIME_2_TXT_SEG,
               render->face_texture + mPlayer_EYE_TEX_NUM * 0x100);
    gSPSegment(POLY_OPA_DISP++, ANIME_3_TXT_SEG, render->clothing_texture);
    gSPSegment(POLY_OPA_DISP++, ANIME_4_TXT_SEG, render->clothing_palette);
    gSPSegment(POLY_OPA_DISP++, ANIME_5_TXT_SEG, render->face_palette);
    CLOSE_POLY_OPA_DISP(graph);
    render->hand_pos_valid = FALSE;
    cKF_Si3_draw_R_SV(game, &render->keyframe0, render->work_mtx[game->frame_counter & 1], NULL,
                      &Net_Remote_Player_draw_after, remote);
    Net_Remote_Player_draw_held_item(remote, game);
}

#endif
