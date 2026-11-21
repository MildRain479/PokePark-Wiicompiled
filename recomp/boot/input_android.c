/* Wii Remote 0 on Android, for hle_wpad.c, as input_win32.c on Windows:
 * from a controller (Android's gamepad buttons and axes, bound in the
 * settings as on Windows), or from controls on the touch screen. There is
 * no keyboard or mouse.
 *
 * Touch controls are drawn over the picture (menu_draw) while the touch
 * screen was used last, and hidden once a controller is used (or always,
 * with the Touch controls setting off):
 *   left             a stick: the d-pad's directions, and the stick's
 *                    own for moving in any direction (hle_stick.c)
 *   right, bottom    2 (jump), 1 (dash), A (Thunderbolt), B (camera),
 *                    Shake and Shake 2 (in minigames, by turns)
 *   top left         -, + and View (first person, as a controller's right
 *                    stick click)
 *   anywhere else    dragging turns the camera, as the mouse does
 * A touch on the Options tab of the PokéPark Pad, or the save file
 * screen's Options button, opens it; while it is open, touches work it
 * (options.c) and the controls are hidden.
 *
 * Events come in on the activity's thread (window_android.c); the game's
 * thread reads them at each poll. */
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_input.h"
#include "host_settings.h"
#include "motion.h"

int hle_font_metrics(int* cellW, int* cellH, int* baseline); /* hle_prompts.c */
int hle_font_glyph(uint16_t code, uint8_t* cov, int* left, int* advance);
void game_screen_size(int* w, int* h); /* second_screen_android.c */
int second_screens_swapped(void);
void second_touch_push(int action, int id, float x, float y);

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------ */
/* The controller (the activity's thread writes, the game's reads) */

static uint32_t sPadButtons, sPadHat;
static float sAxis[6]; /* left x, y (down positive, as Android has them), right x, y, triggers */

static uint32_t key_bit(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return XPAD_A;
    case AKEYCODE_BUTTON_B: return XPAD_B;
    case AKEYCODE_BUTTON_X: return XPAD_X;
    case AKEYCODE_BUTTON_Y: return XPAD_Y;
    case AKEYCODE_BUTTON_L1: return XPAD_LB;
    case AKEYCODE_BUTTON_R1: return XPAD_RB;
    case AKEYCODE_BUTTON_L2: return XPAD_LT;
    case AKEYCODE_BUTTON_R2: return XPAD_RT;
    case AKEYCODE_BUTTON_START: return XPAD_START;
    case AKEYCODE_BUTTON_SELECT: return XPAD_BACK;
    case AKEYCODE_BUTTON_THUMBL: return XPAD_LTHUMB;
    case AKEYCODE_BUTTON_THUMBR: return XPAD_RTHUMB;
    case AKEYCODE_DPAD_UP: return XPAD_UP;
    case AKEYCODE_DPAD_DOWN: return XPAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return XPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return XPAD_RIGHT;
    case AKEYCODE_BACK: return XPAD_B; /* some controllers' B */
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Touches */

enum { ROLE_FREE, ROLE_NEW, ROLE_STICK, ROLE_BUTTON, ROLE_CAMERA, ROLE_USED };
#define MAX_TOUCHES 10

typedef struct {
    int id, role, gone; /* gone: lifted, still to be seen by the game */
    float x0, y0, x, y, lastX, lastY;
    uint32_t bit; /* ROLE_BUTTON's */
    int64_t down; /* when it touched, in ms */
} Touch;

/* A tap holds its button at least this long, so the game sees a tap
 * lifted between two of its polls. */
#define TAP_MS 50

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static Touch sTouch[MAX_TOUCHES];

static Touch* touch_of(int id) {
    int i;
    for (i = 0; i < MAX_TOUCHES; i++) {
        if (sTouch[i].role != ROLE_FREE && !sTouch[i].gone && sTouch[i].id == id) return &sTouch[i];
    }
    return NULL;
}

static void touch_down(int id, float x, float y) {
    int k;
    for (k = 0; k < MAX_TOUCHES && sTouch[k].role != ROLE_FREE; k++) {}
    if (k < MAX_TOUCHES) {
        Touch* t = &sTouch[k];
        memset(t, 0, sizeof(*t));
        t->id = id;
        t->role = ROLE_NEW;
        t->down = now_ms();
        t->x0 = t->x = t->lastX = x;
        t->y0 = t->y = t->lastY = y;
    }
}

static void touch_move(int id, float x, float y) {
    Touch* t = touch_of(id);
    if (!t) return;
    t->x = x;
    t->y = y;
}

static void touch_up(int id) {
    Touch* t = touch_of(id);
    if (t) t->gone = 1;
}

/* A touch on the screen the game is on: the second one, swapped
 * (second_screen_android.c): 0 down, 1 up, 2 moved. */
void input_android_touch(int action, int id, float x, float y) {
    pthread_mutex_lock(&sLock);
    if (action == 0) touch_down(id, x, y);
    else touch_move(id, x, y);
    if (action == 1) touch_up(id);
    pthread_mutex_unlock(&sLock);
}

static void touch_event(AInputEvent* e) {
    int32_t action = AMotionEvent_getAction(e), kind = action & AMOTION_EVENT_ACTION_MASK;
    size_t p = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    size_t i, n = AMotionEvent_getPointerCount(e);
    /* Swapped, this screen has the Pad's pages: its touches are theirs. */
    if (second_screens_swapped()) {
        int down = kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN;
        int up = kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP;
        if (down || up) {
            second_touch_push(down ? 0 : 1, AMotionEvent_getPointerId(e, p), AMotionEvent_getX(e, p), AMotionEvent_getY(e, p));
        } else {
            for (i = 0; i < n; i++)
                second_touch_push(kind == AMOTION_EVENT_ACTION_CANCEL ? 1 : 2, AMotionEvent_getPointerId(e, i),
                                  AMotionEvent_getX(e, i), AMotionEvent_getY(e, i));
        }
        return;
    }
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN)
        touch_down(AMotionEvent_getPointerId(e, p), AMotionEvent_getX(e, p), AMotionEvent_getY(e, p));
    for (i = 0; i < n; i++) touch_move(AMotionEvent_getPointerId(e, i), AMotionEvent_getX(e, i), AMotionEvent_getY(e, i));
    if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP) touch_up(AMotionEvent_getPointerId(e, p));
    if (kind == AMOTION_EVENT_ACTION_CANCEL) {
        for (i = 0; i < MAX_TOUCHES; i++) {
            if (sTouch[i].role != ROLE_FREE) sTouch[i].gone = 1;
        }
    }
}

