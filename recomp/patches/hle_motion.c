/* Minigame controls: keys, sticks and the mouse as the Wii Remote's
 * motion. The minigames are scripts, and the only motion they ask about
 * (AttractionInput::virt0x18) is the remote's tilt (KPAD's acceleration,
 * x and y), how upright it is (KPAD's acc_vertical), and in two of them
 * where it points; besides those, shakes, swings and pushes are bursts of
 * acceleration. Each minigame's instructions (Lobby.bmg) say how it is
 * played; here each gets the remote held as they ask, moved by:
 *
 *   running (Absol, Bulbasaur, Dusknoir): shake = the two shake keys by
 *     turns (Q and E, LB and RB): a swing up or down for each press after
 *     the other key's, so the faster they go by turns, the faster the
 *     shaking (about 8 presses a second wins Bulbasaur's dash in 9 s);
 *   swinging on a vine or rope (Venusaur, Tangrowth): held upright; the
 *     shake keys by turns swing it back and forth, in time with the vine;
 *     B pushes it forward to jump, as the game asks ("Push [B] as shown"),
 *     and so does 2 (Space);
 *   bashing boulders (Blaziken): held upright; 1 or 2 (left click, Space)
 *     swings it like a bat, toward the screen; the shake keys by turns
 *     shake it from side to side to recover;
 *   steering (Gyarados, Empoleon): held sideways, tilted left and right
 *     by left/right (A/D, arrows) or the left stick, in proportion;
 *   flying (Pelipper, Salamence, Rayquaza): also tilted forward and back,
 *     by up/down or the stick: up tilts it back (climb), down forward
 *     (dive);
 *   rolling (Rhyperior): the same, the other way: up tilts it forward,
 *     as the game goes up the screen for that;
 *   pointing (Rotom, Rayquaza's last part): the mouse or the right stick
 *     moves the pointer, as two sensor-bar dots in the IR camera; past
 *     the screen's sides it turns Rotom's view.
 *
 * With a gyroscope (a DualSense, a Switch controller, the handheld itself;
 * boot/motion.h), the settings can have turning it move the pointer too
 * (gyro aiming), and tilting it steer, like a wheel, and climb and dive
 * (gyro steering: tilted back climbs, from how it was held as the
 * minigame began); the keys and sticks still go first. Rayquaza's game
 * isn't steered by tilt: its course turns too sharply for it, and the
 * controller is turned to aim at the end.
 *
 * The d-pad still goes to the game as well. RECOMP_TILT_FLIP=lr, fb or
 * lrfb turns tilting the other way, and RECOMP_POINTER_FLIP=y moves the
 * pointer the other way up, in case a minigame expects it so. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"

void (*g_host_pointer_read)(float* dx, float* dy);
void (*g_host_sticks_read)(float* lx, float* ly, float* rx, float* ry);
int (*g_host_gyro_read)(float* yaw, float* pitch);
int (*g_host_tilt_read)(float* roll, float* pitch);

/* One g in the accelerometer's units (hle_wpad.c's), and a swing. */
#define G 104
#define SWING_REPORTS 6 /* 30 ms at 200 reports a second */
#define PI 3.14159265f

enum { HOLD_SIDEWAYS, HOLD_UPRIGHT };
enum {
    USE_SHAKE = 1, USE_STEER = 2, USE_PITCH = 4, USE_SWING = 8, USE_PUSH = 16, USE_BAT = 32, USE_POINTER = 64,
    USE_PITCH_ROLL = 128,
    USE_NO_TILT = 256 /* steered by the keys and sticks alone, not gyro steering */
};

