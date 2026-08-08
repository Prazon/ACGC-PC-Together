#include "ac_net_remote_player.h"

#ifdef NETCODE_ENABLED

#include "acnet/c_api.h"
#include "ac_my_house.h"
#include "ac_shop_goods_h.h"
#include "ac_tools.h"
#include "bg_item_h.h"
#include "c_keyframe.h"
#include "m_actor_shadow.h"
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

/* The item skeletons (net, rod, balloon, pinwheel) all fit in eight joints,
 * exactly like PLAYER_ACTOR::item_joint_data. */
#define NET_REMOTE_ITEM_JOINT_NUM 8

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
    /* The held tool's own skeleton, driven exactly like
     * PLAYER_ACTOR::item_keyframe. Only the non-GFX item shapes use it -- an
     * axe or a shovel is a bare display list. */
    cKF_SkeletonInfo_R_c item_keyframe;
    s_xyz item_joint_data[NET_REMOTE_ITEM_JOINT_NUM];
    s_xyz item_morph_data[NET_REMOTE_ITEM_JOINT_NUM];
    Mtx item_work_mtx[2][4] ATTRIBUTE_ALIGN(32);
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
    /* Where keyframe0/1 stood when a skeleton rebuild threw them away. A
     * rebuild is an appearance change, not an animation change, so the motion
     * has to resume rather than start over -- restarting is what made a remote
     * caught mid-cast play the whole cast a second time. */
    f32 resume_frame0;
    f32 resume_frame1;
    u8 resume_valid;
    /* Last main index reported by the --verbose animation trace, so it prints
     * once per state change rather than once per frame. */
    u16 logged_action;
    /* Which tool is built into item_keyframe, so a repeat of the same state
     * does not rebuild it every frame either. */
    s16 loaded_item_shape;
    s16 loaded_item_anim;
    s8 loaded_item_kind;
    u8 item_skeleton_loaded;
    u8 item_visible;
    /* The item states whose frame the original writes per frame instead of
     * playing. A viewer holds the entry pose and must not advance them. */
    u8 item_frame_held;
    f32 item_scale;
    /* Balloon presentation state. The original keeps the equivalent on
     * PLAYER_ACTOR; none of it is replicated because all of it is derivable
     * from motion the viewer already has. */
    s_xyz balloon_angle;
    s16 balloon_lean_angle;
    s16 balloon_add_rot_x;
    s16 balloon_add_rot_x_counter;
    f32 balloon_add_rot_z;
    f32 balloon_anim_max_frame;
    f32 balloon_anim_speed;
    f32 balloon_current_frame;
    u8 balloon_stop_movement_flag;
    /* Captured from the hand joint during the skeleton draw and used to place
     * the held item afterwards. The matrix carries orientation as well, which
     * a bare position cannot -- without it every tool points the same way
     * regardless of the pose. */
    MtxF right_hand_mtx;
    xyz_t hand_pos;
    xyz_t hand_move;
    u8 hand_pos_valid;
    /* Blink phase plus the resolved eye/mouth tile. See mPlib_Face_Step. */
    mPlib_face_state_c face;
    /* PLAYER_ACTOR::shadow_pos. The shadow keeps a pointer to it rather than a
     * copy, so it has to outlive the frame that computes it. */
    xyz_t shadow_pos;
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
        if (render->animation_loaded) {
            render->resume_frame0 = render->keyframe0.frame_control.current_frame;
            render->resume_frame1 = render->keyframe1.frame_control.current_frame;
            render->resume_valid = TRUE;
        }
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
    const int mode = state->animation_looping ? cKF_FRAMECONTROL_REPEAT : cKF_FRAMECONTROL_STOP;
    f32 frame0 = 1.0f;
    f32 frame1 = 1.0f;
    int resume;

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

    /* Only a rebuild of the very animation that was interrupted can be
     * resumed; the captured frames belong to loaded_body/loaded_overlay, so an
     * appearance change that lands on the same frame as a state change starts
     * the new animation from the top as usual. */
    resume = render->resume_valid && render->loaded_body == state->animation_body &&
             render->loaded_overlay == state->animation_overlay &&
             render->loaded_looping == state->animation_looping &&
             render->loaded_reversed == state->animation_reversed;
    if (resume) {
        frame0 = render->resume_frame0;
        frame1 = render->resume_frame1;
    }
    render->resume_valid = FALSE;

    if (render->loaded_part_table != state->animation_part_table) {
        mPlib_DMA_player_Part_Table(render->part_table, state->animation_part_table);
        render->loaded_part_table = state->animation_part_table;
    }
    if (state->animation_reversed) {
        cKF_SkeletonInfo_R_init_reverse_setspeedandmorphandmode(
            &render->keyframe0, mPlib_Get_Pointer_Animation(state->animation_body), NULL, 1.0f, 0.5f, mode);
        cKF_SkeletonInfo_R_init_reverse_setspeedandmorphandmode(
            &render->keyframe1, mPlib_Get_Pointer_Animation(state->animation_overlay), NULL, 1.0f, 0.5f, mode);
        /* The reverse init has no start-frame parameter -- it always begins at
         * the last frame -- so a resumed reverse animation is placed after it. */
        if (resume) {
            render->keyframe0.frame_control.current_frame = frame0;
            render->keyframe1.frame_control.current_frame = frame1;
        }
    } else {
        cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(
            &render->keyframe0, mPlib_Get_Pointer_Animation(state->animation_body), NULL, frame0, 1.0f, 0.5f, mode);
        cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(
            &render->keyframe1, mPlib_Get_Pointer_Animation(state->animation_overlay), NULL, frame1, 1.0f, 0.5f, mode);
    }
    render->loaded_body = state->animation_body;
    render->loaded_overlay = state->animation_overlay;
    render->loaded_looping = state->animation_looping;
    render->loaded_reversed = state->animation_reversed;
    render->animation_loaded = TRUE;
}

