/* PC-side glue for custom furniture items appended after the stock tables.
 *
 * Stock furniture names are loaded from the disc image into ftrName2_table
 * (242 records of 16 bytes); custom items appended after FTR_DUMMY live past
 * that region, so their names must be filled in here after pc_assets_init().
 */
#include <stdio.h>
#include <string.h>

#include "pc_custom_furniture.h"

#include "m_common_data.h"
#include "m_name_table.h"
#include "m_private.h"

extern unsigned char ftrName2_table[];

void pc_custom_furniture_init(void) {
    /* AC's charset matches ASCII for A-Z a-z 0-9 space and most punctuation
     * (include/m_font.h); records are 16 bytes, space-padded, no terminator.
     * Order must match the FTR_PSX_* furniture entries, which in turn match
     * s_item_discs[] in pc/src/pc_psx.c. */
    static const char names[][17] = {
        "PlayStation     ", /* FTR_PSX_CONSOLE - the disc picker */
        "Crash Bandicoot ", /* FTR_PSX_GAME0 */
        "Spyro the Dragon", /* FTR_PSX_GAME1 */
        "Tomba!          ", /* FTR_PSX_GAME2 */
        "Ape Escape      ", /* FTR_PSX_GAME3 */
    };
    int i;

    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        memcpy(ftrName2_table + 0xF20 + i * 16, names[i], 16);
    }
}

/* Debug helper: F5 puts the PS1 into the first empty inventory slot and marks
 * it collected for the catalog. Temporary until the item is distributed
 * through a shop/event flow. */
void pc_custom_furniture_update(const unsigned char* keys) {
    static int f5_was_down = 0;
    int f5_down = keys[SDL_SCANCODE_F5];

    if (f5_down && !f5_was_down) {
        Private_c* priv = Common_Get(now_private);
        /* player_actor_exists gates out the title/player-select screens, where
         * now_private already points at save player 1 and a write would leak
         * into that player's save data. */
        if (priv != NULL && Common_Get(player_actor_exists)) {
            int slot = mPr_GetPossessionItemIdxWithCond(priv, EMPTY_NO, mPr_ITEM_COND_NORMAL);
            if (slot != -1) {
                mPr_SetPossessionItem(priv, slot, FTR_START(FTR_PSX_CONSOLE), mPr_ITEM_COND_NORMAL);
                mPr_SetItemCollectBit(FTR_START(FTR_PSX_CONSOLE));
                printf("[PC] debug: PlayStation added to pocket slot %d\n", slot);
            } else {
                printf("[PC] debug: no empty pocket slot for PlayStation\n");
            }
        }
    }
    f5_was_down = f5_down;
}