static const struct {
    int at, hold, use;
} kMinigames[] = {
    {1, HOLD_SIDEWAYS, USE_SHAKE},                          /* Absol's Hurdle Bounce */
    {2, HOLD_SIDEWAYS, USE_STEER | USE_PITCH | USE_POINTER | USE_NO_TILT}, /* Rayquaza's Balloon Panic */
    {3, HOLD_UPRIGHT, USE_SWING | USE_PUSH},                /* Venusaur's Vine Swing */
    {4, HOLD_UPRIGHT, USE_SWING | USE_PUSH},                /* Tangrowth's Swing-Along */
    {5, HOLD_SIDEWAYS, USE_SHAKE},                          /* Dusknoir's Speed Slam */
    {6, HOLD_SIDEWAYS, USE_STEER},                          /* Gyarados' Aqua Dash: a button for speed */
    {7, HOLD_SIDEWAYS, USE_STEER | USE_PITCH},              /* Pelipper's Circle Circuit */
    {9, HOLD_SIDEWAYS, USE_STEER},                          /* Empoleon's Snow Slide */
    {10, HOLD_SIDEWAYS, 0},                                 /* Bastiodon's Panel Crush: buttons */
    {11, HOLD_SIDEWAYS, USE_STEER | USE_PITCH | USE_PITCH_ROLL}, /* Rhyperior's Bumper Burn */
    {12, HOLD_UPRIGHT, USE_BAT | USE_SHAKE},                /* Blaziken's Boulder Bash */
    {13, HOLD_SIDEWAYS, USE_POINTER | USE_SHAKE},           /* Rotom's Spooky Shoot-'em-Up */
    {15, HOLD_SIDEWAYS, USE_STEER | USE_PITCH},             /* Salamence's Air Race */
    {16, HOLD_SIDEWAYS, USE_SHAKE},                         /* Bulbasaur's Daring Dash */
};

/* How far the keys and stick tilt the remote, and how quickly it gets
 * there (a share of the way each report: about 30 ms). The minigames'
 * scripts read the accelerometer themselves (FScriptLoader::syscall0x15);
 * at 35 degrees their turns were too wide to make, so left and right go
 * all the way over. They also go by readings of about 1 g alone, as
 * from a remote held still: at 2.4 g across, Gyarados' Aqua Dash kept
 * turning right for as long as left was held after it. */
#define MAX_STEER (90.0f * PI / 180.0f)
/* Forward and back as far as climbing goes: Pelipper's balloon climbs
 * as fast at 50 degrees as at 70, and at 30 too slowly. */
#define MAX_PITCH (50.0f * PI / 180.0f)
#define EASE 0.15f
/* Gyro steering: the tilt (from how it was held as the minigame began)
 * that steers or climbs all the way. */
#define TILT_STEER_FULL (30.0f * PI / 180.0f)
#define TILT_PITCH_FULL (25.0f * PI / 180.0f)
#define TILT_SETTLE_REPORTS 100 /* half a second */

/* The pointer: how far a pixel of mouse movement and a second of full
 * stick move it (the screen is -1 to 1 across), and where its dots sit in
 * the IR camera's 1024x768 view. The stick goes by its tilt squared:
 * slow near the middle, for aiming, and across the screen in about half
 * a second at full tilt, for turning (at 1.5 it took 1.3 seconds and
 * aiming felt sluggish). */
#define POINTER_PER_PIXEL (1.0f / 600.0f)
#define POINTER_PER_STICK_SECOND 3.5f
#define DOT_SPREAD 100 /* each dot from the middle */
/* Gyro aiming: across half the screen for 20 degrees of turn. */
#define POINTER_PER_RADIAN (180.0f / PI / 20.0f)

/* How far the pointer goes across, past the screen's edges: Rotom's game
 * turns to look around while it points past them (from about 1.4; the
 * cursor stays at the edge). Much past 1.6 the game loses the pointer
 * (1.8 hid it and stopped the turning). */
#define POINTER_X_LIMIT 1.6f
/* Where a game that doesn't turn its view keeps it (Rayquaza's: past the
 * screen's sides its pointer is gone), and the screen's top and bottom
 * (KPAD's 1 is 226/182 of the way). Turning the controller while steering
 * by tilt moved the hidden pointer as far as it went, off the screen by
 * the time it was wanted. */
