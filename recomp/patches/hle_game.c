/* Game-specific changes (not hardware emulation).
 *
 * Boot screens: the game boots into the Strap scene (the wrist strap and
 * safety notice), then Hold ("Hold Wii Remote Horizontally"), then the
 * StartMenu (title). Strap sets up the game and starts ParkPrepare, the
 * background load of the park's data, and stays up for a fixed time; Hold
 * then shows for a few more seconds after the load has finished. Here
 * Strap hands over to Hold as soon as the load is done. Hold still runs
 * its course (see f_80173E10). RECOMP_BOOT_SCREENS=1 keeps the original
 * screens. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"

/* Scenes, as indexed by the scene table (0x80337FA8). */
enum { SCENE_PARK, SCENE_ATTRACTION, SCENE_START_MENU, SCENE_LOBBY, SCENE_STRAP, SCENE_HOLD, SCENE_CHECK_DATA };

/* ParkPrepare::_static.fState (+0x60): 2 once the park's data is loaded. */
#define PARK_PREPARE_STATE 0x803A21D8
#define PARK_PREPARE_DONE 2

static int sLeftStrap;

static int keep_boot_screens(void) {
    const char* e = getenv("RECOMP_BOOT_SCREENS");
    return e && *e && *e != '0';
}

static int sScene = -1;
static int sSwallowChange;

/* BOOL fn_801741F0(s32 scene): picks the next scene, by index. */
void f_801741F0(ppc_t* c) {
    /* The Strap scene's script, still running after f_80173E10 has left
     * it, picks Hold again when its time is up: see f_8016C950. */
    if (sLeftStrap == 1 && sScene == SCENE_HOLD && c->r[3] == SCENE_HOLD) {
        sLeftStrap = 2;
        sSwallowChange = 1;
        c->r[3] = 1;
        return;
    }
    HLE_LOG("next scene: %d", (int)c->r[3]);
    sScene = (int)c->r[3];
    f_801741F0_orig(c);
}

/* void SceneManager::fn_8016C950(void): sets fChangeScene (+0x2C), which
 * switches to the scene picked. Not for the Strap script's late switch to
 * Hold (above): coming after the one f_80173E10 made, it would start Hold
 * over, reinitializing the heaps under what the boot screens had loaded.
 * PrepareSceneDat, with every minigame's records, was among it, and the
 * resource manager went on finding the records there: a minigame's lobby
 * then read garbage for them and never got as far as showing the friends
 * to pick from. */
void f_8016C950(ppc_t* c) {
    if (sSwallowChange) {
        sSwallowChange = 0;
        return;
    }
    f_8016C950_orig(c);
}

/* Whether the scene is a minigame (an attraction). */
int hle_in_minigame(void) { return sScene == SCENE_ATTRACTION; }

/* void AttractionManager::LoadAttractionResources(int index, ...): loads
 * At<index + 1>Dat and Res, the minigame about to start. */
static int sAttraction;

void f_80136B20(ppc_t* c) {
    sAttraction = (int)c->r[4] + 1;
    f_80136B20_orig(c);
}

/* The minigame being played (its At number, 1-16), or 0 outside one. */
int hle_minigame(void) { return sScene == SCENE_ATTRACTION ? sAttraction : 0; }

/* Testing a minigame (the settings menu's Debug section, or
 * RECOMP_ATTRACTION=<n> once in the park): its lobby is entered from the
 * park the way its gate does it, whether or not the story has reached it.
 * The gate's script leaves the minigame's index (one less than the At
 * number) in the next scene's parameters as AttractionId and ends the
 * park scene. The park's sequencer then takes the index from there, keeps
 * it, and switches to the lobby, passing it on to the minigame and back
 * each time. */
#define ADD_SCENE_PARAMETER 0x8016CCC0 /* void AddSceneParameter(Params*, const char* name, s32 value) */
#define ATTRACTION_ID_NAME 0x80336C68  /* "AttractionId" */
#define SCENE_NEXT_PARAMS 0x98         /* SceneManager: the next scene's parameters */
#define END_SCENE 0x8016CC30           /* BOOL SceneManager::fn_8016CC30(void): ends the scene */
static int sMinigameRequest;

void hle_request_minigame(int at) { sMinigameRequest = at; }

/* void SceneSpecializerPark::RunBegin(void): each frame of the park, at a
 * point where the scene can be changed. */