/* Locomotion animations play at a speed derived from how fast the player is
 * actually moving; everything else plays at the game's baseline of 0.5.
 *
 * The baseline is 0.5, not 1.0. Every Player_actor_InitAnimation_Base1/2/3
 * call site was surveyed: 131 of the 133 real ones pass 0.5f -- wait, talk,
 * pickup, dig, swing, sit, shock, all of it. The only 1.0f states are the
 * pitfall climb-out and the umbrella twirl, and the only other literal,
 * YATTA1/YATTA2 at 0.6f, is the VER_GAFU01_00 branch of a version guard --
 * this build is VER_GAFE01_00, whose branch passes 0.5f. Returning 1.0 here
 * ran every non-locomotion remote animation, idle included, at exactly double
 * rate. The item keyframe learned this same lesson first; see
 * Net_Remote_Player_update_item.
 *
 * The locomotion family: Player_actor_CulcAnimation_Walk assigns keyframe0/1
 * frame_control.speed every frame from actor->speed; run, dash and the demo
 * walk all funnel into it (Player_actor_CulcAnimation_Run and _Dash are
 * wrappers), so one formula covers the four. Ready_walk_net has its own
 * constants.
 *
 * The speed is derived rather than replicated. Actor_position_speed_set makes
 * position_speed the actor's scalar speed resolved through its facing, so
 * |velocity_xz| is exactly actor->speed, and MovementSimulator::tick stores the
 * client transform verbatim -- the replicated velocity is already in the game's
 * own units. Replicating the animation speed instead would mean a value that
 * changes every frame while walking, which cannot ride the change-triggered
 * reliable presentation delta.
 *
 * Deliberately approximated at the 0.5 baseline: radio exercise and the car
 * wash, whose frames the event/minigame drives externally after a 0.0 init,
 * and the snowball push, whose speed the snowball actor writes per frame.
 * Dropped from the walk formula: over_speed_normalize_NoneZero, a terrain
 * slope correction that is 1.0 on flat ground, and the wall-collision
 * branches, which are local collision state a viewer does not have. */
static f32 Net_Remote_Player_animation_speed(const AC_NET_REMOTE_PLAYER* remote,
                                             const NET_REMOTE_RENDER_DATA* render) {
    const f32 speed = ((const ACTOR*)remote)->speed;
    f32 sp;

    switch (remote->action) {
        case mPlayer_INDEX_WALK:
        case mPlayer_INDEX_RUN:
        case mPlayer_INDEX_DASH:
        case mPlayer_INDEX_DEMO_WALK:
            sp = 0.59999996f * sqrtf(speed / 7.5f);
            break;
        case mPlayer_INDEX_READY_WALK_NET:
            sp = 0.252f * sqrtf(speed / 1.8f);
            break;
        case mPlayer_INDEX_CLIMBUP_PITFALL:
        case mPlayer_INDEX_ROTATE_UMBRELLA:
            return 1.0f;
        case mPlayer_INDEX_CHANGE_CLOTH:
            /* The dressing-room try-on (MENU_CHANGE1) is the state's 1.0
             * branch; the Halloween prank (ITAZURA1) takes the baseline. The
             * try_on flag is not replicated, but the animation choice is. */
            return render->loaded_body == mPlayer_ANIM_MENU_CHANGE1 ? 1.0f : 0.5f;
        default:
            return 0.5f;
    }

    return sp < 0.22f ? 0.22f : sp;
}

static void Net_Remote_Player_update_animation_speed(AC_NET_REMOTE_PLAYER* remote, NET_REMOTE_RENDER_DATA* render) {
    const f32 sp = Net_Remote_Player_animation_speed(remote, render);

    render->keyframe0.frame_control.speed = sp;
    render->keyframe1.frame_control.speed = sp;

    /* --verbose prints one line per state change, which is what to compare
     * against the local player: a full-stick walk should report speed 4.875 and
     * anim 0.484, a full-stick dash 7.5 and 0.600, and anything stationary
     * 0.500. A remote reporting anim 0.500 while visibly striding means the
     * action never arrived and the switch fell through to its default. */
    {
        extern int g_pc_verbose;
        if (g_pc_verbose && render->logged_action != remote->action) {
            render->logged_action = remote->action;
            printf("[NET] remote anim account=%llu action=%u speed=%.3f anim=%.3f body=%u\n",
                   (unsigned long long)remote->account_id, (unsigned)remote->action,
                   (double)((ACTOR*)remote)->speed, (double)sp, (unsigned)render->loaded_body);
            fflush(stdout);
        }
    }
}

/* Player_actor_SetupShadow: the shadow is a per-main-index property, so a
 * viewer reproduces it from the replicated action instead of replicating a bit.
 *
 * The original indexes a 121-entry s8 table by now_main_index. Reproducing it
 * as a positional array here would be a second copy that silently rots if the
 * enum ever gains a state, so only the entries that are *not*
 * mPlayer_SHADOW_TYPE_NORMAL are named. Every one of them is a state where the
 * player is inside or underneath something -- in bed, down a pitfall, hidden as
 * a groundhog, sitting in the boat -- which is what confirms the reading of the
 * table rather than the ordinal count alone.
 *
 * Without this the remote actor never called Shape_Info_init at all, so
 * shadow_proc stayed NULL and remote players cast no shadow anywhere. */
static void Net_Remote_Player_update_shadow(AC_NET_REMOTE_PLAYER* remote, NET_REMOTE_RENDER_DATA* render) {
    ACTOR* actor = (ACTOR*)remote;

    if (!mPlayer_MAIN_INDEX_VALID((int)remote->action)) return;

    switch (remote->action) {
        /* mPlayer_SHADOW_TYPE_NONE */
        case mPlayer_INDEX_DMA:
        case mPlayer_INDEX_WAIT_BED:
        case mPlayer_INDEX_ROLL_BED:
        case mPlayer_INDEX_STANDUP_BED:
        case mPlayer_INDEX_SITDOWN_WAIT:
        case mPlayer_INDEX_STANDUP:
        case mPlayer_INDEX_DEMO_GETON_TRAIN_WAIT:
        case mPlayer_INDEX_HIDE:
        case mPlayer_INDEX_WASH_CAR:
        case mPlayer_INDEX_FALL_PITFALL:
        case mPlayer_INDEX_STRUGGLE_PITFALL:
        case mPlayer_INDEX_DEMO_GETON_BOAT_SITDOWN:
        case mPlayer_INDEX_DEMO_GETON_BOAT_WAIT:
        case mPlayer_INDEX_DEMO_GETON_BOAT_WADE:
        case mPlayer_INDEX_DEMO_GETOFF_BOAT_STANDUP:
            actor->shape_info.draw_shadow = FALSE;
            break;
        /* mPlayer_SHADOW_TYPE_WORLD_POS */
        case mPlayer_INDEX_DOOR:
            actor->shape_info.draw_shadow = TRUE;
            mActorShadow_SetForceShadowPos(actor, &actor->world.position);
            break;
        /* mPlayer_SHADOW_TYPE_ANIME_POS: the shadow stays where the animation's
         * own translation puts it, so it does not slide out from under a player
         * walking out of a door. */
        case mPlayer_INDEX_OUTDOOR:
            actor->shape_info.draw_shadow = TRUE;
            cKF_SkeletonInfo_R_AnimationMove_CulcTransToWorld(&render->shadow_pos, &actor->world.position, 0.0f,
                                                              1000.0f, 0.0f, actor->shape_info.rotation.y,
                                                              &actor->scale, &render->keyframe0,
                                                              cKF_ANIMATION_TRANS_XZ | cKF_ANIMATION_TRANS_Y);
            mActorShadow_SetForceShadowPos(actor, &render->shadow_pos);
            break;
        default:
            actor->shape_info.draw_shadow = TRUE;
            mActorShadow_UnSetForceShadowPos(actor);
            break;
    }
}