static int32_t sPadDevice = -1; /* the controller a button was pressed on last */

int input_android_event(AInputEvent* e) {
    int32_t source = AInputEvent_getSource(e);
    int used = 0;
    pthread_mutex_lock(&sLock);
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e);
        uint32_t bit = key_bit(code);
        if (code == AKEYCODE_BACK && !(source & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK))) bit = 0;
        if (bit) {
            if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) sPadDevice = AInputEvent_getDeviceId(e);
            if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) sPadButtons |= bit;
            else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) sPadButtons &= ~bit;
        }
        /* Back doesn't leave the game (the home gesture does). */
        used = bit != 0 || code == AKEYCODE_BACK;
    } else if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
        if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
            float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
            float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            float lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0);
            float rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
            /* The right stick is Z and RZ on some controllers, RX and RY on
             * others, where Z and RZ are the triggers (the AYN Thor's).
             * The NDK can't ask which, so it's told by what they do: a
             * trigger never goes below 0, and RX/RY moving is a stick. */
            static int zIsStick, rIsStick;
            float z = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0);
            float rz = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
            float rx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RX, 0);
            float ry = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RY, 0);
            if (z < -0.25f || rz < -0.25f) zIsStick = 1;
            if (!zIsStick && (fabsf(rx) > 0.25f || fabsf(ry) > 0.25f)) rIsStick = 1;
            sAxis[0] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0);
            sAxis[1] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
            if (rIsStick) {
                sAxis[2] = rx;
                sAxis[3] = ry;
                if (lt == 0) lt = z;
                if (rt == 0) rt = rz;
            } else {
                sAxis[2] = z;
                sAxis[3] = rz;
                if (sAxis[2] == 0 && sAxis[3] == 0) {
                    sAxis[2] = rx;
                    sAxis[3] = ry;
                }
            }
            if (lt == 0) lt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0);
            if (rt == 0) rt = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
            sPadHat = (hx < -0.5f ? XPAD_LEFT : 0) | (hx > 0.5f ? XPAD_RIGHT : 0) | (hy < -0.5f ? XPAD_UP : 0) |
                      (hy > 0.5f ? XPAD_DOWN : 0) | (lt > 0.5f ? XPAD_LT : 0) | (rt > 0.5f ? XPAD_RT : 0);
            used = 1;
        } else if (source & AINPUT_SOURCE_TOUCHSCREEN) {
            touch_event(e);
            used = 1;
        }
    }
    pthread_mutex_unlock(&sLock);
    return used;
}

/* ------------------------------------------------------------------------ */
/* Where the touch controls are: in units of a ninth of the screen's
 * height, from its corners. */

typedef struct {
    float x, y, r; /* x < 0: from the right; y < 0: from the bottom */
    uint32_t bit;
    const char* label;
} Control;

static const Control kStick = {2.4f, -2.4f, 1.5f, 0, NULL};
static const Control kButtons[] = {
    {-1.7f, -1.9f, 0.85f, HOST_PAD_2, "2"},        {-3.7f, -1.3f, 0.75f, HOST_PAD_1, "1"},
    {-1.5f, -4.0f, 0.75f, HOST_PAD_A, "A"},        {-3.5f, -3.3f, 0.65f, HOST_PAD_B, "B"},
    {-5.5f, -1.0f, 0.55f, HOST_PAD_SHAKE, "Shake"}, {-5.4f, -2.4f, 0.55f, HOST_PAD_SHAKE2, "Shake 2"},
    {0.9f, 0.8f, 0.45f, HOST_PAD_MINUS, "-"},      {2.1f, 0.8f, 0.45f, HOST_PAD_PLUS, "+"},
    {3.3f, 0.8f, 0.45f, HOST_PAD_HOME, "View"},
};
#define BUTTONS ((int)(sizeof(kButtons) / sizeof(kButtons[0])))