void f_80171630(ppc_t* c) {
    static int envDone;
    const char* e = getenv("RECOMP_ATTRACTION");
    int at;
    f_80171630_orig(c);
    if (!envDone && e && *e) {
        /* RECOMP_ATTRACTION=<n>,<seconds>: after that long (in free roam). */
        static uint64_t start;
        const char* comma = strchr(e, ',');
        if (!start) start = ppc_timebase();
        if ((double)(ppc_timebase() - start) / PPC_TB_HZ >= (comma ? atof(comma + 1) : 0)) {
            envDone = 1;
            sMinigameRequest = atoi(e);
        }
    }
    at = sMinigameRequest;
    if (at < 1 || at > 16) return;
    sMinigameRequest = 0;
    f_80019620(c); /* SceneManager::Get */
    c->r[5] = (uint32_t)(at - 1);
    hle_call(c, ADD_SCENE_PARAMETER, c->r[3] + SCENE_NEXT_PARAMS, ATTRACTION_ID_NAME);
    f_80019620(c);
    hle_call(c, END_SCENE, c->r[3], 0);
}

/* void SceneSpecializerStrap::RunSystem(void): runs every frame. Once the
 * park has loaded, does what the Strap scene's script does when its time
 * is up (scene syscall 0x15, subcommands 1 and 2): pick the next scene,
 * Hold, then start the switch. Hold must not be skipped: the game records
 * that it has run, and a new game started before it has goes through Hold
 * after the opening, and Hold always leads to the start menu. (Leaving
 * Hold as soon as it has seen the load done isn't enough either: its
 * script records it at the end.) */
void f_80173E10(ppc_t* c) {
    if (!sLeftStrap && LD32(PARK_PREPARE_STATE) == PARK_PREPARE_DONE && !keep_boot_screens()) {
        sLeftStrap = 1;
        c->r[3] = SCENE_HOLD;
        f_801741F0(c);
        f_80019620(c); /* SceneManager::Get */
        f_8016C950(c); /* sets fChangeScene (+0x2C) */
    }
}

/* Frame pacing: the game times its updates and draws with periodic
 * alarms, their periods computed at start-up as (bus clock / 4) times
 * 1/59.94 s and 2/59.94 s by four schedulers, each with its own copy of
 * the two constants. The main loop's are a 60 Hz alarm that queues a pad
 * sample for each update, and a 30 Hz alarm that wakes the loop to run
 * the queued updates and draw (after running more than two it skips the
 * next draw). Both alarms count from time base 0, so they keep step only
 * if the 30 Hz period is exactly twice the 60 Hz one. From 1/60 and 2/60
 * rounded down it came out a tick over (1012499 and 2024999 ticks), and
 * as the host's time base is its uptime, the 60 Hz alarm fired anywhere
 * in the frame, creeping 1.8 ms an hour. Where it fired as the loop
 * counted its samples, the loop ran 1 and 3 updates by turns and drew
 * every other frame (15 fps).
 *
 * So the constants follow the emulated refresh rate (hle_irq.c), each
 * step at most one or two retrace periods (a step even slightly longer
 * leaves some frames without one), and with the game's own arithmetic (a
 * single-precision product, truncated) the 30 Hz period is exactly twice
 * the 60 Hz one: the two alarms fire together, the sample first. */
static const uint32_t kFrameStepConsts[] = {0x80487540, 0x804877A0, 0x80488924, 0x80488990};

/* The alarm period, in ticks, that the game makes of step constant f. */
static uint64_t step_ticks(float f) {
    volatile float product = f * (float)PPC_TB_HZ;
    return (uint64_t)product;
}

static float step_const(void) {
    double ticks = PPC_TB_HZ / hle_vi_hz();
    float f = (float)(1.0 / hle_vi_hz()) * 1.001f;
    while ((double)step_ticks(f) > ticks || step_ticks(2 * f) != 2 * step_ticks(f)) f = nextafterf(f, 0.0f);
    return f;
}

void hle_game_init(void) {
    size_t i;
    for (i = 0; i < sizeof(kFrameStepConsts) / sizeof(kFrameStepConsts[0]); i++) {
        ST32(kFrameStepConsts[i], f32_bits(step_const()));
        ST32(kFrameStepConsts[i] + 4, f32_bits(2 * step_const()));
    }
}

/* Frame rate. The main loop draws on its 30 Hz alarm (clock 1) after
 * running one update per 60 Hz pad sample (clock 0), and each scene sets
 * clock 1's period from the scene table: one step (60 fps) for the title
 * and menus, two (30 fps) in the park. At 60 fps clock 1 is held to one
 * step, so the loop draws after every update. The updates themselves are
 * untouched: Pikachu's movement through walking, dashing and jumping is the
 * same position for position, animations advance per update (UpdateFrame
 * is called as often), and what runs twice as often is only posing and
 * drawing models at the current frame. */
#define CLOCKS 0x80374AB0 /* static_80374ab0::_static: two clocks of 0x70 bytes */
#define CLOCK_SIZE 0x70
#define CLOCK_WANT_PERIOD 0x40 /* u64, taken up by the clock's next tick */

static int sFrameRate;        /* 0: the game's own; 60: every update */
static uint64_t sScenePeriod; /* clock 1's period as the scene set it */