/* Player_actor_set_lean_angle / _recover_lean_angle: the forward pitch a
 * running player leans into. It is derived from the body animation's playback
 * speed, not from the ground normal, so a viewer that already reproduces the
 * animation speed gets the lean for free -- and because the sixth power falls
 * away so fast, it is visible on a dash (the full 20 degrees) and barely
 * present at a walk, exactly as the original.
 *
 * The four states that lean are the same four Net_Remote_Player_animation_speed
 * treats as locomotion; every other state calls the recover form, which eases
 * the pitch back to zero. Both use add_calc_short_angle2, so the remote's pitch
 * is smoothed over frames rather than snapped. */
static void Net_Remote_Player_update_lean(AC_NET_REMOTE_PLAYER* remote, NET_REMOTE_RENDER_DATA* render) {
    ACTOR* actor = (ACTOR*)remote;
    s16 target = DEG2SHORT_ANGLE2(0.0f);

    switch (remote->action) {
        case mPlayer_INDEX_WALK:
        case mPlayer_INDEX_RUN:
        case mPlayer_INDEX_DASH:
        case mPlayer_INDEX_DEMO_WALK: {
            const f32 sp = render->keyframe0.frame_control.speed;
            f32 lean = SQ(sp) / 0.36f;

            lean = SQ(lean);
            lean = lean * lean * lean * DEG2SHORT_ANGLE2(20.0f);
            if (lean > DEG2SHORT_ANGLE2(20.0f)) lean = DEG2SHORT_ANGLE2(20.0f);
            target = (s16)lean;
            break;
        }
        default:
            break;
    }

    add_calc_short_angle2(&actor->shape_info.rotation.x, target, 1.0f - sqrtf(0.5f), DEG2SHORT_ANGLE2(10.0f),
                          DEG2SHORT_ANGLE2(0.0f));
}

/* Which item animation belongs to a replicated mPlayer_ITEM_MAIN_* state.
 *
 * Resolved numerically from the thirteen Player_actor_SetupItem_Base2 call
 * sites. Those pass mPlayer_ANIM_* and mPlayer_INDEX_* constants where
 * mPlayer_ITEM_DATA_* and mPlayer_ITEM_MAIN_* are meant -- the decomp flags it
 * itself at m_player_common.c_inc:3977 ("fairly random indexes") -- but the
 * ordinals are correct, and every one of the thirteen resolves to the
 * semantically matching pair, which is what confirms the reading:
 *
 *   swing_net   ANIM_PICKUP1(7)     -> ITEM_DATA_NET_SWING      INDEX_REFUSE_PICKUP(3)  -> ITEM_MAIN_NET_SWING
 *   stop_net    ANIM_GET_CHANGE1(11)-> ITEM_DATA_SWING_WAIT     INDEX_RETURN_DEMO(4)    -> ITEM_MAIN_NET_STOP
 *   pull_net    ANIM_HOLD_WAIT1(6)  -> ITEM_DATA_NET_GET_M      INDEX_WAIT(7)           -> ITEM_MAIN_NET_PULL
 *   putaway_net ANIM_LTURN1(8)      -> ITEM_DATA_KAMAE_MAIN_M   INDEX_WALK(8)           -> ITEM_MAIN_NET_PUTAWAY
 *   notice_net  ANIM_GET_PUTAWAY1(12)->ITEM_DATA_YATTA_M        INDEX_RUN(9)            -> ITEM_MAIN_NET_COMPLETE_COLLECTION
 *   ready_rod   ANIM_UMBRELLA1(18)  -> ITEM_DATA_ROD_SWING      INDEX_TUMBLE_GETUP(12)  -> ITEM_MAIN_ROD_READY
 *   cast_rod    ANIM_UMBRELLA1(18)  -> ITEM_DATA_ROD_SWING      INDEX_TURN_DASH(13)     -> ITEM_MAIN_ROD_CAST
 *   air_rod     ANIM_RUN_SLIP1(20)  -> ITEM_DATA_ROD_NOT_SWING  INDEX_FALL(14)          -> ITEM_MAIN_ROD_AIR
 *   relax_rod   ANIM_UMB_OPEN1(17)  -> ITEM_DATA_ROD_SINARI     INDEX_WADE(15)          -> ITEM_MAIN_ROD_RELAX
 *   collect_rod ANIM_TRANS_WAIT1(15)-> ITEM_DATA_ROD_GET_T      INDEX_DOOR(16)          -> ITEM_MAIN_ROD_COLLECT
 *   vib_rod     ANIM_UMB_OPEN1(17)  -> ITEM_DATA_ROD_SINARI     INDEX_OUTDOOR(17)       -> ITEM_MAIN_ROD_VIB
 *   fly_rod     ANIM_TRANS_WAIT1(15)-> ITEM_DATA_ROD_GET_T      INDEX_INVADE(18)        -> ITEM_MAIN_ROD_FLY
 *   putaway_rod ANIM_TRANS_WAIT1(15)-> ITEM_DATA_ROD_GET_T      INDEX_HOLD(19)          -> ITEM_MAIN_ROD_PUTAWAY
 *
 * Every other item state uses the item kind's basic animation, which is what
 * Player_actor_SetupItem_Base0/1 does. *mode_p and *frame_p take the playback
 * mode and the start frame from the same call site. A start frame other than
 * 1.0 marks a state the original never plays -- see the rod cases. */
