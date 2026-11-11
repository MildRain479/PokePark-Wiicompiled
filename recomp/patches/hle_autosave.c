/* Autosave (the Gameplay setting): the game saved, as the PokéPark Pad's
 * Save does, whenever the player makes a friend or enters another area.
 *
 * The Pad's save is two script calls on the GlobalManager
 * (GlobalManager::syscall0x15): 0xC8 starts it (the play time brought up
 * to date, the save data written out and its write to the NAND posted, to
 * the slot the game was loaded from, GM+0x5FE4; GM+0x5FE8 is 3 while it's
 * under way) and 0xC9, asked each frame, says whether it's still busy (1)
 * or done (0, the write's request released). A friend made is
 * FriendNoteSaveDataOne::mark_as_friend on a Pokémon not yet one; an area
 * entered, OtherDataSingleMode::set_target_zone_data with another area or
 * zone than the one there (hle_game.c tells us; loading a game sends the
 * one saved). The save waits for the park's own frames (the minimap's
 * drawing, hle_gx.c), so for the end of a scene, a menu or a battle, and
 * for any save already under way. It also waits until no event script has
 * run for half a second: a story scene in the park still draws the
 * minimap, and a friend made in one (Chikorita's, on the first path) comes
 * before the scene's end is written down, so a save then would play the
 * scene again on loading. The EventScriptInterface's scripts in use are
 * the pool's less its free list, as EventManager::RunBegin counts them. */
#include "hle.h"

#define SAVE_START 0xC8u
#define SAVE_POLL 0xC9u
#define GM_SLOT 0x5FE4u  /* the save slot the game was loaded from */
#define GM_STATE 0x5FE8u /* 3: a save under way */
#define SAVING 3u
#define SLOTS 4u /* GD0 to GD3 */
#define SCRIPTS 0x8048645Cu /* EventScriptInterface::_instance */
#define SCRIPT_POOL 0x16Cu  /* the free list's links */
#define SCRIPT_COUNT 0x170u /* the pool's size */
#define SCRIPT_FREE 0x174u  /* the first free one, -1 for none */
#define QUIET_FRAMES 30     /* park frames with no script running */

static int sOn;      /* the setting */
static int sWanted;  /* something to save */
static int sOurs;    /* a save of ours under way */
static int sQuiet;   /* park frames since a script last ran */

/* Whether an event script (a story scene, a talk) is running. */
static int script_running(void) {
    uint32_t esi = LD32(SCRIPTS), count, free = 0, i;
    if (!esi) return 0;
    count = LD32(esi + SCRIPT_COUNT);
    for (i = LD32(esi + SCRIPT_FREE); i != 0xFFFFFFFFu && free <= count; free++)
        i = LD32(LD32(esi + SCRIPT_POOL) + i * 4);
    return free != count;
}

void hle_set_autosave(int on) { sOn = on; }

void hle_autosave_area(uint32_t other, uint32_t area, uint32_t zone) {
    if (LD8(other + 0x20) != (area & 0xFF) || LD8(other + 0x21) != (zone & 0xFF)) {
        if (sOn) HLE_LOG("autosave: area %u zone %u entered", area & 0xFF, zone & 0xFF);
        sWanted |= sOn;
    }
}

/* void FriendNoteSaveDataOne::mark_as_friend() */
void f_80127390(ppc_t* c) {
    if (!(LD8(c->r[3]) & 0x80)) {
        if (sOn) HLE_LOG("autosave: a friend made");
        sWanted |= sOn;
    }
    f_80127390_orig(c);
}

/* A script call on the GlobalManager, its arguments (none) on the stack. */
static uint32_t global_call(ppc_t* c, uint32_t gm, uint32_t id) {
    uint32_t sp = c->r[1];
    c->r[1] = (sp - 0x40) & ~7u;
    ST32(c->r[1] + 0x10, 0);
    c->r[3] = gm;
    c->r[4] = id;
    c->r[5] = c->r[1] + 0x10;
    f_801827C0_orig(c);
    c->r[1] = sp;
    return c->r[3];
}

/* int GlobalManager::syscall0x15(u32 id, Value* args): the Pad's save
 * asked for while one of ours is under way finishes ours (the Pad then
 * asks after it, 0xC9, as after its own), and saves again after. */
void f_801827C0(ppc_t* c) {
    if (c->r[4] == SAVE_START && sOurs && LD32(c->r[3] + GM_STATE) == SAVING) {
        sWanted = 1;
        c->r[3] = 1;
        return;
    }
    f_801827C0_orig(c);
}

/* Each of the park's frames, on the game's thread. */
void hle_autosave_update(ppc_t* c) {
    uint32_t lr = c->lr, r3 = c->r[3], gm;
    if (!sOurs && !sWanted) return;
    if (!sOurs && (script_running() ? (sQuiet = 0, 1) : ++sQuiet < QUIET_FRAMES)) return;
    f_8002E840(c);
    gm = c->r[3];
    if (sOurs) {
        if (LD32(gm + GM_STATE) != SAVING || !global_call(c, gm, SAVE_POLL)) {
            sOurs = 0;
            HLE_LOG("autosave: saved");
        }
    } else if (LD32(gm + GM_STATE) == 0 && LD32(gm + GM_SLOT) < SLOTS) {
        sWanted = 0;
        sQuiet = 0;
        global_call(c, gm, SAVE_START);
        sOurs = LD32(gm + GM_STATE) == SAVING;
        HLE_LOG("autosave: saving to slot %u", LD32(gm + GM_SLOT));
    }
    c->lr = lr;
    c->r[3] = r3;
}
