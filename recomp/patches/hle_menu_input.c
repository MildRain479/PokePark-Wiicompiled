/* The game's menus with buttons of their own (ACT_MENU_* in the settings:
 * in them B goes back, where in the park X dashes).
 *
 * The menus (the title, the save files, the Pad's pages, talking) read the
 * remote through one function, fn_8012A7F0, which copies the KPAD status
 * the pad manager keeps (GetKPADStatus) once a frame. The park's pause
 * (SceneSpecializerPark::RunPause, and fn_80171640 under it) reads that
 * status too: in the park for + and - (the Pad, the map), and while the
 * Pad is open for closing it. Pikachu, the camera, the scripts and the
 * minigames read the pad manager's own states instead (GetPadState), and
 * Pikachu stops reading while a menu is up.
 *
 * So the menus' reading, and the pause's while Pikachu isn't reading the
 * pad, are given the buttons as the menus have them bound
 * (hle_wpad_menu_buttons): held, pressed and let-go words, worked out the
 * way KPAD does at each KPAD read, written into the status for them and
 * put back once they return. Everything else reads what it always did.
 *
 * Only 2, 1, A and B are changed (all the menus bind apart): the pad
 * manager's status has the d-pad already turned for the remote held
 * sideways, so the rest is left as it says (written whole, the d-pad
 * went a quarter turn round: right moved down the title's menu).
 *
 * KPADStatus: u32 hold, trig, release, then the rest (pointer, motion).
 * RECOMP_MENU_BINDINGS=0 leaves the status alone (tests); RECOMP_MENU_LOG=1
 * prints the menus' buttons as they change, and when Pikachu starts and
 * stops reading the pad. */
#include <stdio.h>
#include <stdlib.h>

#include "hle.h"

/* Where the readers' calls of GetKPADStatus return to. */
#define MENU_READ_RETURN 0x8012A840u   /* fn_8012A7F0 */
#define PAUSE_READ_RETURN 0x8017093Cu  /* RunPause */
#define PAUSE2_READ_RETURN 0x8017167Cu /* fn_80171640, under RunPause */
/* ControlModuleImpl's call of GetPadState: Pikachu reading the pad. */
#define PIKACHU_READ_RETURN 0x800820E0u
/* What the menus bind apart: 2, 1, B and A (WPAD_BUTTON_*). */
#define MENU_OWN_BITS 0x0F00u

static uint32_t sHold, sTrig, sRelease;
static uint32_t sPatched, sSaved[3]; /* the status changed, and what it said */
static int sDepth;                   /* readers under way (they nest: the Pad's pages run in the pause) */
static uint64_t sPikachuRead;        /* when, on the time base */

static int log_on(void) {
    static int log = -1;
    if (log < 0) {
        const char* e = getenv("RECOMP_MENU_LOG");
        log = e && *e && *e != '0';
    }
    return log;
}

/* Whether Pikachu has read the pad in the last tenth of a second: in the
 * park, no menu up (talking included). */
static int pikachu_reading(void) { return sPikachuRead && ppc_timebase() - sPikachuRead < PPC_TB_HZ / 10; }

/* s32 KPADiRead(s32 chan, KPADStatus* samples, u32 length, ...): a frame's
 * reading. */
void f_80254310(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    uint32_t samples = c->r[4];
    f_80254310_orig(c);
    if (chan == 0) {
        uint32_t hold = hle_wpad_menu_buttons();
        if (log_on() && hold != sHold)
            fprintf(stderr, "menu buttons: %04X (the park's: %04X)\n", hold, LD32(samples) & 0xFFFF);
        sTrig = hold & ~sHold;
        sRelease = sHold & ~hold;
        sHold = hold;
    }
}

/* void* static_803719d8::GetPadState(this, s32 chan) */
void f_80034D50(ppc_t* c) {
    static int was;
    if (c->lr == PIKACHU_READ_RETURN) sPikachuRead = ppc_timebase();
    if (log_on() && pikachu_reading() != was) {
        was = !was;
        fprintf(stderr, "menu buttons: Pikachu %s the pad\n", was ? "reads" : "stopped reading");
    }
    f_80034D50_orig(c);
}

/* KPADStatus* static_803719d8::GetKPADStatus(this, s32 chan) */
void f_80034D60(ppc_t* c) {
    static int off = -1;
    uint32_t back = c->lr;
    int32_t chan = (int32_t)c->r[4];
    int menus;
    if (off < 0) {
        const char* e = getenv("RECOMP_MENU_BINDINGS");
        off = e && *e == '0';
    }
    f_80034D60_orig(c);
    menus = back == MENU_READ_RETURN ||
            ((back == PAUSE_READ_RETURN || back == PAUSE2_READ_RETURN) && !pikachu_reading());
    if (!off && menus && sDepth && chan == 0 && c->r[3] && !sPatched) {
        uint32_t st = c->r[3];
        sSaved[0] = LD32(st);
        sSaved[1] = LD32(st + 4);
        sSaved[2] = LD32(st + 8);
        ST32(st, (sSaved[0] & ~MENU_OWN_BITS) | (sHold & MENU_OWN_BITS));
        ST32(st + 4, (sSaved[1] & ~MENU_OWN_BITS) | (sTrig & MENU_OWN_BITS));
        ST32(st + 8, (sSaved[2] & ~MENU_OWN_BITS) | (sRelease & MENU_OWN_BITS));
        sPatched = st;
    }
}

/* The status put back once the outermost reader returns, for what reads
 * it next (UpdatePadStates, HOME). */
static void reader_done(void) {
    if (--sDepth == 0 && sPatched) {
        ST32(sPatched, sSaved[0]);
        ST32(sPatched + 4, sSaved[1]);
        ST32(sPatched + 8, sSaved[2]);
        sPatched = 0;
    }
}

/* The menus' reading of the remote. */
void f_8012A7F0(ppc_t* c) {
    sDepth++;
    f_8012A7F0_orig(c);
    reader_done();
}

/* SceneSpecializerPark::RunPause (fn_80171640 runs under it). */
void f_80170910(ppc_t* c) {
    sDepth++;
    f_80170910_orig(c);
    reader_done();
}