static int Net_Remote_Player_item_anim(int item_state, int kind, int* mode_p, f32* frame_p) {
    *mode_p = cKF_FRAMECONTROL_STOP;
    *frame_p = 1.0f;
    switch (item_state) {
        case mPlayer_ITEM_MAIN_NET_SWING:
            *mode_p = cKF_FRAMECONTROL_REPEAT;
            return mPlayer_ITEM_DATA_NET_SWING;
        case mPlayer_ITEM_MAIN_NET_STOP:
            return mPlayer_ITEM_DATA_SWING_WAIT;
        case mPlayer_ITEM_MAIN_NET_PULL:
            return mPlayer_ITEM_DATA_NET_GET_M;
        case mPlayer_ITEM_MAIN_NET_PUTAWAY:
            return mPlayer_ITEM_DATA_KAMAE_MAIN_M;
        case mPlayer_ITEM_MAIN_NET_COMPLETE_COLLECTION:
            return mPlayer_ITEM_DATA_YATTA_M;
        case mPlayer_ITEM_MAIN_ROD_READY:
        case mPlayer_ITEM_MAIN_ROD_CAST:
            return mPlayer_ITEM_DATA_ROD_SWING;
        case mPlayer_ITEM_MAIN_ROD_AIR:
            return mPlayer_ITEM_DATA_ROD_NOT_SWING;
        case mPlayer_ITEM_MAIN_ROD_RELAX:
        case mPlayer_ITEM_MAIN_ROD_VIB:
            /* The only two states whose item keyframe the original never
             * advances. Player_actor_Item_main_rod_relax/_vib call
             * Player_actor_Item_SetFrame_forUki_relax/_vib, which write
             * current_frame outright each frame from the float's geometry,
             * starting from the 180.0f the state is entered with -- there is no
             * Item_CulcAnimation_Base in either. The float is not replicated,
             * so a viewer holds that entry pose. Playing the animation here
             * instead ran the rod's whole bend cycle on a loop, over and over,
             * for as long as the peer was waiting for a bite. */
            *mode_p = cKF_FRAMECONTROL_REPEAT;
            *frame_p = 180.0f;
            return mPlayer_ITEM_DATA_ROD_SINARI;
        case mPlayer_ITEM_MAIN_ROD_COLLECT:
        case mPlayer_ITEM_MAIN_ROD_FLY:
        case mPlayer_ITEM_MAIN_ROD_PUTAWAY:
            return mPlayer_ITEM_DATA_ROD_GET_T;
        default:
            *mode_p = cKF_FRAMECONTROL_REPEAT;
            return mPlib_Get_BasicItemAnimeIndex_fromItemKind(kind);
    }
}

/* Player_actor_Check_ItemAnimationToItemKind, for a viewer.
 *
 * item_state is authored by the acting client while equipped_item comes from
 * the server's inventory, so the two can legitimately disagree for a frame
 * across a tool swap -- and a peer could make them disagree deliberately.
 * Feeding a rod animation into a net skeleton is the failure that guards
 * against. */
static int Net_Remote_Player_item_anim_valid(int kind, int anim) {
    u8 type;

    if (!mPlayer_ITEM_DATA_VALID(anim)) return FALSE;
    type = mPlib_Get_Item_DataPointerType(anim);
    if (mPlayer_ITEM_IS_NET(kind)) return type == mPlayer_ITEM_DATA_TYPE_NET_ANIMATION;
    if (mPlayer_ITEM_IS_ROD(kind)) return type == mPlayer_ITEM_DATA_TYPE_ROD_ANIMATION;
    if (mPlayer_ITEM_IS_BALLOON(kind)) return type == mPlayer_ITEM_DATA_TYPE_BALLOON_ANIMATION;
    if (mPlayer_ITEM_IS_WINDMILL(kind)) return type == mPlayer_ITEM_DATA_TYPE_PINWHEEL_ANIMATION;
    return FALSE;
}

static void Net_Remote_Player_reset_balloon(NET_REMOTE_RENDER_DATA* render) {
    /* Player_actor_Item_Setup_main_balloon_normal guards this reset with
     * mPlayer_ITEM_IS_BALLOON(last_item_main_index), comparing an
     * mPlayer_ITEM_MAIN_* (0..23) against the balloon *item kind* range
     * (56..63). It is therefore always false and the reset always runs on
     * entry to the balloon item state -- reproduced here as the unconditional
     * reset it actually is. */
    render->balloon_lean_angle = 0;
    render->balloon_angle = ZeroSVec;
    render->balloon_anim_max_frame = render->item_keyframe.frame_control.max_frames;
    render->balloon_anim_speed = 0.0f;
    render->balloon_stop_movement_flag = TRUE;
    render->balloon_add_rot_z = 30.0f;
    render->balloon_add_rot_x = 0;
    render->balloon_add_rot_x_counter = 0;
    render->balloon_current_frame = 0.0f;
    render->item_keyframe.frame_control.current_frame = render->balloon_anim_max_frame;
    render->item_keyframe.frame_control.speed = render->balloon_anim_speed;
}

/* Build or rebuild the held tool. Mirrors Player_actor_LoadOrDestruct_Item and
 * Player_actor_Item_DMA_Data, minus the double-bank bookkeeping: the item
 * models, skeletons and animations are global symbols reached through
 * mPlib_Get_Item_DataPointer, so a viewer never re-DMAs anything and has
 * nothing to double-buffer. */
