/* pc_discord_text.h - presence string composition, free of SDL/Windows/IPC.
 *
 * Split out of pc_discord.c so the wording rules can be compiled and checked
 * on any host, and so a future non-Windows IPC backend reuses them verbatim.
 */
#ifndef PC_DISCORD_TEXT_H
#define PC_DISCORD_TEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors the states of AcNetClientStatus that change the wording. The caller
 * maps, rather than this header including the netcode API: REJECTED/FAILED
 * read as offline, since a failed online attempt should look like a normal
 * single-player session and never surface an error on a public profile. */
enum {
    PC_DISCORD_NET_OFFLINE = 0,
    PC_DISCORD_NET_CONNECTING = 1,
    PC_DISCORD_NET_CONNECTED = 2,
    PC_DISCORD_NET_RECONNECTING = 3
};

typedef struct pc_discord_inputs_s {
    int save_loaded;                /* a town save is live; scene_no is meaningful */
    int scene_no;                   /* SCENE_* from the save, when save_loaded */
    const unsigned char* town_name; /* game font codes, NULL when unknown */
    size_t town_name_len;
    int net_status;                 /* PC_DISCORD_NET_* */
    int nearby_players;             /* remote players in the interest set (not the town) */
    int town_population;            /* town-wide, 0 when the server reports none */
    int town_capacity;
} pc_discord_inputs_t;

/* Always writes NUL-terminated strings; state may come back empty, meaning
 * "no second line". Output is ASCII only: it is escaped into JSON and length
 * clamped downstream, and a clamp that split a multi-byte sequence would make
 * Discord reject the whole frame. */
void pc_discord_compose(const pc_discord_inputs_t* in,
                        char* details, size_t details_size,
                        char* state, size_t state_size);

#ifdef __cplusplus
}
#endif

#endif /* PC_DISCORD_TEXT_H */