static void place(const Control* c, int w, int h, float* x, float* y, float* r) {
    float u = h / 9.0f;
    *x = c->x < 0 ? w + c->x * u : c->x * u;
    *y = c->y < 0 ? h + c->y * u : c->y * u;
    *r = c->r * u;
}

/* ------------------------------------------------------------------------ */
/* Reading, on the game's thread */

static int sControllerLast;
static uint32_t sTouchHeld;            /* buttons held by touches */
static float sStickX, sStickY;         /* the touch stick, -1 to 1, y up */
static float sCamX, sCamY, sPtrX, sPtrY; /* dragged since the last read, in mouse pixels */

/* A screen's drag as so many mouse pixels: about as the mouse turns the
 * camera across a window. */
static float drag_scale(int w) { return w > 0 ? 1600.0f / w : 1.0f; }

/* Gives each new touch its role, and works out what the touches hold. */
static void touches(int w, int h) {
    int i, b;
    float sx = 0, sy = 0;
    uint32_t held = 0;
    for (i = 0; i < MAX_TOUCHES; i++) {
        Touch* t = &sTouch[i];
        float cx, cy, r;
        if (t->role == ROLE_FREE) continue;
        if (t->role == ROLE_NEW) {
            int open = menu_is_open();
            t->role = ROLE_CAMERA;
            if (g_settings.touch_controls) sControllerLast = 0;
            if (open || !g_settings.touch_controls) {
                /* With the controls off, touches still work the Options
                 * tab and buttons (so they can be turned on again). */
                options_touch((int)t->x0, (int)t->y0);
                t->role = ROLE_USED;
            } else {
                /* The controls first: 2 sits over the save file screen's
                 * Options button, whose uncovered part still opens it. */
                place(&kStick, w, h, &cx, &cy, &r);
                if (hypotf(t->x0 - cx, t->y0 - cy) < r * 1.4f) t->role = ROLE_STICK;
                for (b = 0; b < BUTTONS; b++) {
                    place(&kButtons[b], w, h, &cx, &cy, &r);
                    if (hypotf(t->x0 - cx, t->y0 - cy) < r * 1.25f) {
                        t->role = ROLE_BUTTON;
                        t->bit = kButtons[b].bit;
                    }
                }
                if (t->role == ROLE_CAMERA && options_touch((int)t->x0, (int)t->y0)) t->role = ROLE_USED;
            }
        }
        if (t->role == ROLE_STICK && !t->gone) {
            float dx, dy, d;
            place(&kStick, w, h, &cx, &cy, &r);
            dx = (t->x - cx) / r;
            dy = (cy - t->y) / r;
            d = hypotf(dx, dy);
            if (d > 1) dx /= d, dy /= d, d = 1;
            sx = dx;
            sy = dy;
            if (d > 0.35f) {
                /* Eight directions, as the d-pad's. */
                if (dx > 0.38f * d) held |= HOST_PAD_SCREEN_RIGHT;
                if (dx < -0.38f * d) held |= HOST_PAD_SCREEN_LEFT;
                if (dy > 0.38f * d) held |= HOST_PAD_SCREEN_UP;
                if (dy < -0.38f * d) held |= HOST_PAD_SCREEN_DOWN;
            }
        } else if (t->role == ROLE_BUTTON && (!t->gone || now_ms() - t->down < TAP_MS)) {
            held |= t->bit;
            if (t->gone) continue; /* held on, freed once TAP_MS is up */
        } else if (t->role == ROLE_CAMERA) {
            float k = drag_scale(w);
            sCamX += (t->x - t->lastX) * k;
            sCamY += (t->y - t->lastY) * k;
            sPtrX += (t->x - t->lastX) * k;
            sPtrY += (t->y - t->lastY) * k;
        }
        t->lastX = t->x;
        t->lastY = t->y;
        if (t->gone) t->role = ROLE_FREE;
    }
    sTouchHeld = held;
    sStickX = sx;
    sStickY = sy;
}

static float stick_axis(float v) {
    float kDead = g_settings.stick_dead_zone / 100.0f;
    if (v > kDead) return (v - kDead) / (1 - kDead);
    if (v < -kDead) return (v + kDead) / (1 - kDead);
    return 0;
}

#define STICK_THRESHOLD 0.5f

/* The controller's buttons (XPAD_*), and the left stick's directions as
 * d-pad bits. Under the lock. */