static void Net_Remote_Player_refresh_item(AC_NET_REMOTE_PLAYER* remote, GAME* game) {
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    int kind;
    int shape;
    int anim;
    int mode;
    int was_balloon;
    f32 start_frame;

    if (render == NULL) return;

    kind = mPlib_Get_ItemNoToItemKind((mActor_name_t)remote->equipped_item);
    /* mPlayer_ITEM_MAIN_NONE is the original's own "nothing in the hand"
     * state: it covers a stowed tool, a menu, an interior that forbids tools,
     * and every non-tool animation. Without this the tool used to render
     * whenever the inventory hand was merely non-empty. */
    if (remote->item_state == mPlayer_ITEM_MAIN_NONE || !mPlayer_ITEM_KIND_VALID(kind)) {
        render->item_visible = FALSE;
        render->loaded_item_kind = -1;
        render->item_skeleton_loaded = FALSE;
        if (remote->umbrella_actor != NULL && Common_Get(clip).tools_clip != NULL) {
            Common_Get(clip).tools_clip->aTOL_chg_request_mode_proc((ACTOR*)remote, remote->umbrella_actor,
                                                                    aTOL_ACTION_DESTRUCT);
            remote->umbrella_actor = NULL;
        }
        return;
    }

    /* An umbrella is the one tool the original draws through a real actor, and
     * it is handled before the shape lookup because it deliberately has no shape
     * of its own: mPlib_Get_BasicItemShapeIndex_fromItemKind returns -1 for
     * every umbrella kind, and Player_actor_Item_draw_umbrella draws nothing --
     * it only hands the pole matrix to the TOOLS_ACTOR. Birth it as a child of
     * this remote, exactly as Player_actor_birth_umbrella does for the local
     * player, and destruct it as soon as the tool changes. */
    if (mPlayer_ITEM_IS_UMBRELLA(kind)) {
        if (remote->umbrella_actor != NULL && ((TOOLS_ACTOR*)remote->umbrella_actor)->tool_name !=
                                                  (kind - mPlayer_ITEM_KIND_UMBRELLA00)) {
            if (Common_Get(clip).tools_clip != NULL) {
                Common_Get(clip).tools_clip->aTOL_chg_request_mode_proc((ACTOR*)remote, remote->umbrella_actor,
                                                                        aTOL_ACTION_DESTRUCT);
            }
            remote->umbrella_actor = NULL;
        }
        if (remote->umbrella_actor == NULL && Common_Get(clip).tools_clip != NULL) {
            remote->umbrella_actor = Common_Get(clip).tools_clip->aTOL_birth_proc(
                kind - mPlayer_ITEM_KIND_UMBRELLA00, aTOL_ACTION_S_TAKEOUT, (ACTOR*)remote, game, -1, NULL);
            if (remote->umbrella_actor != NULL) {
                remote->umbrella_actor->world.position = remote->actor_class.world.position;
            }
        }
        render->item_visible = remote->umbrella_actor != NULL;
        render->item_skeleton_loaded = FALSE;
        render->loaded_item_kind = (s8)kind;
        render->loaded_item_shape = -1;
        render->loaded_item_anim = -1;
        return;
    }

    if (remote->umbrella_actor != NULL && Common_Get(clip).tools_clip != NULL) {
        Common_Get(clip).tools_clip->aTOL_chg_request_mode_proc((ACTOR*)remote, remote->umbrella_actor,
                                                                aTOL_ACTION_DESTRUCT);
        remote->umbrella_actor = NULL;
    }

    shape = mPlib_Get_BasicItemShapeIndex_fromItemKind(kind);
    if (!mPlayer_ITEM_DATA_VALID(shape)) {
        render->item_visible = FALSE;
        render->item_skeleton_loaded = FALSE;
        return;
    }
    render->item_visible = TRUE;

    /* A GFX shape is a bare display list: axe, shovel, fan.
     * Player_actor_Item_DMA_Data skips the skeleton for exactly this case. */
    if (mPlib_Get_Item_DataPointerType(shape) == mPlayer_ITEM_DATA_TYPE_GFX) {
        render->item_skeleton_loaded = FALSE;
        render->loaded_item_kind = (s8)kind;
        render->loaded_item_shape = (s16)shape;
        render->loaded_item_anim = -1;
        return;
    }

    anim = Net_Remote_Player_item_anim(remote->item_state, kind, &mode, &start_frame);
    if (!Net_Remote_Player_item_anim_valid(kind, anim)) {
        anim = mPlib_Get_BasicItemAnimeIndex_fromItemKind(kind);
        mode = cKF_FRAMECONTROL_REPEAT;
        start_frame = 1.0f;
        if (!Net_Remote_Player_item_anim_valid(kind, anim)) {
            render->item_skeleton_loaded = FALSE;
            render->item_visible = FALSE;
            return;
        }
    }
    render->item_frame_held = start_frame != 1.0f;

    if (render->item_skeleton_loaded && render->loaded_item_shape == shape && render->loaded_item_anim == anim) {
        return;
    }

    was_balloon = render->item_skeleton_loaded && mPlayer_ITEM_IS_BALLOON(render->loaded_item_kind);
    cKF_SkeletonInfo_R_ct(&render->item_keyframe, (cKF_Skeleton_R_c*)mPlib_Get_Item_DataPointer(shape), NULL,
                          render->item_joint_data, render->item_morph_data);
    /* 0.5 is the item animation speed every Player_actor_LoadOrDestruct_Item
     * call site passes -- take-out, put-in, and all thirteen SetupItem_Base2
     * swaps. This used to be 1.0, which ran every remote tool animation at
     * exactly double rate. */
    cKF_SkeletonInfo_R_init_standard_setframeandspeedandmorphandmode(
        &render->item_keyframe, (cKF_Animation_R_c*)mPlib_Get_Item_DataPointer(anim), NULL, start_frame, 0.5f, 0.0f,
        mode);
    render->loaded_item_shape = (s16)shape;
    render->loaded_item_anim = (s16)anim;
    render->loaded_item_kind = (s8)kind;
    render->item_skeleton_loaded = TRUE;

    if (mPlayer_ITEM_IS_BALLOON(kind) && !was_balloon) {
        Net_Remote_Player_reset_balloon(render);
    }
}

/* Player_actor_Item_CulcAnimation_balloon_normal plus the lean chase from
 * Player_actor_Item_set_balloon_lean_angle. The lean target is
 * -shape_info.rotation.x, which is terrain pitch; the server replicates yaw
 * only, so a remote's target is flat. That is the one place a remote balloon
 * differs from a local one. */
static void Net_Remote_Player_balloon_move(ACTOR* actor, NET_REMOTE_RENDER_DATA* render) {
    f32 speed = render->item_keyframe.frame_control.speed;
    f32 cur = render->item_keyframe.frame_control.current_frame;
    f32 max = render->item_keyframe.frame_control.max_frames;

    add_calc_short_angle2(&render->balloon_lean_angle, -actor->shape_info.rotation.x, 1.0f - sqrtf(0.90999999f), 250,
                          0);

    render->balloon_anim_max_frame = cur;
    cur += speed;
    render->balloon_anim_speed = speed;

    if (cur > max) {
        cur = max;
    } else if (cur < 0.5f * max) {
        cur = 0.5f * max;
    }

    render->item_keyframe.frame_control.current_frame = cur;
}

/* Player_actor_Item_Movement_balloon_normal. Every input is either the remote's
 * own interpolated motion or the replicated main index. */