#define POINTER_X_SCREEN 1.0f
#define POINTER_Y_LIMIT (182.0f / 226.0f)

static int flip(const char* env, const char* what) {
    const char* e = getenv(env);
    return e && strstr(e, what) != NULL;
}

static float clamp1(float v) { return v > 1 ? 1 : v < -1 ? -1 : v; }

static int16_t clamp16(float v) { return (int16_t)(v > 511 ? 511 : v < -512 ? -512 : v); }

/* A swing of the shake keys by turns: +1 or -1 while one lasts, else 0.
 * Holding a key, or the same key again, makes none. */
static int shake_swing(uint32_t held, uint32_t report) {
    static uint32_t last, swingEnd;
    static int side, sign;
    uint32_t pressed = held & ~last & (HOST_PAD_SHAKE | HOST_PAD_SHAKE2);
    int which = pressed == HOST_PAD_SHAKE ? 1 : pressed == HOST_PAD_SHAKE2 ? 2 : 0;
    last = held;
    if (which && which != side) {
        side = which;
        sign = which == 1 ? 1 : -1;
        swingEnd = report + SWING_REPORTS;
    }
    return (int32_t)(swingEnd - report) > 0 ? sign : 0;
}

/* A pair of opposite keys: +1 for plus, -1 for minus, and with both held
 * the one pressed last, so rolling from one to the other turns straight
 * over instead of stopping until the first is let go. */
typedef struct {
    uint32_t last;
    int side;
} KeyPair;

static float key_pair(KeyPair* k, uint32_t held, uint32_t plus, uint32_t minus) {
    uint32_t pressed = held & ~k->last;
    k->last = held;
    if (!(held & minus)) k->side = held & plus ? 1 : 0;
    else if (!(held & plus)) k->side = -1;
    else if (pressed & plus) k->side = 1;
    else if (pressed & minus) k->side = -1;
    return (float)k->side;
}

/* A gesture set off by a press of one of the keys in mask, lasting
 * length reports: the reports since it began, or -1 if none is under way. */
typedef struct {
    uint32_t last, began;
    int on;
} Gesture;

static int gesture(Gesture* g, uint32_t held, uint32_t mask, uint32_t report, int length) {
    if (g->on && report - g->began >= (uint32_t)length) g->on = 0;
    if ((held & mask) && !(g->last & mask) && !g->on) {
        g->began = report;
        g->on = 1;
    }
    g->last = held;
    return g->on ? (int)(report - g->began) : -1;
}