static void controller_state(uint32_t* buttons, uint32_t* stick) {
    *buttons = settings_face_swap(sPadButtons) | sPadHat;
    *stick = (sAxis[1] < -STICK_THRESHOLD ? XPAD_UP : 0) | (sAxis[1] > STICK_THRESHOLD ? XPAD_DOWN : 0) |
             (sAxis[0] < -STICK_THRESHOLD ? XPAD_LEFT : 0) | (sAxis[0] > STICK_THRESHOLD ? XPAD_RIGHT : 0);
}

static const uint32_t kActionBit[BIND_COUNT] = {
    HOST_PAD_SCREEN_UP, HOST_PAD_SCREEN_DOWN, HOST_PAD_SCREEN_LEFT, HOST_PAD_SCREEN_RIGHT,
    HOST_PAD_2, HOST_PAD_1, HOST_PAD_A, HOST_PAD_B, HOST_PAD_PLUS, HOST_PAD_MINUS, HOST_PAD_HOME, HOST_PAD_SHAKE, HOST_PAD_SHAKE2,
    HOST_PAD_2, HOST_PAD_1, HOST_PAD_A, HOST_PAD_B, /* the menus' */
    HOST_PAD_2, HOST_PAD_1, HOST_PAD_A, HOST_PAD_B, /* the minigames' */
};
/* What the menus and minigames have bound apart; the park's don't count there. */
#define LAYER_OWN (HOST_PAD_2 | HOST_PAD_1 | HOST_PAD_A | HOST_PAD_B)

/* Action i, bound to something held, as each layer (LAYER_*) takes it. */
static void action_held(int i, uint32_t held[LAYERS]) {
    int l;
    if (i >= ACT_COUNT) held[ACT_LAYER(i)] |= kActionBit[i];
    else {
        for (l = 0; l < LAYERS; l++)
            if (l == LAYER_PARK || !(kActionBit[i] & LAYER_OWN)) held[l] |= kActionBit[i];
    }
}

/* The buttons held in each layer (LAYER_*), from the controller. */
static void controller(uint32_t b, uint32_t stick, uint32_t held[LAYERS]) {
    uint32_t dirs = 0;
    int i, k;
    for (i = 0; i < LAYERS; i++) held[i] = 0;
    for (i = 0; i < BIND_COUNT; i++) {
        for (k = 0; k < BIND_SLOTS; k++) {
            if (b & g_settings.pad[i][k]) {
                action_held(i, held);
                break;
            }
        }
    }
    if (stick & XPAD_UP) dirs |= HOST_PAD_SCREEN_UP;
    if (stick & XPAD_DOWN) dirs |= HOST_PAD_SCREEN_DOWN;
    if (stick & XPAD_LEFT) dirs |= HOST_PAD_SCREEN_LEFT;
    if (stick & XPAD_RIGHT) dirs |= HOST_PAD_SCREEN_RIGHT;
    for (i = 0; i < LAYERS; i++) held[i] |= dirs;
}

int input_controller_last(void) {
    static int forced = -1;
    if (forced < 0) {
        const char* e = getenv("RECOMP_PROMPTS");
        forced = e && !strcmp(e, "controller");
    }
    return forced || sControllerLast;
}

/* The buttons held at the last read, as the menus and the minigames have
 * them bound. */
static uint32_t sMenuHeld, sGameHeld;

static int read_menu(int chan, uint32_t* held) {
    *held = sMenuHeld;
    return chan == 0;
}

static int read_game(int chan, uint32_t* held) {
    *held = sGameHeld;
    return chan == 0;
}

static void sensors_poll(void);
static void rumble_tick(void);

static int read_pad(int chan, uint32_t* held) {
    static int menuHeld;
    uint32_t buttons, stick, pad[LAYERS], touch;
    int w, h;
    if (chan != 0) return 0;
    game_screen_size(&w, &h);
    pthread_mutex_lock(&sLock);
    controller_state(&buttons, &stick);
    touches(w, h);
    pthread_mutex_unlock(&sLock);
    options_controller(buttons, buttons | stick);
    sensors_poll();
    rumble_tick();
    controller(buttons, stick, pad);
    if (pad[0] | pad[1] | pad[2] || stick_axis(sAxis[2]) != 0 || stick_axis(sAxis[3]) != 0) sControllerLast = 1;
    /* The touch controls work the tab as a remote's buttons would: - on
     * the save file screen opens it. They're the remote's buttons, in the
     * menus too. */
    touch = options_touch_buttons(sTouchHeld) ? 0 : sTouchHeld;
    *held = pad[LAYER_PARK] | touch;
    sMenuHeld = pad[LAYER_MENUS] | touch;
    sGameHeld = pad[LAYER_GAMES] | touch;
    if (menu_is_open()) menuHeld = 1;
    else if (menuHeld && !*held && !sMenuHeld && !sGameHeld) menuHeld = 0;
    if (menuHeld) *held = sMenuHeld = sGameHeld = 0;
    return 1;
}