static void Net_Remote_Player_balloon_movement(AC_NET_REMOTE_PLAYER* remote, NET_REMOTE_RENDER_DATA* render) {
    ACTOR* actor = (ACTOR*)remote;
    f32 max = render->item_keyframe.frame_control.max_frames;

    if (render->item_scale == 1.0f) {
        f32 speed = (26.0f * (render->item_keyframe.frame_control.current_frame - 1.0f)) / (max - 1.0f);
        xyz_t pos = ZeroVec;

        if (render->balloon_stop_movement_flag == FALSE) {
            s16 ang = render->balloon_lean_angle;
            s16 rot = actor->shape_info.rotation.y;
            s16 target;
            f32 cos = cos_s(ang);
            xyz_t hand_move = render->hand_move;

            pos.y -= hand_move.y * cos;

            {
                f32 cos2 = cos_s(DEG2SHORT_ANGLE2(90.0f) - ang);
                f32 sin_rot = sin_s(rot);
                f32 cos_rot = cos_s(rot);
                f32 poscalc = ((sin_rot * hand_move.x) + (cos_rot * hand_move.z));

                pos.y -= poscalc * cos2;
            }

            {
                f32 balloon_add_rot_z = render->balloon_add_rot_z;
                s16 add_z_angle;

                balloon_add_rot_z -= (0.0014f * render->balloon_angle.z);
                add_z_angle = render->balloon_angle.z + (int)balloon_add_rot_z;

                if (add_z_angle > 0x800) {
                    add_z_angle = 0x800;
                } else if (add_z_angle < -0x800) {
                    add_z_angle = -0x800;
                }

                render->balloon_angle.z = add_z_angle;
                render->balloon_add_rot_z = balloon_add_rot_z;
            }

            {
                f32 sin_rot = sin_s(rot);
                f32 cos_rot = cos_s(rot);
                s16 balloon_angle_z = (s16)(-1200.0f * ((sin_rot * hand_move.x) + (cos_rot * hand_move.z)));
                s16 balloon_angle_x = render->balloon_angle.x;
                int xang = ABS(balloon_angle_x);
                int zang = ABS(balloon_angle_z);

                if (zang - xang < 0) {
                    add_calc_short_angle2(&render->balloon_angle.x, balloon_angle_z, 1.0f - sqrtf(0.9f), 2500, 0);
                } else {
                    add_calc_short_angle2(&render->balloon_angle.x, balloon_angle_z, 1.0f - sqrtf(0.6f), 2500, 0);
                }
            }

            {
                s16 add_x_angle = render->balloon_add_rot_x;

                if (remote->action == mPlayer_INDEX_WALK || remote->action == mPlayer_INDEX_RUN) {
                    s16 counter = render->balloon_add_rot_x_counter;
                    f32 sin;

                    counter += (s16)(400.0f * actor->speed);
                    sin = sin_s(counter);
                    render->balloon_add_rot_x_counter = counter;

                    target = (s16)(1000.0f * sin);
                } else {
                    target = 0;
                }

                add_calc_short_angle2(&add_x_angle, target, 1.0f - sqrtf(0.6f), 2500, 0);
                render->balloon_add_rot_x = add_x_angle;
            }
        }

        speed += pos.y;
        if (speed < 13.0f) {
            speed = 13.0f;
        } else if (speed > 26.0f) {
            speed = 26.0f;
        }

        render->item_keyframe.frame_control.current_frame = 1.0f + ((speed * (max - 1.0f)) / 26.0f);
    } else {
        render->balloon_angle.z = 0;
    }

    {
        f32 item_speed = render->item_keyframe.frame_control.speed;
        f32 cur = render->item_keyframe.frame_control.current_frame;
        f32 curmax = 0.7f * max;

        if (cur >= max) {
            item_speed = -0.085f;
        } else if (item_speed <= 0.0f && cur <= curmax) {
            item_speed = 0.0f;
        } else {
            item_speed += 0.0039585f;
        }

        render->item_keyframe.frame_control.speed = item_speed;
    }
}

/* Player_actor_Item_PlayAnimation_balloon_normal. */
static void Net_Remote_Player_balloon_play(NET_REMOTE_RENDER_DATA* render) {
    f32 cur = render->item_keyframe.frame_control.current_frame;
    f32 max = render->item_keyframe.frame_control.max_frames;

    if (render->balloon_current_frame != cur) {
        f32 cur_diff = cur - render->balloon_current_frame;
        f32 old_speed = render->item_keyframe.frame_control.speed;

        if (cur_diff >= 0.0f) {
            render->item_keyframe.frame_control.start_frame = 1.0f;
            render->item_keyframe.frame_control.end_frame = max;
        } else {
            render->item_keyframe.frame_control.end_frame = 1.0f;
            render->item_keyframe.frame_control.start_frame = max;
        }

        render->item_keyframe.frame_control.speed = cur_diff;
        cKF_SkeletonInfo_R_play(&render->item_keyframe);
        render->item_keyframe.frame_control.current_frame = cur;
        render->balloon_current_frame = cur;
        render->item_keyframe.frame_control.speed = old_speed;
    }
}

static int Net_Remote_Player_balloon_draw_Before(GAME* game, cKF_SkeletonInfo_R_c* keyframe, int joint_idx,
                                                 Gfx** joint_shape, u8* joint_flags, void* arg, s_xyz* joint_rot,
                                                 xyz_t* joint_pos) {
    (void)keyframe;
    (void)joint_shape;
    (void)joint_flags;
    (void)arg;
    (void)joint_rot;
    (void)joint_pos;
    switch (joint_idx) {
        case 1:
        case 2:
        case 3:
            OPEN_POLY_OPA_DISP(game->graph);
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetTexEdgeAlpha(POLY_OPA_DISP++, 80);
            CLOSE_POLY_OPA_DISP(game->graph);
            break;
    }
    return 1;
}

static int Net_Remote_Player_balloon_draw_After(GAME* game, cKF_SkeletonInfo_R_c* keyframe, int joint_idx,
                                                Gfx** joint_shape, u8* joint_flags, void* arg, s_xyz* joint_rot,
                                                xyz_t* joint_pos) {
    (void)keyframe;
    (void)joint_shape;
    (void)joint_flags;
    (void)arg;
    (void)joint_rot;
    (void)joint_pos;
    switch (joint_idx) {
        case 1:
        case 2:
        case 3:
            OPEN_POLY_OPA_DISP(game->graph);
            gDPPipeSync(POLY_OPA_DISP++);
            gDPSetTexEdgeAlpha(POLY_OPA_DISP++, 144);
            CLOSE_POLY_OPA_DISP(game->graph);
            break;
    }
    return 1;
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
    /* The same shadow the local player builds in Player_actor_ct. Without it
     * shadow_proc stays NULL -- Actor_info_make_actor's default -- and
     * Actor_draw skips the shadow entirely, so every remote player floated over
     * unshaded ground. ofs_y is restored afterwards because Shape_Info_init
     * takes it as a parameter and the player's 200.0f is set after the call
     * there too. */
    Shape_Info_init(actor, 0.0f, &mAc_ActorShadowCircle, 18.0f, 18.0f);
    /* Shape_Info_init enables it; hold it off until the appearance has actually
     * loaded. Actor_draw runs shadow_proc whether or not the draw proc bailed
     * out, so a remote whose skeleton is not built yet would otherwise be a
     * shadow on the ground with nobody standing on it.
     * Net_Remote_Player_update_shadow turns it on from the same branch that
     * confirms the skeleton. */
    actor->shape_info.draw_shadow = FALSE;
    actor->shape_info.ofs_y = 200.0f;
    actor->cull_width = 80.0f;
    actor->cull_height = 100.0f;
    actor->cull_distance = 1200.0f;
    actor->cull_radius = 80.0f;
    remote->umbrella_actor = NULL;
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
        render->loaded_item_kind = -1;
        render->loaded_item_shape = -1;
        render->loaded_item_anim = -1;
        render->item_scale = 1.0f;
        mPlib_Face_Reset(&render->face);
    }
    remote->render_data = render;
}

