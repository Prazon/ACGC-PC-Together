#ifndef PC_CUSTOM_FURNITURE_H
#define PC_CUSTOM_FURNITURE_H

#include <SDL2/SDL.h>

/* Fill in name records for custom furniture appended after the stock tables.
 * Call once after pc_assets_init() succeeds, before the game boots. */
void pc_custom_furniture_init(void);

/* Per-frame debug hook (called from PADRead with the SDL keyboard state):
 * F5 gives the custom item. */
void pc_custom_furniture_update(const unsigned char* keys);

#endif /* PC_CUSTOM_FURNITURE_H */