static void read_sticks(float* lx, float* ly, float* rx, float* ry) {
    *lx = *ly = *rx = *ry = 0;
    if (menu_is_open()) return;
    *lx = stick_axis(sAxis[0]);
    *ly = -stick_axis(sAxis[1]);
    *rx = stick_axis(sAxis[2]);
    *ry = -stick_axis(sAxis[3]);
    if (*lx == 0 && *ly == 0) {
        *lx = sStickX;
        *ly = sStickY;
    }
}

#define LOOK_STICK_PIXELS_PER_SECOND 600.0f

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void read_mouse(float* dx, float* dy) {
    static double last;
    double now = now_seconds();
    float lx, ly, rx, ry, dt = last ? (float)(now - last) : 0;
    if (dt > 0.1f) dt = 0.1f;
    last = now;
    read_sticks(&lx, &ly, &rx, &ry);
    *dx = (sCamX * g_settings.mouse_sensitivity + rx * LOOK_STICK_PIXELS_PER_SECOND * g_settings.stick_speed * dt) *
          (g_settings.invert_turn ? -1 : 1);
    *dy = (sCamY * g_settings.mouse_sensitivity - ry * LOOK_STICK_PIXELS_PER_SECOND * g_settings.stick_speed * dt) *
          (g_settings.invert_tilt ? -1 : 1);
    sCamX = sCamY = 0;
}

static void read_pointer(float* dx, float* dy) {
    *dx = sPtrX * g_settings.mouse_sensitivity;
    *dy = sPtrY * g_settings.mouse_sensitivity;
    sPtrX = sPtrY = 0;
}

/* Rumble: the vibrator of the controller pressed last (InputDevice's),
 * or else the handheld's own, through Java on the game thread. Each buzz
 * lasts RUMBLE_MS unless sent again, which it is every RESEND_MS while the
 * game keeps the motor on (at each pad read), so a lost "off" can't leave
 * it going. */
extern struct android_app* g_android_app; /* window_android.c */

#define RUMBLE_MS 1000
#define RESEND_MS 400
#define PREVIEW_MS 150

static jobject sGameAudio; /* AudioAttributes for a game's vibration (a global reference) */
static jobject sVibrator;        /* a global reference */
static int32_t sVibratorDevice = -2; /* the device it is for (-1: the handheld's) */
static int sRumbleOn;
static double sRumbleSent, sPreviewEnd;

static JNIEnv* jni(void) {
    JNIEnv* env;
    if (!g_android_app || (*g_android_app->activity->vm)->AttachCurrentThread(g_android_app->activity->vm, &env, NULL))
        return NULL;
    return env;
}

static int jni_failed(JNIEnv* env) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

/* The vibrator to use: the controller's if it has one, else the device's. */
static jobject vibrator(JNIEnv* env) {
    int32_t id = sPadDevice;
    jobject v = NULL;
    jclass vc = (*env)->FindClass(env, "android/os/Vibrator");
    if (id == sVibratorDevice) return sVibrator;
    if (sVibrator) (*env)->DeleteGlobalRef(env, sVibrator);
    sVibrator = NULL;
    sVibratorDevice = id;
    if (!vc) return jni_failed(env), NULL;
    if (id >= 0) {
        jclass ic = (*env)->FindClass(env, "android/view/InputDevice");
        jobject dev = ic ? (*env)->CallStaticObjectMethod(env, ic, (*env)->GetStaticMethodID(env, ic, "getDevice", "(I)Landroid/view/InputDevice;"), id) : NULL;
        if (dev) v = (*env)->CallObjectMethod(env, dev, (*env)->GetMethodID(env, ic, "getVibrator", "()Landroid/os/Vibrator;"));
        if (jni_failed(env)) v = NULL;
        if (v && !(*env)->CallBooleanMethod(env, v, (*env)->GetMethodID(env, vc, "hasVibrator", "()Z"))) v = NULL;
    }
    if (!v) {
        jobject act = g_android_app->activity->clazz;
        jclass cc = (*env)->GetObjectClass(env, act);
        jstring name = (*env)->NewStringUTF(env, "vibrator");
        v = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, cc, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"), name);
        if (jni_failed(env)) v = NULL;
        if (v && !(*env)->CallBooleanMethod(env, v, (*env)->GetMethodID(env, vc, "hasVibrator", "()Z"))) v = NULL;
    }
    if (v) sVibrator = (*env)->NewGlobalRef(env, v);
    jni_failed(env);
    return sVibrator;
}