static void Net_Remote_Player_dt(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    (void)game;
    /* The umbrella keeps a parent_actor pointer and dereferences parent->drawn
     * every frame, so it must not outlive this actor. */
    if (remote->umbrella_actor != NULL && Common_Get(clip).tools_clip != NULL) {
        Common_Get(clip).tools_clip->aTOL_chg_request_mode_proc(actor, remote->umbrella_actor, aTOL_ACTION_DESTRUCT);
    }
    remote->umbrella_actor = NULL;
    if (remote->render_data != NULL) {
        NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
        if (render->initialized) {
            cKF_SkeletonInfo_R_dt(&render->keyframe0);
            cKF_SkeletonInfo_R_dt(&render->keyframe1);
        }
        if (render->item_skeleton_loaded) {
            cKF_SkeletonInfo_R_dt(&render->item_keyframe);
        }
        zelda_free(render);
        remote->render_data = NULL;
    }
}

/* Player_actor_SetupItemScale: the tool is full size except while it is being
 * taken out or put away, where the original grows and shrinks it. Both are
 * main-index states, so a viewer can reproduce the ramp from the replicated
 * action and its own animation clock rather than replicating the scale. */
static void Net_Remote_Player_update_item_scale(AC_NET_REMOTE_PLAYER* remote, NET_REMOTE_RENDER_DATA* render) {
    f32 frame = render->keyframe0.frame_control.current_frame;

    if (remote->action == mPlayer_INDEX_TAKEOUT_ITEM) {
        /* get_percent_forAccelBrake(timer, 36, 54, 0, 0) in
         * m_player_main_takeout_item.c_inc: flat until 36, ramp to 1 by 54. */
        if (frame <= 36.0f) {
            render->item_scale = 0.0f;
        } else if (frame >= 54.0f) {
            render->item_scale = 1.0f;
        } else {
            render->item_scale = (frame - 36.0f) / 18.0f;
        }
    } else if (remote->action == mPlayer_INDEX_PUTIN_ITEM) {
        /* 1 - get_percent_forAccelBrake(timer, 0, 18, 0, 0). */
        if (frame >= 18.0f) {
            render->item_scale = 0.0f;
        } else if (frame <= 0.0f) {
            render->item_scale = 1.0f;
        } else {
            render->item_scale = 1.0f - (frame / 18.0f);
        }
    } else {
        render->item_scale = 1.0f;
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
            actor->speed = sqrtf(states[i].transform.velocity_x * states[i].transform.velocity_x +
                                 states[i].transform.velocity_z * states[i].transform.velocity_z);
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
            /* Already bounds-checked by the protocol decoder against
             * kPlayerActionCount / kPlayerItemStateCount. */
            remote->action = states[i].transform.action;
            remote->item_state = states[i].animation_item_state;
            remote->missing_frames = 0;
            if (Net_Remote_Player_refresh_appearance(remote, &states[i])) {
                NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
                Net_Remote_Player_apply_animation(render, &states[i]);
                /* Set the playback speed before advancing, the same order
                 * Player_actor_CulcAnimation_Walk uses. */
                Net_Remote_Player_update_animation_speed(remote, render);
                cKF_SkeletonInfo_R_combine_play(&render->keyframe0, &render->keyframe1, render->part_table);
                mPlib_Face_Step(&render->face, (int)remote->action, (int)states[i].animation_body,
                                render->keyframe0.frame_control.current_frame,
                                render->keyframe0.frame_control.max_frames, dt);
                Net_Remote_Player_update_item_scale(remote, render);
                /* After the keyframe has been advanced: the lean reads the
                 * speed that was just applied, and the door shadow reads the
                 * animation translation of the frame about to be drawn. */
                Net_Remote_Player_update_lean(remote, render);
                Net_Remote_Player_update_shadow(remote, render);
                Net_Remote_Player_refresh_item(remote, game);
                if (render->item_visible && render->item_skeleton_loaded) {
                    if (mPlayer_ITEM_IS_BALLOON(render->loaded_item_kind)) {
                        Net_Remote_Player_balloon_move(actor, render);
                    } else if (!render->item_frame_held) {
                        cKF_SkeletonInfo_R_play(&render->item_keyframe);
                    }
                }
            }
            return;
        }
    }
    /* No sample this frame. Hold the last pose and leave the actor alone:
     * Net_SynchronizeRemoteActors owns the lifetime and counts the absence, so
     * a brief gap in the snapshot stream no longer destroys the actor and
     * restarts the animation on the replacement. */
}

/* The hand joint's matrix is only available while the skeleton is being walked,
 * so it is captured here and the item drawn after the skeleton -- the same
 * order Player_actor_draw_After_hand and Player_actor_Item_draw use for the
 * local player. The frame-to-frame delta is what drives the balloon's sway. */
static int Net_Remote_Player_draw_after(GAME* game, cKF_SkeletonInfo_R_c* kf, int joint_no, Gfx** gfx_pp,
                                        u8* work_flag, void* arg, s_xyz* rot, xyz_t* pos) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)arg;
    NET_REMOTE_RENDER_DATA* render;
    xyz_t last_hand_pos;
    (void)game;
    (void)kf;
    (void)gfx_pp;
    (void)work_flag;
    (void)rot;
    (void)pos;
    if (remote == NULL || joint_no != mPlayer_JOINT_HAND) return TRUE;
    render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    if (render == NULL) return TRUE;
    last_hand_pos = render->hand_pos;
    Matrix_Position_Zero(&render->hand_pos);
    if (render->hand_pos_valid) {
        render->hand_move.x = render->hand_pos.x - last_hand_pos.x;
        render->hand_move.y = render->hand_pos.y - last_hand_pos.y;
        render->hand_move.z = render->hand_pos.z - last_hand_pos.z;
    } else {
        render->hand_move = ZeroVec;
    }
    Matrix_get(&render->right_hand_mtx);
    render->hand_pos_valid = TRUE;
    return TRUE;
}

