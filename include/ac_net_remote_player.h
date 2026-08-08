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
    u16 clothing_index;
    u32 appearance_revision;
    u8 pattern_present;
    u8 pattern_palette;
    u8 transition_phase;
    u32 transition_door;
    u32 transition_expires_tick;
    /* mPlayer_INDEX_*, replicated in the transform as `action`. The face and
     * the tool take-out/put-away scale both key off it. */
    u16 action;
    /* mPlayer_ITEM_MAIN_*: which tool state the owning client is in, and
     * therefore whether the tool is in the hand at all. */
    u8 item_state;
    /* Resource selectors the viewer cannot derive from anything it holds: the
     * face texture (stung, decoyed) and palette (tanned), the umbrella's
     * open/close action, and the item held mid-pickup. Bounds-checked by the
     * protocol decoder before they reach here. */
    u8 bee_swell;
    u8 decoy;
    u8 change_color;
    u8 sunburn;
    u8 umbrella_state;
    u16 carried_item;
    /* An umbrella is a real TOOLS_ACTOR child, exactly as it is for the local
     * player; every other tool is drawn straight from the item skeleton. */
    ACTOR* umbrella_actor;
    void* render_data;
} AC_NET_REMOTE_PLAYER;

extern ACTOR_PROFILE Net_Remote_Player_Profile;

#ifdef __cplusplus
}
#endif

#endif
