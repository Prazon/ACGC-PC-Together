/* PS1 console — PC-only custom furniture (FTR_PSX_CONSOLE).
 * Model generated from PSONE.obj by tools/converters/obj2gfx.py.
 * Interaction runs the stock famicom dialog flow with a PSX sentinel
 * rom_no, which famicom_emu_init routes to the libretro-based PS1
 * emulator scene (src/psx_emu.c). */
#include "psx_emu.h"

extern u8 int_psx_tex[];
extern u16 int_psx_pal[];
extern Gfx int_psx_model[];

static void aPsxConsole_ct(FTR_ACTOR* ftr_actor, u8* data) {
    // nothing
}

static void aPsxConsole_mv(FTR_ACTOR* ftr_actor, ACTOR* my_room_actor, GAME* game, u8* data) {
    if (Common_Get(clip).my_room_clip != NULL) {
#ifdef TARGET_PC
        /* agb_rom_no 255 = "no GBA version" (see fFC_agb_game_table); -1
         * would wrongly offer the GBA-transfer dialog choice. */
        Common_Get(clip).my_room_clip->famicom_emu_common_move_proc(ftr_actor, my_room_actor, game,
                                                                   PSX_EMU_ROM_BASE, 255);
#else
        /* No PSX scene off-PC — fall back to the external NES ROM browser. */
        Common_Get(clip).my_room_clip->famicom_emu_common_move_proc(ftr_actor, my_room_actor, game, 0, -1);
#endif
    }
}

static void aPsxConsole_dw(FTR_ACTOR* ftr_actor, ACTOR* my_room_actor, GAME* game, u8* data) {
    OPEN_DISP(game->graph);

    gSPMatrix(NEXT_POLY_OPA_DISP, _Matrix_to_Mtx_new(game->graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(NEXT_POLY_OPA_DISP, int_psx_model);

    CLOSE_DISP(game->graph);
}

static void aPsxConsole_dt(FTR_ACTOR* ftr_actor, u8* data) {
    // nothing
}

static aFTR_vtable_c aPsxConsole_func = {
    &aPsxConsole_ct,
    &aPsxConsole_mv,
    &aPsxConsole_dw,
    &aPsxConsole_dt,
    NULL,
};

aFTR_PROFILE iam_psx_console = {
    // clang-format off
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    18.0f,
    0.01f,
    aFTR_SHAPE_TYPEA,
    mCoBG_FTR_TYPEA,
    0,
    0,
    0,
    aFTR_INTERACTION_FAMICOM_ITEM,
    &aPsxConsole_func,
    // clang-format on
};