/* The balloon is not drawn under the hand matrix like the other tools: the
 * original builds its own matrix from the hand position plus the accumulated
 * lean and sway. Transcribed from Player_actor_Item_draw_balloon. */
static void Net_Remote_Player_draw_balloon(AC_NET_REMOTE_PLAYER* remote, GAME* game) {
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    ACTOR* actor = (ACTOR*)remote;
    GAME_PLAY* play = (GAME_PLAY*)game;
    GRAPH* graph = game->graph;
    Mtx* item_mtx;
    s16 angle;
    s16 rot;
    f32 scale;

    if (!_Game_play_isPause(play)) {
        Net_Remote_Player_balloon_movement(remote, render);
        Net_Remote_Player_balloon_play(render);
    }

    item_mtx = render->item_work_mtx[game->frame_counter % 2];

    Matrix_push();

    rot = actor->shape_info.rotation.y;
    scale = render->item_scale;
    angle = DEG2SHORT_ANGLE2(-90.0f) + render->balloon_lean_angle + render->balloon_angle.x +
            render->balloon_add_rot_x;

    Matrix_translate(render->hand_pos.x, render->hand_pos.y, render->hand_pos.z, MTX_LOAD);
    Matrix_RotateY(rot, MTX_MULT);
    Matrix_RotateX(angle, MTX_MULT);
    Matrix_RotateZ(0x4000, MTX_MULT);
    Matrix_RotateX(render->balloon_angle.z, MTX_MULT);
    Matrix_scale(actor->scale.x * scale, actor->scale.y * scale, actor->scale.z * scale, MTX_MULT);

    OPEN_POLY_OPA_DISP(graph);
    gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    CLOSE_POLY_OPA_DISP(graph);

    Setpos_HiliteReflect_init(&render->hand_pos, play);

    cKF_Si3_draw_R_SV(game, &render->item_keyframe, item_mtx, Net_Remote_Player_balloon_draw_Before,
                      Net_Remote_Player_balloon_draw_After, actor);
    Matrix_pull();

    if (render->balloon_stop_movement_flag != FALSE) {
        render->balloon_stop_movement_flag = FALSE;
    }
}

/* Draw the tool the same way Player_actor_Item_draw does: under the hand joint
 * matrix, scaled by item_scale, dispatching on whether the item shape is a bare
 * display list or its own skeleton.
 *
 * The original's per-tool procs additionally capture axe_pos, net_pos,
 * net_start/end_pos, the net collision points and the rod tip. Every one of
 * those feeds collision or the fishing state machine, and a presentation actor
 * has neither, so only the rendering is reproduced. */
static void Net_Remote_Player_draw_held_item(AC_NET_REMOTE_PLAYER* remote, GAME* game) {
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    GRAPH* graph = game->graph;
    Mtx* mtx;
    f32 scale;

    if (render == NULL || !render->hand_pos_valid || !render->item_visible) return;
    if (render->item_scale <= 0.0f) return;

    if (render->item_skeleton_loaded && mPlayer_ITEM_IS_BALLOON(render->loaded_item_kind)) {
        Net_Remote_Player_draw_balloon(remote, game);
        return;
    }

    scale = render->item_scale;
    Matrix_push();
    Matrix_put(&render->right_hand_mtx);
    if (scale != 1.0f) {
        Matrix_scale(scale, scale, scale, MTX_MULT);
    }
    mtx = _Matrix_to_Mtx_new(graph);
    if (mtx != NULL) {
        _texture_z_light_fog_prim(graph);
        OPEN_POLY_OPA_DISP(graph);
        gSPMatrix(POLY_OPA_DISP++, mtx, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        CLOSE_POLY_OPA_DISP(graph);

        if (remote->umbrella_actor != NULL) {
            /* Hand the pole matrix to the tool actor, which draws itself.
             * Exactly what Player_actor_Item_draw_umbrella does. */
            TOOLS_ACTOR* umbrella = (TOOLS_ACTOR*)remote->umbrella_actor;
            Matrix_get(&umbrella->matrix_work);
            umbrella->init_matrix = TRUE;
        } else if (render->item_skeleton_loaded) {
            cKF_Si3_draw_R_SV(game, &render->item_keyframe, render->item_work_mtx[game->frame_counter % 2], NULL, NULL,
                              NULL);
        } else if (mPlayer_ITEM_DATA_VALID(render->loaded_item_shape)) {
            OPEN_POLY_OPA_DISP(graph);
            gSPDisplayList(POLY_OPA_DISP++, mPlib_Get_Item_DataPointer(render->loaded_item_shape));
            CLOSE_POLY_OPA_DISP(graph);
        }
    }
    Matrix_pull();
}

static void Net_Remote_Player_draw(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    NET_REMOTE_RENDER_DATA* render = (NET_REMOTE_RENDER_DATA*)remote->render_data;
    GRAPH* graph = game->graph;

    if (render == NULL || !render->initialized) return;
    _texture_z_light_fog_prim(graph);
    OPEN_POLY_OPA_DISP(graph);
    /* The 0xE00 face resource is 8 eye tiles followed by 6 mouth tiles, all
     * 0x100 bytes; the two segments select one of each. This used to be pinned
     * to tile 0, which is why remote players never blinked or emoted. */
    gSPSegment(POLY_OPA_DISP++, ANIME_1_TXT_SEG, render->face_texture + render->face.eye_tex_idx * 0x100);
    gSPSegment(POLY_OPA_DISP++, ANIME_2_TXT_SEG,
               render->face_texture + (mPlayer_EYE_TEX_NUM + render->face.mouth_tex_idx) * 0x100);
    gSPSegment(POLY_OPA_DISP++, ANIME_3_TXT_SEG, render->clothing_texture);
    gSPSegment(POLY_OPA_DISP++, ANIME_4_TXT_SEG, render->clothing_palette);
    gSPSegment(POLY_OPA_DISP++, ANIME_5_TXT_SEG, render->face_palette);
    CLOSE_POLY_OPA_DISP(graph);
    cKF_Si3_draw_R_SV(game, &render->keyframe0, render->work_mtx[game->frame_counter & 1], NULL,
                      &Net_Remote_Player_draw_after, remote);
    Net_Remote_Player_draw_held_item(remote, game);
}

#endif