/* A buzz of ms at the rumble setting's strength, or 0: stopped. */
static void vibrate(int ms) {
    static const int kAmplitude[4] = {0, 80, 160, 255};
    JNIEnv* env = jni();
    jobject v;
    jclass vc, ec;
    if (!env || !(v = vibrator(env))) return;
    vc = (*env)->FindClass(env, "android/os/Vibrator");
    if (ms <= 0 || !g_settings.rumble) {
        (*env)->CallVoidMethod(env, v, (*env)->GetMethodID(env, vc, "cancel", "()V"));
    } else {
        int amp = kAmplitude[g_settings.rumble & 3];
        jobject effect;
        ec = (*env)->FindClass(env, "android/os/VibrationEffect");
        if (!(*env)->CallBooleanMethod(env, v, (*env)->GetMethodID(env, vc, "hasAmplitudeControl", "()Z")))
            amp = -1; /* DEFAULT_AMPLITUDE */
        effect = (*env)->CallStaticObjectMethod(env, ec, (*env)->GetStaticMethodID(env, ec, "createOneShot", "(JI)Landroid/os/VibrationEffect;"),
                                                (jlong)ms, (jint)amp);
        if (effect && !sGameAudio) {
            /* As a game's (AudioAttributes.USAGE_GAME), not a touch's:
             * turning touch feedback off doesn't stop it. */
            jclass bc = (*env)->FindClass(env, "android/media/AudioAttributes$Builder");
            jobject b = bc ? (*env)->NewObject(env, bc, (*env)->GetMethodID(env, bc, "<init>", "()V")) : NULL;
            if (b) {
                (*env)->CallObjectMethod(env, b, (*env)->GetMethodID(env, bc, "setUsage", "(I)Landroid/media/AudioAttributes$Builder;"), 14);
                b = (*env)->CallObjectMethod(env, b, (*env)->GetMethodID(env, bc, "build", "()Landroid/media/AudioAttributes;"));
                if (b && !jni_failed(env)) sGameAudio = (*env)->NewGlobalRef(env, b);
            }
            jni_failed(env);
        }
        if (effect && sGameAudio)
            (*env)->CallVoidMethod(env, v,
                                   (*env)->GetMethodID(env, vc, "vibrate", "(Landroid/os/VibrationEffect;Landroid/media/AudioAttributes;)V"),
                                   effect, sGameAudio);
        else if (effect)
            (*env)->CallVoidMethod(env, v, (*env)->GetMethodID(env, vc, "vibrate", "(Landroid/os/VibrationEffect;)V"), effect);
    }
    jni_failed(env);
}

static void rumble(int chan, int on) {
    if (chan != 0 || on == sRumbleOn) return;
    sRumbleOn = on;
    sRumbleSent = now_seconds();
    vibrate(on ? RUMBLE_MS : 0);
}

/* At each pad read: the buzz kept going while it should be, and the
 * preview's ended. */
static void rumble_tick(void) {
    double now = now_seconds();
    if (sPreviewEnd && now >= sPreviewEnd) {
        sPreviewEnd = 0;
        if (!sRumbleOn) vibrate(0);
    }
    if (sRumbleOn && now - sRumbleSent >= RESEND_MS / 1000.0) {
        sRumbleSent = now;
        vibrate(RUMBLE_MS);
    }
}

/* A short buzz at the new strength, when the setting is changed. */
void input_rumble_preview(void) {
    if (sRumbleOn) return;
    vibrate(PREVIEW_MS);
    sPreviewEnd = now_seconds() + PREVIEW_MS / 1000.0;
}

/* ------------------------------------------------------------------------ */
/* The handheld's own motion sensors, for the minigames (motion.h): on
 * while they want them, read on the game thread at each pad read. */

extern struct android_app* g_android_app; /* window_android.c */

static ASensorManager* sSensorManager;
static const ASensor *sAccel, *sGyro;
static ASensorEventQueue* sSensorQueue;
static int sSensorsOn, sRotation;

static void sensors_find(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    sSensorManager = ASensorManager_getInstanceForPackage("io.github.pokeparkrecomp");
    if (!sSensorManager) return;
    sAccel = ASensorManager_getDefaultSensor(sSensorManager, ASENSOR_TYPE_ACCELEROMETER);
    sGyro = ASensorManager_getDefaultSensor(sSensorManager, ASENSOR_TYPE_GYROSCOPE);
}

/* A device's USB vendor (InputDevice.getVendorId), or 0. */
static int device_vendor(int32_t id) {
    JNIEnv* env;
    jclass cls;
    jobject dev;
    int v = 0;
    if (!g_android_app || (*g_android_app->activity->vm)->AttachCurrentThread(g_android_app->activity->vm, &env, NULL))
        return 0;
    cls = (*env)->FindClass(env, "android/view/InputDevice");
    if (cls) {
        dev = (*env)->CallStaticObjectMethod(env, cls, (*env)->GetStaticMethodID(env, cls, "getDevice", "(I)Landroid/view/InputDevice;"), id);
        if (dev) v = (*env)->CallIntMethod(env, dev, (*env)->GetMethodID(env, cls, "getVendorId", "()I"));
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        v = 0;
    }
    return v;
}

int input_pad_family(void) {
    static int32_t known = -2;
    static int family = PAD_ICONS_XBOX;
    int32_t id = sPadDevice;
    if (id != known) {
        int v = id >= 0 ? device_vendor(id) : 0;
        family = v == 0x054C ? PAD_ICONS_PLAYSTATION : v == 0x057E ? PAD_ICONS_NINTENDO : PAD_ICONS_XBOX;
        known = id;
    }
    return family;
}

