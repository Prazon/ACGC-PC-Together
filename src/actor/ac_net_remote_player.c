#include "ac_net_remote_player.h"

#ifdef NETCODE_ENABLED

#include "acnet/c_api.h"
#include "ac_my_house.h"
#include "c_keyframe.h"
#include "m_malloc.h"
#include "m_name_table.h"
#include "m_needlework.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_private.h"
#include "m_rcp.h"
#include "sys_matrix.h"

#include <math.h>
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
    cKF_SkeletonInfo_R_c keyframe;
    s_xyz joint_data[mPlayer_JOINT_NUM + 1];
    s_xyz morph_data[mPlayer_JOINT_NUM + 1];
    Mtx work_mtx[2][13] ATTRIBUTE_ALIGN(32);
    u8 clothing_texture[mNW_DESIGN_TEX_SIZE] ATTRIBUTE_ALIGN(32);
    u16 clothing_palette[mNW_PALETTE_SIZE / sizeof(u16)] ATTRIBUTE_ALIGN(32);
    u8 face_texture[0xE00] ATTRIBUTE_ALIGN(32);
    u16 face_palette[mNW_PALETTE_SIZE / sizeof(u16)] ATTRIBUTE_ALIGN(32);
    u8 loaded_gender;
    u8 loaded_face;
    u16 loaded_clothing;
    s8 animation_mode;
    u8 initialized;
} NET_REMOTE_RENDER_DATA;

enum {
    NET_REMOTE_ANIM_WAIT,
    NET_REMOTE_ANIM_WALK
};

static int Net_Remote_Player_refresh_appearance(AC_NET_REMOTE_PLAYER* remote) {
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    mPr_cloth_c cloth;
    int gender;
    int face;

    if (render == NULL) return FALSE;
    gender = remote->gender == mPr_SEX_FEMALE ? mPr_SEX_FEMALE : mPr_SEX_MALE;
    face = remote->face < mPr_FACE_TYPE_NUM ? remote->face : 0;
    if (render->initialized && render->loaded_gender == gender &&
        render->loaded_face == face && render->loaded_clothing == remote->clothing) return TRUE;

    memset(&cloth, 0, sizeof(cloth));
    mPlib_change_player_cloth_info(&cloth, remote->clothing);
    mPlib_Load_PlayerTexAndPallet(render->clothing_texture, render->clothing_palette, cloth.idx);
    if (!mPlib_Load_PlayerFaceTexAndPallet(render->face_texture, render->face_palette, gender, face)) return FALSE;

    cKF_SkeletonInfo_R_dt(&render->keyframe);
    cKF_SkeletonInfo_R_ct(&render->keyframe, mPlib_get_player_mdl_for_gender(gender), NULL,
                          render->joint_data, render->morph_data);
    cKF_SkeletonInfo_R_init_standard_repeat(&render->keyframe, mPlib_Get_Pointer_Animation(0), NULL);
    render->loaded_gender = (u8)gender;
    render->loaded_face = (u8)face;
    render->loaded_clothing = remote->clothing;
    render->animation_mode = NET_REMOTE_ANIM_WAIT;
    render->initialized = TRUE;
    return TRUE;
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
        render->animation_mode = -1;
    }
    remote->render_data = render;
}

static void Net_Remote_Player_dt(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    (void)game;
    if (remote->render_data != NULL) {
        NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
        if (render->initialized) cKF_SkeletonInfo_R_dt(&render->keyframe);
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
            /* The portable client already buffers six simulation ticks and
             * interpolates snapshot history. Its sample advances at snapshot
             * cadence, though, so ease the presentation actor between those
             * samples at the actual render rate. Large discontinuities are
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
            remote->missing_frames = 0;
            if (Net_Remote_Player_refresh_appearance(remote)) {
                NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
                const f32 speed_squared = actor->position_speed.x * actor->position_speed.x +
                                          actor->position_speed.z * actor->position_speed.z;
                const int animation = speed_squared > 1.0f ? NET_REMOTE_ANIM_WALK : NET_REMOTE_ANIM_WAIT;
                if (render->animation_mode != animation) {
                    cKF_SkeletonInfo_R_init_standard_repeat(
                        &render->keyframe, mPlib_Get_Pointer_Animation(animation == NET_REMOTE_ANIM_WALK ? 1 : 0), NULL);
                    render->animation_mode = (s8)animation;
                }
                cKF_SkeletonInfo_R_play(&render->keyframe);
            }
            return;
        }
    }
    if (++remote->missing_frames > 180) Actor_delete(actor);
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
    cKF_Si3_draw_R_SV(game, &render->keyframe, render->work_mtx[game->frame_counter & 1], NULL, NULL, remote);
}

#endif
