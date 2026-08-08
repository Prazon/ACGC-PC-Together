/* Per-game PlayStation consoles — PC-only custom furniture.
 *
 * Follows the US NES convention: one item per game, all sharing the same
 * console model, each booting straight into its own disc with no menu.
 * The disc each item owns is s_item_discs[] in pc/src/pc_psx.c, indexed in
 * the same order as the FTR_PSX_GAME* furniture entries. */

#include "psx_emu.h"
#ifdef TARGET_PC
#include "pc_psx.h"
#endif

extern Gfx int_psx_model[];

static int aPsxGame_index(FTR_ACTOR* ftr_actor) {
    int idx = ((int)ftr_actor->name - FTR_START(FTR_PSX_GAME0)) >> 2;

#ifdef TARGET_PC
    if (idx < 0 || idx >= pc_psx_item_count()) {
        idx = 0;
    }
#endif
    return idx;
}

static void aPsxGame_ct(FTR_ACTOR* ftr_actor, u8* data) {
    // nothing
}

static void aPsxGame_mv(FTR_ACTOR* ftr_actor, ACTOR* my_room_actor, GAME* game, u8* data) {
    if (Common_Get(clip).my_room_clip != NULL) {
#ifdef TARGET_PC
        /* Choose this console's disc up front, then take the stock yes/no
         * dialog rather than the picker. */
        if (ftr_actor->switch_changed_flag) {
            pc_psx_select_item(aPsxGame_index(ftr_actor));
        }
        Common_Get(clip).my_room_clip->famicom_emu_common_move_proc(ftr_actor, my_room_actor, game,
                                                                   PSX_EMU_ROM_DIRECT, 255);
#else
        Common_Get(clip).my_room_clip->famicom_emu_common_move_proc(ftr_actor, my_room_actor, game, 0, -1);
#endif
    }
}

static void aPsxGame_dw(FTR_ACTOR* ftr_actor, ACTOR* my_room_actor, GAME* game, u8* data) {
    OPEN_DISP(game->graph);

    gSPMatrix(NEXT_POLY_OPA_DISP, _Matrix_to_Mtx_new(game->graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(NEXT_POLY_OPA_DISP, int_psx_model);

    CLOSE_DISP(game->graph);
}

static void aPsxGame_dt(FTR_ACTOR* ftr_actor, u8* data) {
    // nothing
}

static aFTR_vtable_c aPsxGame_func = {
    &aPsxGame_ct,
    &aPsxGame_mv,
    &aPsxGame_dw,
    &aPsxGame_dt,
    NULL,
};

#define aPSX_GAME_PROFILE(name)         \
    aFTR_PROFILE name = {               \
        NULL, NULL, NULL, NULL,         \
        NULL, NULL, NULL, NULL,         \
        18.0f,                          \
        0.01f,                          \
        aFTR_SHAPE_TYPEA,               \
        mCoBG_FTR_TYPEA,                \
        0, 0, 0,                        \
        aFTR_INTERACTION_FAMICOM_ITEM,  \
        &aPsxGame_func,                 \
    }

aPSX_GAME_PROFILE(iam_psx_game0);
aPSX_GAME_PROFILE(iam_psx_game1);
aPSX_GAME_PROFILE(iam_psx_game2);
aPSX_GAME_PROFILE(iam_psx_game3);
