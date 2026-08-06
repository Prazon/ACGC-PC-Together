#ifndef AC_NET_REMOTE_PLAYER_H
#define AC_NET_REMOTE_PLAYER_H

#include "m_actor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ac_net_remote_player_s {
    ACTOR actor_class;
    u64 account_id;
    u64 entity_id;
    u32 zone_id;
    u32 missing_frames;
    u8 name[8];
    u8 gender;
    u8 face;
    u16 clothing;
    u16 equipped_item;
    u8 transition_phase;
    u32 transition_door;
    u32 transition_expires_tick;
    void* render_data;
} AC_NET_REMOTE_PLAYER;

extern ACTOR_PROFILE Net_Remote_Player_Profile;

#ifdef __cplusplus
}
#endif

#endif