static uint64_t draw_period(void) {
    uint64_t one = step_ticks(step_const());
    return sFrameRate >= 60 && sScenePeriod > one ? one : sScenePeriod;
}

/* void set_frame_limit(Clocks*, int clock, u64 period) */
void f_8000BE20(ppc_t* c) {
    if (c->r[4] == 1) {
        uint64_t p;
        sScenePeriod = ((uint64_t)c->r[5] << 32) | c->r[6];
        p = draw_period();
        c->r[5] = (uint32_t)(p >> 32);
        c->r[6] = (uint32_t)p;
    }
    f_8000BE20_orig(c);
}

/* Sets the frame rate, from the settings: 30, or 60 and up (the game
 * draws 60; gl_backend.c draws the frames between). Takes effect on the
 * main loop's next tick. */
void hle_set_frame_rate(int fps) {
    uint64_t p;
    sFrameRate = fps;
    if (!sScenePeriod) return; /* the game hasn't set it up yet */
    p = draw_period();
    ST32(CLOCKS + CLOCK_SIZE + CLOCK_WANT_PERIOD, (uint32_t)(p >> 32));
    ST32(CLOCKS + CLOCK_SIZE + CLOCK_WANT_PERIOD + 4, (uint32_t)p);
}

/* Movies: the scripts open one (ThpManager syscall 0, with a loop flag),
 * then poll its state (syscall 5: 2 while playing, 3 once finished)
 * before closing it. Holding + for half a second reports a movie that
 * doesn't loop as finished, which skips it (no sooner than about a second
 * in); RECOMP_SKIP_MOVIES=1 does that by itself, which skips the 56 s
 * opening for testing. The looping title background plays as usual.
 * (Reporting it finished at once closed the player while it was still
 * starting, and the game then waited forever.) */
enum { THP_SYS_OPEN = 0, THP_SYS_STATE = 5, THP_PLAYING = 2, THP_FINISHED = 3 };
#define SKIP_AFTER_POLLS 60
#define SKIP_HOLD (PPC_TB_HZ / 2) /* + held this long skips a movie */
static int sMovieLoops, sMoviePolls;
static uint64_t sPlusSince; /* when + was pressed, 0 if it isn't held */

/* int ThpManager::virt0x18(int sub, u32* args) */
void f_801776E0(ppc_t* c) {
    static int skip = -1;
    uint32_t sub = c->r[4], args = c->r[5];
    if (skip < 0) {
        const char* e = getenv("RECOMP_SKIP_MOVIES");
        skip = e && *e && *e != '0';
    }
    if (sub == THP_SYS_OPEN) {
        sMovieLoops = LD32(args + 4) != 0;
        sMoviePolls = 0;
    }
    f_801776E0_orig(c);
    if (sub == THP_SYS_STATE && !sMovieLoops && c->r[3] == THP_PLAYING) {
        uint64_t now = ppc_timebase();
        if (!(hle_wpad_held() & HOST_PAD_PLUS)) sPlusSince = 0;
        else if (!sPlusSince) sPlusSince = now;
        if (++sMoviePolls > SKIP_AFTER_POLLS && (skip || (sPlusSince && now - sPlusSince >= SKIP_HOLD))) {
            if (!skip) HLE_LOG("movie skipped (+ held)");
            c->r[3] = THP_FINISHED;
        }
    }
}

void hle_friends_story_data(uint32_t other); /* hle_friends.c */
void hle_autosave_area(uint32_t other, uint32_t area, uint32_t zone); /* hle_autosave.c */

/* void OtherDataSingleMode::set_target_zone_data(u8 area, u8 zone, u16
 * entrance): where the player goes next, told on loading a game too, so
 * where the story's other data is (the berries, for the second screen),
 * and whether to autosave (another area or zone than the one there).
 * RECOMP_ZONE=<area>,<zone>[,<entrance>] sends them elsewhere instead, the
 * first time, for testing; RECOMP_FILM=<n> gives that many rolls of film
 * (its u16 at 0x2C: get_film_roll_amount), for taking photos. */
void f_80127820(ppc_t* c) {
    static int done;
    const char* e = getenv("RECOMP_ZONE");
    const char* film = getenv("RECOMP_FILM");
    hle_friends_story_data(c->r[3]);
    if (film && *film) ST16(c->r[3] + 0x2C, (uint16_t)atoi(film));
    if (!done && e && *e) {
        const char* p = strchr(e, ',');
        done = 1;
        c->r[4] = (uint32_t)atoi(e);
        c->r[5] = p ? (uint32_t)atoi(p + 1) : 1;
        p = p ? strchr(p + 1, ',') : NULL;
        if (p) c->r[6] = (uint32_t)atoi(p + 1);
    }
    hle_autosave_area(c->r[3], c->r[4], c->r[5]);
    f_80127820_orig(c);
}
