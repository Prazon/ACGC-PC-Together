/* PS1 emulator scene (PC only) — mirrors src/famicom_emu.c's structure.
 * Reached through the stock famicom flow: the PlayStation furniture passes
 * rom_no PSX_EMU_ROM_BASE, the yes/no dialog and fade run unchanged, and
 * famicom_emu_init dispatches to psx_emu_boot instead of famicom_init. */
#ifdef TARGET_PC

#include "psx_emu.h"

#include "jsyswrap.h"
#include "libjsys/jsyswrapper.h"
#include "m_common_data.h"
#include "m_scene.h"

#include "pc_libretro_core.h"
#include "pc_psx.h"

extern int g_pc_nes_active; /* blocks pause menu + forces 60fps pacing (pc_vi.c) */

/* TRUE while the pending emulator request came from the PlayStation item —
 * lets shared famicom dialog text (e.g. quit instructions, msg 0x17B5) swap
 * "NES" for "PlayStation" at load time (m_msg_main.c_inc). */
int g_pc_psx_msg_flavor = 0;

static int psx_done = FALSE;
static int psx_done_countdown = 0;
static u32 psx_save_timer = 0;

static u16 psx_map_pad(pad_t* pad) {
    u16 b = pad->now.button;
    u16 mask = 0;

    /* GC → PS1: A=Cross (jump), B=Square (spin), X=Circle, Y=Triangle. */
    if (b & BUTTON_A) mask |= 1 << PC_LR_PAD_B;
    if (b & BUTTON_B) mask |= 1 << PC_LR_PAD_Y;
    if (b & BUTTON_X) mask |= 1 << PC_LR_PAD_A;
    if (b & BUTTON_Y) mask |= 1 << PC_LR_PAD_X;
    if (b & BUTTON_START) mask |= 1 << PC_LR_PAD_START;
    if (b & BUTTON_Z) mask |= 1 << PC_LR_PAD_SELECT;
    if (b & BUTTON_L) mask |= 1 << PC_LR_PAD_L;
    if (b & BUTTON_R) mask |= 1 << PC_LR_PAD_R;
    if (b & BUTTON_DUP) mask |= 1 << PC_LR_PAD_UP;
    if (b & BUTTON_DDOWN) mask |= 1 << PC_LR_PAD_DOWN;
    if (b & BUTTON_DLEFT) mask |= 1 << PC_LR_PAD_LEFT;
    if (b & BUTTON_DRIGHT) mask |= 1 << PC_LR_PAD_RIGHT;

    /* Main stick doubles as the d-pad (Crash is digital-only). */
    if (pad->now.stick_y > 20) mask |= 1 << PC_LR_PAD_UP;
    if (pad->now.stick_y < -20) mask |= 1 << PC_LR_PAD_DOWN;
    if (pad->now.stick_x < -20) mask |= 1 << PC_LR_PAD_LEFT;
    if (pad->now.stick_x > 20) mask |= 1 << PC_LR_PAD_RIGHT;

    return mask;
}

static void psx_emu_main(GAME* psx) {
    int padid;
    pad_t* current_pad;
    u32 combo;

    if (!psx_done) {
        for (padid = 0; padid < 4; padid++) {
            current_pad = &gamePT->pads[padid];
            combo = current_pad->now.button | current_pad->on.button;

            if (combo == (BUTTON_L | BUTTON_R | BUTTON_Z) ||
                combo == (BUTTON_L | BUTTON_R | BUTTON_X | BUTTON_Y)) {
                psx_done = TRUE;
                psx_done_countdown = 60;
                JC_JFWDisplay_startFadeOut(JC_JFWDisplay_getManager(), psx_done_countdown);
                break;
            }
        }
    }

    if (psx_done) {
        if (psx_done_countdown == 0) {
            return_emu_game(psx);
        } else {
            psx_done_countdown--;
        }
    }

    JW_BeginFrame();
    psx->disable_display = 1;

    if (!psx_done) {
        sAdo_GameFrame(); /* pump audio engine each frame, like the NES scene */
        pc_psx_frame(psx_map_pad(&gamePT->pads[0]));
        pc_psx_render();

        /* Flush the memory card periodically so a crash can't lose much. */
        if (++psx_save_timer >= 60 * 30) {
            psx_save_timer = 0;
            pc_psx_save();
        }
    } else {
        static GXColor black_color = { 0, 0, 0, 0 };
        void* manager = JC_JFWDisplay_getManager();

        JC_JFWDisplay_clearEfb(manager, black_color);
    }
    JW_EndFrame();
}

static void psx_emu_cleanup(GAME* psx) {
    (void)psx;
    JC_JFWDisplay_startFadeIn(JC_JFWDisplay_getManager(), 1);
    pc_psx_shutdown(); /* saves the memory card, unloads core, restores GL */
    sAdo_SubGameEnd();
    g_pc_nes_active = 0;

    /* Clear the internal-ROM-launch flag (bit3) so the room doesn't show the
     * NES "saved Famicom data" dialog on return; its other purpose (starting
     * sub-game audio in the room actor's dt) already ran before we booted. */
    Common_Set(my_room_message_control_flags, Common_Get(my_room_message_control_flags) & ~8);
}

extern void psx_emu_boot(GAME* game) {
    int i;

    g_pc_nes_active = 1;
    psx_done = FALSE;
    psx_done_countdown = 0;
    psx_save_timer = 0;

    game_resize_hyral(game, 0);

    game->exec = psx_emu_main;
    game->cleanup = psx_emu_cleanup;

    /* Pump audio until it transitions to sub-game mode (famicom_emu.c does
     * the same; sAdo_SubGameStart was requested by the room actor's dt). */
    for (i = 0; i < 120 && sAdo_SubGameOK() == FALSE; i++) {
        sAdo_GameFrame();
    }

    if (pc_psx_boot_selected() != PC_PSX_OK) {
        /* Same UX as a failed NES ROM load: "no game data" dialog in-room. */
        Common_Set(my_room_message_control_flags, Common_Get(my_room_message_control_flags) | 1);
        return_emu_game(game);
    }
}

#endif /* TARGET_PC */