int hle_motion(uint32_t held, uint32_t report, int16_t acc[3], int16_t dots[4][2]) {
    static float steer, pitch, px, py;
    static Gesture push, bat;
    static KeyPair leftRight, upDown;
    static float level, levelRoll; /* gyro steering: the tilt taken as level */
    static int levelAt;            /* the minigame it was taken in */
    static uint32_t lastTilt;      /* the report it was last read at */
    static uint32_t tiltFrom;      /* the report it came at (after none) */
    static int pointerAt;          /* the minigame the pointer was centred for */
    int at = hle_minigame(), i, use = -1, hold = HOLD_SIDEWAYS, swing, tilt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, x, y, z, wantSteer = 0, wantPitch = 0, roll = 0, tiltPitch = 0;
    if (!at) return 0;
    for (i = 0; i < (int)(sizeof(kMinigames) / sizeof(kMinigames[0])); i++) {
        if (kMinigames[i].at == at) {
            use = kMinigames[i].use;
            hold = kMinigames[i].hold;
        }
    }
    if (use < 0) use = USE_SHAKE; /* not one we know: shaking, which is common */
    if (g_host_sticks_read) g_host_sticks_read(&lx, &ly, &rx, &ry);
    if ((use & USE_STEER) && !(use & USE_NO_TILT) && g_host_tilt_read && g_host_tilt_read(&roll, &tiltPitch)) {
        /* Level as it was held when the minigame began (or the tilt
         * came back after a second without it). */
        if (levelAt != at || report - lastTilt > 200) {
            levelAt = at;
            tiltFrom = report;
        }
        lastTilt = report;
        /* Level is how it is held once the sensors have settled (half a
         * second: their first readings can be far off); no steering by
         * tilt until then. */
        if (report - tiltFrom <= TILT_SETTLE_REPORTS) {
            level = tiltPitch;
            levelRoll = roll;
            if (report - tiltFrom == TILT_SETTLE_REPORTS)
                fprintf(stderr, "motion: level taken: roll %.1f, pitch %.1f degrees\n", roll * 180 / PI,
                        tiltPitch * 180 / PI);
        } else {
            /* Steering from level too: held a little to one side, it
             * doesn't keep turning that way. */
            roll -= levelRoll;
            tilt = 1;
        }
    }

    /* Tilt, toward where the keys or the stick put it. */
    if (use & USE_STEER) {
        /* Right lowers the pointer end (to the left, held sideways), as
         * the minigames turn right: lifting it turned them left. */
        wantSteer = key_pair(&leftRight, held, HOST_PAD_SCREEN_LEFT, HOST_PAD_SCREEN_RIGHT);
        if (lx != 0) wantSteer = -lx;
        else if (wantSteer == 0 && tilt) wantSteer = clamp1(-roll / TILT_STEER_FULL);
        if (flip("RECOMP_TILT_FLIP", "lr")) wantSteer = -wantSteer;
    }
    if (use & USE_PITCH) {
        /* Up climbs: the remote tilted back. */
        wantPitch = key_pair(&upDown, held, HOST_PAD_SCREEN_DOWN, HOST_PAD_SCREEN_UP);
        if (ly != 0) wantPitch = -ly;
        if (use & USE_PITCH_ROLL) wantPitch = -wantPitch;
        if (wantPitch == 0 && tilt) {
            /* The far edge up from level (tilted back) climbs, as the stick
             * pulled back; the remote tilted as the controller is (in
             * Rhyperior's game too, where the keys go the other way). */
            float d = tiltPitch - level;
            d = d > PI ? d - 2 * PI : d < -PI ? d + 2 * PI : d;
            wantPitch = clamp1(-d / TILT_PITCH_FULL);
        }
        if (flip("RECOMP_TILT_FLIP", "fb")) wantPitch = -wantPitch;
    }
    steer += (wantSteer * MAX_STEER - steer) * EASE;
    pitch += (wantPitch * MAX_PITCH - pitch) * EASE;

    /* Gravity, as the accelerometer reads it (up along an axis reads +g on
     * it), in the remote's axes: x across its face, y along it (toward the
     * pointer end), z out of its face. Sideways, the pointer end is to the
     * left: a positive steer lifts it (turning left), and tilting forward
     * lifts the edge nearest the player (+x). Upright, the pointer end is up and the face
     * toward the player. Steering and pitch each set their own axis, as
     * if tilted one way and then the other, so steering hard keeps most of
     * the climb or dive (a single rotation would take it away); z, out of
     * the face, makes up the rest of g. Together they could come to more
     * than g, which the games would pass over, so then both shrink. */
    if (hold == HOLD_UPRIGHT) {
        x = 0;
        y = (float)G;
        z = 0;
    } else {
        float across;
        x = sinf(pitch) * G;
        y = sinf(steer) * G;
        across = sqrtf(x * x + y * y);
        if (across > G) {
            x = x * G / across;
            y = y * G / across;
            across = (float)G;
        }
        z = sqrtf((float)G * G - across * across);
    }

    /* Bursts of acceleration on top. */
    swing = (use & (USE_SHAKE | USE_SWING | USE_BAT)) ? shake_swing(held, report) : 0;
    if (swing) {
        /* Up and down, out of its face, whichever way it is held: running
         * reads only that (in Bulbasaur's dash, a shake along its length
         * did nothing); upright, that is toward the screen and away. With
         * a bat that would swing it, so there the shake (to recover from
         * a boulder) goes from side to side. */
        if (use & USE_BAT) x += swing * G * 5 / 2;
        else z += swing * G * 5 / 2;
    }
    if (use & USE_PUSH) {
        /* A push toward the screen: a thrust, then the stop. */
        int t = gesture(&push, held, HOST_PAD_B | HOST_PAD_2, report, 10);
        if (t >= 0) z += t < 5 ? -3 * G : 3 * G / 2;
    }
    if (use & USE_BAT) {
        /* A bat's swing toward the screen, the one way Blaziken's game
         * counts (swung across the body or along its length, 0): away
         * from the face as it sets off, then the stop. Stop first and
         * set-off after, as it was, took two presses for one swing: the
         * first press's set-off and the second's stop. Each part is held
         * for more than two game frames, so the game sees it. */
        int t = gesture(&bat, held, HOST_PAD_1 | HOST_PAD_2, report, 16);
        if (t >= 0) z += t < 8 ? -2 * G : 3 * G;
    }
    acc[0] = clamp16(x);
    acc[1] = clamp16(y);
    acc[2] = clamp16(z);

    if (!(use & USE_POINTER)) return 1;
    /* The pointer, as the IR camera sees the sensor bar: the dots move
     * the other way to the pointer (pointing right puts the bar to the left
     * of the view, pointing up puts it lower). */
    {
        static int env = -1;
        static float ex, ey, ep;
        float dx = 0, dy = 0;
        if (env < 0) {
            /* RECOMP_POINTER_STICK=<x>,<y>[,<seconds>]: the right stick held
             * there, for unattended runs; with seconds, turned the other
             * way every that long. */
            const char* e = getenv("RECOMP_POINTER_STICK");
            env = e && sscanf(e, "%f,%f,%f", &ex, &ey, &ep) >= 2;
        }
        if (env) {
            int back = ep > 0 && (int)(report / (200.0f * ep)) & 1;
            rx = back ? -ex : ex;
            ry = back ? -ey : ey;
        }
        if (pointerAt != at) {
            /* In the middle as each minigame begins. */
            pointerAt = at;
            px = py = 0;
        }
        if (g_host_pointer_read) g_host_pointer_read(&dx, &dy);
        {
            float gyaw, gpitch;
            if (g_host_gyro_read && g_host_gyro_read(&gyaw, &gpitch)) {
                px += gyaw * POINTER_PER_RADIAN;
                py += gpitch * POINTER_PER_RADIAN;
            }
        }
        px += dx * POINTER_PER_PIXEL + rx * fabsf(rx) * POINTER_PER_STICK_SECOND / 200.0f;
        py += -dy * POINTER_PER_PIXEL + ry * fabsf(ry) * POINTER_PER_STICK_SECOND / 200.0f;
        {
            /* Rotom's game turns its view past the sides; one that is
             * steered too (Rayquaza's) doesn't. */
            float xl = (use & USE_STEER) ? POINTER_X_SCREEN : POINTER_X_LIMIT;
            px = px > xl ? xl : px < -xl ? -xl : px;
            py = py > POINTER_Y_LIMIT ? POINTER_Y_LIMIT : py < -POINTER_Y_LIMIT ? -POINTER_Y_LIMIT : py;
        }
    }
    {
        float yUp = flip("RECOMP_POINTER_FLIP", "y") ? -py : py;
        /* Measured against KPAD's pointer (pos, -1 to 1, y down): x moves
         * 1/225 of the view per pixel; y 1/182, and with the sensor bar
         * below the screen (SYSCONF) the middle is 102 pixels lower. */
        int mx = (int)(512 - px * 225), my = (int)(486 + yUp * 226);
        dots[0][0] = (int16_t)(mx - DOT_SPREAD);
        dots[0][1] = (int16_t)my;
        dots[1][0] = (int16_t)(mx + DOT_SPREAD);
        dots[1][1] = (int16_t)my;
    }
    return 3;
}
