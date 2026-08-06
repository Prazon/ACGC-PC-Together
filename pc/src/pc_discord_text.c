/* pc_discord_text.c - what the presence lines say.
 *
 * No SDL, no windows.h, no pipe, no globals: everything comes in through
 * pc_discord_inputs_t. pc_discord.c gathers the inputs on the game thread and
 * this decides the wording.
 */
#include "pc_discord_text.h"

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "m_font.h"
#include "m_scene_table.h"

/* Shown when no town is known: title screen, or online before the server has
 * sent the town name. */
static const char DEFAULT_DETAILS[] = "Playing Animal Crossing";

/* Town names use the game's own font character codes. The 32..122 block
 * mostly coincides with ASCII, but a handful of codes in it are game
 * symbols or accented letters (see m_font.h): those map to a base letter
 * where one exists and are dropped ('\0') otherwise. */
static char pc_discord_font_to_ascii(u8 c) {
    switch (c) {
        case CHAR_ACUTE_a:
        case CHAR_CIRCUMFLEX_a:
        case CHAR_TILDE_a:
        case CHAR_DIARESIS_a:
        case CHAR_ANGSTROM_a:
            return 'a';
        case CHAR_TILDE:
            return '~';
        case CHAR_SYMBOL_HEART:
        case CHAR_SYMBOL_MUSIC_NOTE:
        case CHAR_SYMBOL_DROPLET:
        case CHAR_SYMBOL_ANNOYED:
            return '\0';
        default:
            if ((c >= CHAR_SPACE && c <= CHAR_UNDERSCORE) || (c >= CHAR_a && c <= CHAR_z)) {
                return (char)c; /* these font codes coincide with ASCII */
            }
            return '\0';
    }
}

/* An unwritten name is zero-filled, and code 0 is outside both ASCII ranges
 * above, so it decodes to the empty string and the caller falls back. */
static void pc_discord_town_to_ascii(char* out, size_t out_size, const unsigned char* raw, size_t raw_len) {
    size_t len = 0;
    size_t i;

    for (i = 0; i < raw_len && len + 1 < out_size; i++) {
        char c = pc_discord_font_to_ascii((u8)raw[i]);
        if (c != '\0') {
            out[len++] = c;
        }
    }
    while (len > 0 && out[len - 1] == ' ') len--;
    out[len] = '\0';
}

static const char* pc_discord_location_for_scene(int scene_no) {
    switch (scene_no) {
        case SCENE_FG: return "Outside";
        case SCENE_SHOP0: return "Inside Nook's Cranny";
        case SCENE_BROKER_SHOP: return "Inside Crazy Redd's tent";
        case SCENE_POST_OFFICE: return "Inside the Post Office";
        case SCENE_POLICE_BOX: return "Inside the Police Station";
        case SCENE_CONVENI: return "Inside Nook 'n' Go";
        case SCENE_SUPER: return "Inside Nookway";
        case SCENE_DEPART:
        case SCENE_DEPART_2: return "Inside Nookington's";
        case SCENE_NEEDLEWORK: return "Inside Able Sisters";
        case SCENE_NPC_HOUSE:
        case SCENE_COTTAGE_NPC: return "Visiting a neighbor's house";
        case SCENE_MY_ROOM_S:
        case SCENE_MY_ROOM_M:
        case SCENE_MY_ROOM_L:
        case SCENE_MY_ROOM_LL1:
        case SCENE_MY_ROOM_LL2:
        case SCENE_MY_ROOM_BASEMENT_S:
        case SCENE_MY_ROOM_BASEMENT_M:
        case SCENE_MY_ROOM_BASEMENT_L:
        case SCENE_MY_ROOM_BASEMENT_LL1:
        case SCENE_COTTAGE_MY: return "At home";
        default:
            if (mSc_IS_SCENE_MUSEUM_ROOM(scene_no)) return "At the Museum";
            return NULL; /* menus, demos, and scenes we're not confident labeling */
    }
}

/* Prefers the server's town-wide count. Falls back to the interest set, where
 * "nearby" is deliberate: acnet_client_remote_players() counts only players
 * the client can see, which is not the town's population. */
static void pc_discord_append_company(char* state, size_t state_size, const pc_discord_inputs_t* in) {
    size_t len = strlen(state);
    const char* separator = (len > 0) ? " - " : "";

    if (in->town_population > 0 && in->town_capacity > 0) {
        if (in->town_population == 1) {
            snprintf(state + len, state_size - len, "%salone in town", separator);
        } else {
            snprintf(state + len, state_size - len, "%s%d of %d in town", separator,
                     in->town_population, in->town_capacity);
        }
        return;
    }

    if (in->nearby_players <= 0) return;
    if (in->nearby_players == 1) {
        snprintf(state + len, state_size - len, "%swith 1 other nearby", separator);
    } else {
        snprintf(state + len, state_size - len, "%swith %d others nearby", separator, in->nearby_players);
    }
}

void pc_discord_compose(const pc_discord_inputs_t* in,
                        char* details, size_t details_size,
                        char* state, size_t state_size) {
    char town[32];
    int online;

    if (details_size == 0 || state_size == 0) return;
    snprintf(details, details_size, "%s", DEFAULT_DETAILS);
    state[0] = '\0';
    if (in == NULL) return;

    /* Mid-handshake and mid-reconnect both read as "connecting": the player is
     * not in a town yet, and the previous town's name would be misleading. */
    if (in->net_status == PC_DISCORD_NET_CONNECTING || in->net_status == PC_DISCORD_NET_RECONNECTING) {
        snprintf(details, details_size, "Connecting to a town...");
        return;
    }

    online = (in->net_status == PC_DISCORD_NET_CONNECTED);
    if (!online && !in->save_loaded) return; /* title screen */

    town[0] = '\0';
    if (in->town_name != NULL) {
        pc_discord_town_to_ascii(town, sizeof(town), in->town_name, in->town_name_len);
    }

    if (town[0] == '\0') {
        /* Online with no name yet (still bootstrapping): say so without
         * inventing a town. */
        if (online) snprintf(details, details_size, "Playing online");
        return;
    }

    if (online) {
        snprintf(details, details_size, "Online in the town of %s", town);
    } else {
        snprintf(details, details_size, "In the town of %s", town);
    }

    if (in->save_loaded) {
        const char* location = pc_discord_location_for_scene(in->scene_no);
        if (location != NULL) {
            snprintf(state, state_size, "%s", location);
        }
    }
    if (online) {
        pc_discord_append_company(state, state_size, in);
    }
}