int input_has_gyro(void) {
    sensors_find();
    return sAccel && sGyro;
}

/* The screen's turn from the device's own way up (Surface.ROTATION_*):
 * the sensors' axes are the device's. */
static int display_rotation(void) {
    JNIEnv* env;
    jobject act;
    jclass cls;
    jobject wm, display;
    int r = 0;
    if (!g_android_app || (*g_android_app->activity->vm)->AttachCurrentThread(g_android_app->activity->vm, &env, NULL))
        return 0;
    act = g_android_app->activity->clazz;
    cls = (*env)->GetObjectClass(env, act);
    wm = (*env)->CallObjectMethod(env, act, (*env)->GetMethodID(env, cls, "getWindowManager", "()Landroid/view/WindowManager;"));
    if (wm) {
        jclass wmc = (*env)->GetObjectClass(env, wm);
        display = (*env)->CallObjectMethod(env, wm, (*env)->GetMethodID(env, wmc, "getDefaultDisplay", "()Landroid/view/Display;"));
        if (display) {
            jclass dc = (*env)->GetObjectClass(env, display);
            r = (*env)->CallIntMethod(env, display, (*env)->GetMethodID(env, dc, "getRotation", "()I"));
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        r = 0;
    }
    return r & 3;
}

/* A sensor's reading, in the device's axes, in the controller's (motion.h):
 * x across the screen to the right, y out of it, z down it. */
static void device_to_pad(const float d[3], float out[3]) {
    float sx, sy;
    switch (sRotation) {
    case 1: sx = -d[1]; sy = d[0]; break;
    case 2: sx = -d[0]; sy = -d[1]; break;
    case 3: sx = d[1]; sy = -d[0]; break;
    default: sx = d[0]; sy = d[1];
    }
    out[0] = sx;
    out[1] = d[2];
    out[2] = -sy;
}

static void sensors_poll(void) {
    static int64_t lastAccel, lastGyro;
    double now = now_seconds();
    int want = motion_wanted(now) && input_has_gyro();
    ASensorEvent ev[16];
    ssize_t n, i;
    if (want && !sSensorQueue) {
        ALooper* looper = ALooper_forThread();
        if (!looper) looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        sSensorQueue = ASensorManager_createEventQueue(sSensorManager, looper, 0x5E45, NULL, NULL);
        if (!sSensorQueue) return;
    }
    if (want != sSensorsOn && sSensorQueue) {
        if (want) {
            sRotation = display_rotation();
            ASensorEventQueue_registerSensor(sSensorQueue, sAccel, 10000, 0);
            ASensorEventQueue_registerSensor(sSensorQueue, sGyro, 5000, 0);
            lastAccel = lastGyro = 0;
        } else {
            ASensorEventQueue_disableSensor(sSensorQueue, sAccel);
            ASensorEventQueue_disableSensor(sSensorQueue, sGyro);
            motion_off();
        }
        sSensorsOn = want;
    }
    if (!sSensorsOn) return;
    while ((n = ASensorEventQueue_getEvents(sSensorQueue, ev, 16)) > 0) {
        for (i = 0; i < n; i++) {
            float v[3];
            device_to_pad(ev[i].data, v);
            if (ev[i].type == ASENSOR_TYPE_ACCELEROMETER) {
                motion_accel(v, lastAccel ? (ev[i].timestamp - lastAccel) * 1e-9f : 0, now);
                lastAccel = ev[i].timestamp;
            } else if (ev[i].type == ASENSOR_TYPE_GYROSCOPE) {
                motion_gyro(v, lastGyro ? (ev[i].timestamp - lastGyro) * 1e-9f : 0, now);
                lastGyro = ev[i].timestamp;
            }
        }
    }
}

static int read_gyro(float* yaw, float* pitch) { return motion_gyro_read(yaw, pitch, now_seconds()); }
static int read_tilt(float* roll, float* pitch) { return motion_tilt_read(roll, pitch, now_seconds()); }

static int script_input(uint32_t held) { return options_script(held) || menu_is_open(); }

void input_init(void) {
    g_host_pad_read = read_pad;
    g_host_menu_read = read_menu;
    g_host_game_read = read_game;
    g_host_script_input = script_input;
    g_host_pad_rumble = rumble;
    g_host_mouse_read = read_mouse;
    prompts_init();
    g_host_pointer_read = read_pointer;
    g_host_sticks_read = read_sticks;
    g_host_gyro_read = read_gyro;
    g_host_tilt_read = read_tilt;
}

/* ------------------------------------------------------------------------ */
/* The menus and the touch controls, drawn over the picture */

int menu_is_open(void) { return options_is_open(); }
void menu_controller(uint32_t raw, uint32_t nav) { options_controller(raw, nav); }

static uint32_t* sPx;
static int sW, sH;
static unsigned sGeneration;

unsigned menu_generation(void) { return sGeneration; }

/* Lays color c (0xAARRGGBB) over pixel d with coverage a (0-1). */
static void blend(uint32_t* d, uint32_t c, float a) {
    float sa = (c >> 24) / 255.0f * a, da = (*d >> 24) / 255.0f, oa = sa + da * (1 - sa);
    uint32_t out;
    int k;
    if (sa <= 0) return;
    out = (uint32_t)(oa * 255 + 0.5f) << 24;
    for (k = 0; k < 24; k += 8) {
        float sc = (float)(c >> k & 255), dc = (float)(*d >> k & 255);
        out |= (uint32_t)((sc * sa + dc * da * (1 - sa)) / oa + 0.5f) << k;
    }
    *d = out;
}

/* A disc, its edge smoothed, or a ring of width `ring` if that's > 0. */
static void disc(float cx, float cy, float r, float ring, uint32_t c) {
    int x, y, x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    for (y = y0 < 0 ? 0 : y0; y <= y1 && y < sH; y++) {
        for (x = x0 < 0 ? 0 : x0; x <= x1 && x < sW; x++) {
            float d = hypotf(x + 0.5f - cx, y + 0.5f - cy), a = r - d + 0.5f;
            if (ring > 0 && d < r - ring) a = d - (r - ring) + 0.5f;
            if (a <= 0) continue;
            blend(&sPx[(size_t)y * sW + x], c, a > 1 ? 1 : a);
        }
    }
}

/* Text in the game's font, centred on (cx, cy), size pixels tall. */
static void text(const char* s, float cx, float cy, float size, uint32_t c) {
    static uint8_t cov[64 * 64];
    int cw, ch, base, left, advance, total = 0, x, y;
    const char* p;
    float k, pen;
    if (!hle_font_metrics(&cw, &ch, &base) || cw * ch > (int)sizeof(cov)) return;
    k = size / ch;
    for (p = s; *p; p++) {
        if (hle_font_glyph((uint8_t)*p, cov, &left, &advance)) total += advance;
    }
    pen = cx - total * k / 2;
    for (p = s; *p; p++) {
        int bx, by, gw = (int)(cw * k), gh = (int)(ch * k);
        if (!hle_font_glyph((uint8_t)*p, cov, &left, &advance)) continue;
        bx = (int)(pen + left * k);
        by = (int)(cy - gh / 2.0f);
        for (y = 0; y < gh; y++) {
            for (x = 0; x < gw; x++) {
                int u = (int)(x / k), v = (int)(y / k), px = bx + x, py = by + y;
                if (u >= cw || v >= ch || px < 0 || py < 0 || px >= sW || py >= sH || !cov[v * cw + u]) continue;
                blend(&sPx[(size_t)py * sW + px], c, cov[v * cw + u] / 255.0f);
            }
        }
        pen += advance * k;
    }
}

static void draw_controls(void) {
    float x, y, r;
    int b;
    place(&kStick, sW, sH, &x, &y, &r);
    disc(x, y, r, 0, 0x40000000u);
    disc(x, y, r, r * 0.06f, 0x90FFFFFFu);
    disc(x + sStickX * r * 0.6f, y - sStickY * r * 0.6f, r * 0.42f, 0, 0xA0FFFFFFu);
    for (b = 0; b < BUTTONS; b++) {
        int held = (sTouchHeld & kButtons[b].bit) != 0;
        size_t len = strlen(kButtons[b].label);
        place(&kButtons[b], sW, sH, &x, &y, &r);
        disc(x, y, r, 0, held ? 0x90FFFFFFu : 0x50000000u);
        disc(x, y, r, r * 0.08f, 0xA0FFFFFFu);
        text(kButtons[b].label, x, y, len > 2 ? r * 0.55f : r * 1.1f, held ? 0xFF303030u : 0xFFFFFFFFu);
    }
}

const uint32_t* menu_draw(int w, int h) {
    static unsigned last;
    const uint32_t* opt = options_draw(w, h);
    int show = g_settings.touch_controls && !menu_is_open() && !input_controller_last();
    unsigned key = 2166136261u;
    /* What's on it: redrawn and uploaded again only when that changes. */
    unsigned parts[8] = {(unsigned)show, opt ? options_draws() : ~0u, sTouchHeld, (unsigned)(sStickX * 8),
                         (unsigned)(sStickY * 8), (unsigned)w, (unsigned)h, (unsigned)hle_font_metrics(&(int){0}, &(int){0}, &(int){0})};
    int i;
    for (i = 0; i < 8; i++) key = (key ^ parts[i]) * 16777619u;
    if (!show) {
        if (key != last) sGeneration++;
        last = key;
        return opt;
    }
    if (key != last || !sPx) {
        if (w != sW || h != sH || !sPx) {
            free(sPx);
            sPx = (uint32_t*)malloc((size_t)w * h * 4);
            if (!sPx) return opt;
            sW = w;
            sH = h;
        }
        if (opt) memcpy(sPx, opt, (size_t)w * h * 4);
        else memset(sPx, 0, (size_t)w * h * 4);
        draw_controls();
        sGeneration++;
        last = key;
    }
    return sPx;
}
