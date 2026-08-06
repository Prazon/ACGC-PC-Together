#include "ac_net_remote_player.h"

#ifdef NETCODE_ENABLED

#include "acnet/c_api.h"
#include "m_name_table.h"
#include "m_rcp.h"
#include "sys_matrix.h"

#include <string.h>

static void Net_Remote_Player_ct(ACTOR* actor, GAME* game);
static void Net_Remote_Player_dt(ACTOR* actor, GAME* game);
static void Net_Remote_Player_move(ACTOR* actor, GAME* game);
static void Net_Remote_Player_draw(ACTOR* actor, GAME* game);

/* Presentation-only stand-in. It deliberately has no PLAYER_ACTOR state,
 * controller, camera, inventory, collision, save callback, or world mutation
 * path. The shape is temporary but makes network presence visible without
 * reusing the single-player profile. */
static Vtx net_remote_vertices[] ATTRIBUTE_ALIGN(32) = {
    {{ {-1200,    0, -800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1200,    0, -800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1200, 2700, -800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ {-1200, 2700, -800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ {-1200,    0,  800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1200,    0,  800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1200, 2700,  800}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ {-1200, 2700,  800}, 0, {0, 0}, {255, 255, 255, 255} }},
};

static Vtx net_remote_head_vertices[] ATTRIBUTE_ALIGN(32) = {
    {{ {-1050, 2600, -900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1050, 2600, -900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1050, 4500, -900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ {-1050, 4500, -900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ {-1050, 2600,  900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1050, 2600,  900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ { 1050, 4500,  900}, 0, {0, 0}, {255, 255, 255, 255} }},
    {{ {-1050, 4500,  900}, 0, {0, 0}, {255, 255, 255, 255} }},
};

static Gfx net_remote_model[] ATTRIBUTE_ALIGN(32) = {
    gsDPPipeSync(),
    gsDPSetRenderMode(G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2),
    gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE),
    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR),
    gsSPVertex(net_remote_vertices, 8, 0),
    gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
    gsSP2Triangles(5, 4, 7, 0, 5, 7, 6, 0),
    gsSP2Triangles(4, 0, 3, 0, 4, 3, 7, 0),
    gsSP2Triangles(1, 5, 6, 0, 1, 6, 2, 0),
    gsSP2Triangles(3, 2, 6, 0, 3, 6, 7, 0),
    gsSP2Triangles(4, 5, 1, 0, 4, 1, 0, 0),
    gsSPSetGeometryMode(G_LIGHTING),
    gsSPEndDisplayList(),
};

static Gfx net_remote_head_model[] ATTRIBUTE_ALIGN(32) = {
    gsSPVertex(net_remote_head_vertices, 8, 0),
    gsSP2Triangles(0, 1, 2, 0, 0, 2, 3, 0),
    gsSP2Triangles(5, 4, 7, 0, 5, 7, 6, 0),
    gsSP2Triangles(4, 0, 3, 0, 4, 3, 7, 0),
    gsSP2Triangles(1, 5, 6, 0, 1, 6, 2, 0),
    gsSP2Triangles(3, 2, 6, 0, 3, 6, 7, 0),
    gsSP2Triangles(4, 5, 1, 0, 4, 1, 0, 0),
    gsSPEndDisplayList(),
};

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
    (void)game;
    /* Server movement is intentionally horizontal-only. Project the remote
     * presentation actor onto this client's real foreground height instead
     * of drawing the server's placeholder Y=0 below rolling terrain. */
    actor->world.position.y = mCoBG_GetBgY_OnlyCenter_FromWpos2(actor->world.position, 0.0f);
    actor->last_world_position.y = actor->world.position.y;
    actor->ground_y = actor->world.position.y;
    actor->cull_width = 80.0f;
    actor->cull_height = 100.0f;
    actor->cull_distance = 1200.0f;
    actor->cull_radius = 80.0f;
}

static void Net_Remote_Player_dt(ACTOR* actor, GAME* game) {
    (void)actor;
    (void)game;
}

static void Net_Remote_Player_move(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    AcNetRemotePlayer states[16];
    size_t count;
    size_t i;
    (void)game;

    count = acnet_client_remote_players(states, 16);
    for (i = 0; i < count; ++i) {
        if (states[i].account_id == remote->account_id && states[i].entity_id == remote->entity_id) {
            xyz_t_move(&actor->last_world_position, &actor->world.position);
            actor->world.position.x = states[i].transform.x;
            actor->world.position.z = states[i].transform.z;
            actor->world.position.y = mCoBG_GetBgY_OnlyCenter_FromWpos2(actor->world.position, 0.0f);
            actor->ground_y = actor->world.position.y;
            actor->position_speed.x = states[i].transform.velocity_x;
            actor->position_speed.y = 0.0f;
            actor->position_speed.z = states[i].transform.velocity_z;
            actor->world.angle.y = states[i].transform.yaw;
            actor->shape_info.rotation.y = states[i].transform.yaw;
            remote->zone_id = states[i].zone_id;
            memcpy(remote->name, states[i].name, sizeof(remote->name));
            remote->gender = states[i].gender;
            remote->face = states[i].face;
            remote->clothing = states[i].clothing;
            remote->equipped_item = states[i].equipped_item;
            remote->missing_frames = 0;
            return;
        }
    }
    if (++remote->missing_frames > 180) Actor_delete(actor);
}

static void Net_Remote_Player_draw(ACTOR* actor, GAME* game) {
    AC_NET_REMOTE_PLAYER* remote = (AC_NET_REMOTE_PLAYER*)actor;
    GRAPH* graph = game->graph;
    u32 hash = (u32)(remote->clothing != 0 ? remote->clothing :
                     (remote->account_id ^ (remote->account_id >> 32)));
    u8 red = (u8)(96 + (hash & 0x7F));
    u8 green = (u8)(96 + ((hash >> 7) & 0x7F));
    u8 blue = (u8)(96 + ((hash >> 14) & 0x7F));

    Matrix_translate(actor->world.position.x, actor->world.position.y, actor->world.position.z, MTX_LOAD);
    /* Vertices use the original model convention of hundredths of a world
     * unit. Without this scale the stand-in is 100x larger than a player. */
    Matrix_scale(0.01f, 0.01f, 0.01f, MTX_MULT);
    Matrix_RotateY(actor->shape_info.rotation.y, MTX_MULT);
    OPEN_POLY_OPA_DISP(graph);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, red, green, blue, 255);
    gSPMatrix(POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, net_remote_model);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0,
                    236 - remote->face * 3, 184 - remote->face * 2,
                    142 - remote->face, 255);
    gSPDisplayList(POLY_OPA_DISP++, net_remote_head_model);
    CLOSE_POLY_OPA_DISP(graph);
}

#endif
