/* Wii Remote 0 from the keyboard and the controllers, for hle_wpad.c. PokePark is played with the remote held sideways, like a
 * classic pad: the d-pad moves, 1 and 2 are the face buttons. By default:
 *
 *                    keyboard / mouse           controller
 *   d-pad (move)     WASD / arrows              d-pad / left stick
 *   2 (jump)         Space / X                  A
 *   1 (dash)         left click / Z             X
 *   A (Thunderbolt)  right click / Enter        B
 *   B                V / Left Ctrl              Y / right trigger
 *   +                P                          Start
 *   -                M                          Back
 *   first person     Home / H                   right stick click
 *   camera           mouse                      right stick
 *   shake            Q / Left Shift             LB   (either shake: Iron Tail)
 *   shake 2          E                          RB
 *   (in minigames a shake is shake and shake 2 pressed by turns, hle_wpad.c)
 *
 * The keyboard counts only while the game's window has focus. While it
 * has focus the mouse is captured: hidden, held in the window, and its
 * movement turns the camera (hle_camera.c). Esc lets it go; a click in
 * the window captures it again. Mouse buttons count only while captured,
 * and not the click that captures it. The settings menu (menu_win32.c)
 * changes the bindings; while it is open the game gets no input. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_input.h"
#include "host_settings.h"
#include "motion.h"

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XPadGamepad;
typedef struct {
    DWORD dwPacketNumber;
    XPadGamepad Gamepad;
} XPadState;
typedef struct {
    WORD wLeftMotorSpeed, wRightMotorSpeed;
} XPadVibration;

static DWORD(WINAPI* pXInputGetState)(DWORD, XPadState*);
static DWORD(WINAPI* pXInputSetState)(DWORD, XPadVibration*);

/* Controllers through SDL3 (SDL3.dll beside the program, loaded at start;
 * tools/recomp/build.py puts it there): PlayStation, Switch, 8BitDo and
 * most others, over USB or Bluetooth, and Xbox ones. Without it, the
 * first XInput controller. With it, every controller connected counts:
 * their buttons together, and each stick and trigger the one pushed
 * furthest. Read as an XInput state, which the bindings are in, SDL's
 * face buttons by where they are (the bottom one is A, as on an Xbox
 * controller: a DualSense's cross). */
#ifdef RECOMP_SDL3
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#define SDL_FNS(X)                                                    \
    X(bool, SDL_SetHint, (const char*, const char*))                  \
    X(bool, SDL_Init, (SDL_InitFlags))                                \
    X(const char*, SDL_GetError, (void))                              \
    X(void, SDL_SetGamepadEventsEnabled, (bool))                      \
    X(void, SDL_SetJoystickEventsEnabled, (bool))                     \
    X(void, SDL_UpdateGamepads, (void))                               \
    X(SDL_JoystickID*, SDL_GetGamepads, (int*))                       \
    X(void, SDL_free, (void*))                                        \
    X(SDL_Gamepad*, SDL_OpenGamepad, (SDL_JoystickID))                \
    X(void, SDL_CloseGamepad, (SDL_Gamepad*))                         \
    X(bool, SDL_GamepadConnected, (SDL_Gamepad*))                     \
    X(SDL_JoystickID, SDL_GetGamepadID, (SDL_Gamepad*))               \
    X(const char*, SDL_GetGamepadName, (SDL_Gamepad*))                \
    X(SDL_GamepadType, SDL_GetGamepadType, (SDL_Gamepad*))            \
    X(bool, SDL_GetGamepadButton, (SDL_Gamepad*, SDL_GamepadButton))  \
    X(Sint16, SDL_GetGamepadAxis, (SDL_Gamepad*, SDL_GamepadAxis))    \
    X(bool, SDL_RumbleGamepad, (SDL_Gamepad*, Uint16, Uint16, Uint32))     \
    X(bool, SDL_GamepadHasSensor, (SDL_Gamepad*, SDL_SensorType))            \
    X(bool, SDL_SetGamepadSensorEnabled, (SDL_Gamepad*, SDL_SensorType, bool)) \
    X(bool, SDL_GetGamepadSensorData, (SDL_Gamepad*, SDL_SensorType, float*, int))
#define SDL_PTR(ret, name, args) static ret(SDLCALL* p##name) args;
SDL_FNS(SDL_PTR)

#define MAX_PADS 8
#define SCAN_MS 500    /* how often controllers coming and going are looked for */
#define RUMBLE_MS 1000 /* each rumble lasts this long unless sent again, */
#define RESEND_MS 400  /* which it is this often while it should go on */
static int sSdl;
static CRITICAL_SECTION sPadLock;
static SDL_Gamepad* sPads[MAX_PADS];
static ULONGLONG sNextScan, sMotorSent;
static WORD sMotor; /* the speed the motors should run at */
static int sSensorsOn[MAX_PADS]; /* the controller's gyroscope and accelerometer on */
static int sFamily = PAD_ICONS_XBOX; /* the kind of controller pressed last */

static int sdl_family(SDL_Gamepad* g) {
    switch (pSDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return PAD_ICONS_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PAD_ICONS_NINTENDO;
    default: return PAD_ICONS_XBOX;
    }
}

static const struct {
    SDL_GamepadButton b;
    WORD bit;
} kSdlButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, XPAD_A},           {SDL_GAMEPAD_BUTTON_EAST, XPAD_B},
    {SDL_GAMEPAD_BUTTON_WEST, XPAD_X},            {SDL_GAMEPAD_BUTTON_NORTH, XPAD_Y},
    {SDL_GAMEPAD_BUTTON_BACK, XPAD_BACK},         {SDL_GAMEPAD_BUTTON_START, XPAD_START},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, XPAD_LTHUMB}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XPAD_RTHUMB},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XPAD_LB},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XPAD_RB},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, XPAD_UP},        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XPAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XPAD_LEFT},    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XPAD_RIGHT},
};

static void sdl_init(void) {
    wchar_t path[MAX_PATH];
    wchar_t* slash;
    HMODULE m;
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH - 16 || !(slash = wcsrchr(path, L'\\'))) return;
    wcscpy(slash + 1, L"SDL3.dll");
    /* Its own folder searched for what it needs (vcruntime140.dll). */
    m = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) return;
#define SDL_GET(ret, name, args)                                            \
    if (!(p##name = (ret(SDLCALL*) args)(void*)GetProcAddress(m, #name))) { \
        fprintf(stderr, "input: SDL3.dll has no %s\n", #name);              \
        return;                                                             \
    }
    SDL_FNS(SDL_GET)
    /* Read whether the window has focus or not, as XInput is, and kept up
     * on a thread of SDL's own: nothing here handles its events. */
    pSDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    pSDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (!pSDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "input: SDL3 couldn't start: %s\n", pSDL_GetError());
        return;
    }
    pSDL_SetGamepadEventsEnabled(false);
    pSDL_SetJoystickEventsEnabled(false);
    InitializeCriticalSection(&sPadLock);
    sSdl = 1;
}

/* Controllers let go of when gone, and opened when new (sPadLock held). */
static void sdl_scan(void) {
    int i, j, n = 0;
    SDL_JoystickID* ids;
    for (i = 0; i < MAX_PADS; i++) {
        if (sPads[i] && !pSDL_GamepadConnected(sPads[i])) {
            pSDL_CloseGamepad(sPads[i]);
            sPads[i] = NULL;
            sSensorsOn[i] = 0;
        }
    }
    ids = pSDL_GetGamepads(&n);
    if (!ids) return;
    for (j = 0; j < n; j++) {
        int have = 0, free_ = -1;
        for (i = 0; i < MAX_PADS; i++) {
            if (sPads[i] && pSDL_GetGamepadID(sPads[i]) == ids[j]) have = 1;
            else if (!sPads[i] && free_ < 0) free_ = i;
        }
        if (have || free_ < 0) continue;
        if ((sPads[free_] = pSDL_OpenGamepad(ids[j]))) {
            const char* name = pSDL_GetGamepadName(sPads[free_]);
            fprintf(stderr, "input: controller: %s\n", name ? name : "(no name)");
            if (sMotor) pSDL_RumbleGamepad(sPads[free_], sMotor, sMotor, RUMBLE_MS);
        }
    }
    pSDL_free(ids);
}

/* The stick pushed further of this and the one so far, as XInput has it
 * (up positive). */
static void sdl_stick(SDL_Gamepad* g, SDL_GamepadAxis ax, SDL_GamepadAxis ay, SHORT* x, SHORT* y) {
    int sx = pSDL_GetGamepadAxis(g, ax), sy = -(int)pSDL_GetGamepadAxis(g, ay);
    if (sy > 32767) sy = 32767;
    if (sx * sx + sy * sy > (int)*x * *x + (int)*y * *y) {
        *x = (SHORT)sx;
        *y = (SHORT)sy;
    }
}

static double seconds_now(void);

/* The first controller with a gyroscope and an accelerometer: its sensors
 * on while the minigames want them, and read (sPadLock held). */
static void sdl_motion(void) {
    static double last;
    double now = seconds_now();
    float dt = last ? (float)(now - last) : 0, v[3];
    int i, want = motion_wanted(now), used = 0;
    last = now;
    for (i = 0; i < MAX_PADS; i++) {
        SDL_Gamepad* g = sPads[i];
        int on;
        if (!g) continue;
        on = want && !used && pSDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && pSDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL);
        if (on != sSensorsOn[i]) {
            pSDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, on);
            pSDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, on);
            sSensorsOn[i] = on;
            if (!on) motion_off();
        }
        if (!on) continue;
        used = 1;
        if (pSDL_GetGamepadSensorData(g, SDL_SENSOR_ACCEL, v, 3)) motion_accel(v, dt, now);
        if (pSDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, v, 3)) motion_gyro(v, dt, now);
    }
}

static int sdl_has_gyro(void) {
    int i, has = 0;
    if (!sSdl) return 0;
    EnterCriticalSection(&sPadLock);
    for (i = 0; i < MAX_PADS; i++)
        if (sPads[i] && pSDL_GamepadHasSensor(sPads[i], SDL_SENSOR_GYRO)) has = 1;
    LeaveCriticalSection(&sPadLock);
    return has;
}

static int sdl_state(XPadState* s) {
    ULONGLONG now = GetTickCount64();
    int i, k, any = 0, resend;
    WORD pressedBefore = 0; /* (a press on a controller says it was used) */
    EnterCriticalSection(&sPadLock);
    pSDL_UpdateGamepads();
    if (now >= sNextScan) {
        sdl_scan();
        sNextScan = now + SCAN_MS;
    }
    sdl_motion();
    resend = sMotor && now - sMotorSent >= RESEND_MS;
    if (resend) sMotorSent = now;
    for (i = 0; i < MAX_PADS; i++) {
        SDL_Gamepad* g = sPads[i];
        int lt, rt;
        if (!g) continue;
        any = 1;
        for (k = 0; k < (int)(sizeof(kSdlButtons) / sizeof(kSdlButtons[0])); k++) {
            if (pSDL_GetGamepadButton(g, kSdlButtons[k].b)) s->Gamepad.wButtons |= kSdlButtons[k].bit;
        }
        lt = pSDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7;
        rt = pSDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7;
        if (lt > s->Gamepad.bLeftTrigger) s->Gamepad.bLeftTrigger = (BYTE)lt;
        if (rt > s->Gamepad.bRightTrigger) s->Gamepad.bRightTrigger = (BYTE)rt;
        sdl_stick(g, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, &s->Gamepad.sThumbLX, &s->Gamepad.sThumbLY);
        sdl_stick(g, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, &s->Gamepad.sThumbRX, &s->Gamepad.sThumbRY);
        if (resend) pSDL_RumbleGamepad(g, sMotor, sMotor, RUMBLE_MS);
        if (s->Gamepad.wButtons != pressedBefore) sFamily = sdl_family(g);
        pressedBefore = s->Gamepad.wButtons;
    }
    LeaveCriticalSection(&sPadLock);
    return any;
}

static void sdl_motors(WORD speed) {
    int i;
    EnterCriticalSection(&sPadLock);
    sMotor = speed;
    sMotorSent = GetTickCount64();
    for (i = 0; i < MAX_PADS; i++) {
        if (sPads[i]) pSDL_RumbleGamepad(sPads[i], speed, speed, speed ? RUMBLE_MS : 0);
    }
    LeaveCriticalSection(&sPadLock);
}
#else
#define sSdl 0
static int sdl_state(XPadState* s) { (void)s; return 0; }
static int sdl_has_gyro(void) { return 0; }
#define sFamily PAD_ICONS_XBOX
static void sdl_motors(WORD speed) { (void)speed; }
#endif

/* The controllers' state, as XInput's; 0 if there are none. */
static int pad_state(XPadState* s) {
    memset(s, 0, sizeof(*s));
    if (sSdl) return sdl_state(s);
    return pXInputGetState && pXInputGetState(0, s) == ERROR_SUCCESS;
}

static int has_focus(void) {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int sCaptured, sReleased, sSwallow, sButtonsLive;
static POINT sCenter;
static float sMouseX, sMouseY, sPointerX, sPointerY;

/* Keeps the cursor in the window while captured, checked on every poll:
 * going fullscreen moves the window. */
static void capture(int on) {
    if (on) {
        RECT r;
        POINT c;
        HWND w = GetForegroundWindow();
        GetClientRect(w, &r);
        MapWindowPoints(w, NULL, (POINT*)&r, 2);
        c.x = (r.left + r.right) / 2;
        c.y = (r.top + r.bottom) / 2;
        if (!sCaptured || c.x != sCenter.x || c.y != sCenter.y) {
            ClipCursor(&r);
            sCenter = c;
            SetCursorPos(sCenter.x, sCenter.y);
        }
        if (!sCaptured) while (ShowCursor(FALSE) >= 0) {}
    } else if (sCaptured) {
        ClipCursor(NULL);
        while (ShowCursor(TRUE) < 0) {}
    }
    sCaptured = on;
}

/* Captures or releases the mouse and collects its movement; works out
 * whether its buttons count (sButtonsLive). */
static void mouse(void) {
    int left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    POINT p;
    sButtonsLive = 0;
    if (!has_focus() || menu_is_open()) {
        capture(0);
        return;
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) sReleased = 1;
    if (sReleased && (left || right)) {
        HWND w = GetForegroundWindow();
        RECT r;
        GetCursorPos(&p);
        ScreenToClient(w, &p);
        GetClientRect(w, &r);
        if (PtInRect(&r, p)) sReleased = 0, sSwallow = 1;
    }
    capture(!sReleased);
    if (!sCaptured) return;
    GetCursorPos(&p);
    sMouseX += (float)(p.x - sCenter.x);
    sPointerX += (float)(p.x - sCenter.x);
    sPointerY += (float)(p.y - sCenter.y);
    sMouseY += (float)(p.y - sCenter.y);
    if (p.x != sCenter.x || p.y != sCenter.y) SetCursorPos(sCenter.x, sCenter.y);
    if (sSwallow) {
        if (left || right) return;
        sSwallow = 0;
    }
    sButtonsLive = 1;
}

static void read_pointer(float* dx, float* dy) {
    *dx = sPointerX * g_settings.mouse_sensitivity;
    *dy = sPointerY * g_settings.mouse_sensitivity;
    sPointerX = sPointerY = 0;
}

/* A stick axis, -32768 to 32767, as -1 to 1 past the dead zone. */
static float stick_axis(SHORT v) {
    float kDead = g_settings.stick_dead_zone * 32767.0f / 100.0f;
    float f = (float)v;
    if (f > kDead) return (f - kDead) / (32767.0f - kDead);
    if (f < -kDead) return (f + kDead) / (32768.0f - kDead);
    return 0.0f;
}

static void read_sticks(float* lx, float* ly, float* rx, float* ry) {
    XPadState s;
    *lx = *ly = *rx = *ry = 0;
    if (!pad_state(&s) || menu_is_open()) return;
    *lx = stick_axis(s.Gamepad.sThumbLX);
    *ly = stick_axis(s.Gamepad.sThumbLY);
    *rx = stick_axis(s.Gamepad.sThumbRX);
    *ry = stick_axis(s.Gamepad.sThumbRY);
}

/* The right stick turns the camera as the mouse does: full over turns it
 * as this many pixels a second would (180 degrees a second), up looking
 * up. */
#define LOOK_STICK_PIXELS_PER_SECOND 600.0f

/* The camera's turn since the last call (hle_camera.c), in mouse pixels:
 * the mouse's movement and the right stick's push over the time. */
static void read_mouse(float* dx, float* dy) {
    static LARGE_INTEGER last;
    LARGE_INTEGER now, freq;
    float lx, ly, rx, ry, dt = 0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (last.QuadPart) dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
    if (dt > 0.1f) dt = 0.1f; /* not a jump after a stall */
    last = now;
    read_sticks(&lx, &ly, &rx, &ry);
    if (!has_focus()) rx = ry = 0;
    *dx = (sMouseX * g_settings.mouse_sensitivity + rx * LOOK_STICK_PIXELS_PER_SECOND * g_settings.stick_speed * dt) *
          (g_settings.invert_turn ? -1 : 1);
    *dy = (sMouseY * g_settings.mouse_sensitivity - ry * LOOK_STICK_PIXELS_PER_SECOND * g_settings.stick_speed * dt) *
          (g_settings.invert_tilt ? -1 : 1);
    sMouseX = sMouseY = 0;
}

static double seconds_now(void) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

/* The minigames' gyro aiming and steering (motion.h), from what
 * sdl_motion read. */
static int read_gyro(float* yaw, float* pitch) {
    int r;
    if (!sSdl) return *yaw = *pitch = 0, 0;
#ifdef RECOMP_SDL3
    EnterCriticalSection(&sPadLock);
#endif
    r = motion_gyro_read(yaw, pitch, seconds_now());
#ifdef RECOMP_SDL3
    LeaveCriticalSection(&sPadLock);
#endif
    return r;
}

static int read_tilt(float* roll, float* pitch) {
    int r;
    if (!sSdl) return *roll = *pitch = 0, 0;
#ifdef RECOMP_SDL3
    EnterCriticalSection(&sPadLock);
#endif
    r = motion_tilt_read(roll, pitch, seconds_now());
#ifdef RECOMP_SDL3
    LeaveCriticalSection(&sPadLock);
#endif
    return r;
}

int input_has_gyro(void) { return sdl_has_gyro(); }
int input_pad_family(void) { return sSdl ? sFamily : PAD_ICONS_XBOX; }

static int is_mouse_button(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

/* The pad bit of each action (ACT_*). */
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

/* The buttons held in each layer (LAYER_*), from the keyboard and mouse. */
static void keyboard(uint32_t held[LAYERS]) {
    int i, k;
    for (i = 0; i < LAYERS; i++) held[i] = 0;
    if (!has_focus()) return;
    for (i = 0; i < BIND_COUNT; i++) {
        for (k = 0; k < BIND_SLOTS; k++) {
            int vk = g_settings.key[i][k];
            if (vk && (!is_mouse_button(vk) || sButtonsLive) && (GetAsyncKeyState(vk) & 0x8000)) {
                action_held(i, held);
                break;
            }
        }
    }
}

/* Half the stick's travel, then the d-pad direction. */
#define STICK_THRESHOLD 16384

/* The controller's buttons (XPAD_*), and separately the directions the
 * left stick is pushed, as d-pad bits. Read once per poll: asking about
 * a controller that isn't plugged in takes milliseconds, and the menu
 * gets the same reading. */
static void controller_state(uint32_t* buttons, uint32_t* stick) {
    XPadState s;
    *buttons = *stick = 0;
    if (!pad_state(&s)) return;
    *buttons = settings_face_swap(s.Gamepad.wButtons);
    if (s.Gamepad.bLeftTrigger > 128) *buttons |= XPAD_LT;
    if (s.Gamepad.bRightTrigger > 128) *buttons |= XPAD_RT;
    if (s.Gamepad.sThumbLY > STICK_THRESHOLD) *stick |= XPAD_UP;
    if (s.Gamepad.sThumbLY < -STICK_THRESHOLD) *stick |= XPAD_DOWN;
    if (s.Gamepad.sThumbLX < -STICK_THRESHOLD) *stick |= XPAD_LEFT;
    if (s.Gamepad.sThumbLX > STICK_THRESHOLD) *stick |= XPAD_RIGHT;
}

/* The left stick always moves, besides what the d-pad is bound to. */
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

/* While the menu is open, and after it closes until everything held is
 * let go, nothing reaches the game: not the press that closed it. */
static int sControllerLast;

/* RECOMP_PROMPTS=controller: as if the controller had been used last (for
 * test runs, whose scripted input is no device's). */
int input_controller_last(void) {
    static int forced = -1;
    if (forced < 0) {
        const char* e = getenv("RECOMP_PROMPTS");
        forced = e && !strcmp(e, "controller");
    }
    return forced || sControllerLast;
}

static void preview_tick(void);

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

static int read_pad(int chan, uint32_t* held) {
    static int menuHeld;
    uint32_t buttons, stick, keys[LAYERS], pad[LAYERS];
    if (chan != 0) return 0;
    preview_tick();
    mouse();
    controller_state(&buttons, &stick);
    menu_controller(buttons, buttons | stick);
    keyboard(keys);
    controller(buttons, stick, pad);
    if (pad[0] | pad[1] | pad[2]) sControllerLast = 1;
    else if (keys[0] | keys[1] | keys[2]) sControllerLast = 0;
    *held = keys[LAYER_PARK] | pad[LAYER_PARK];
    sMenuHeld = keys[LAYER_MENUS] | pad[LAYER_MENUS];
    sGameHeld = keys[LAYER_GAMES] | pad[LAYER_GAMES];
    if (menu_is_open()) menuHeld = 1;
    else if (menuHeld && !*held && !sMenuHeld && !sGameHeld) menuHeld = 0;
    if (menuHeld) *held = sMenuHeld = sGameHeld = 0;
    return 1;
}

/* The Wii Remote's motor on or off, as the controller's at the strength
 * the setting says (normal: as before there was a setting). */
static int sRumbling;

static void set_motors(int on) {
    static const WORD kSpeed[4] = {0, 0x3000, 0x6000, 0xC000};
    XPadVibration v;
    v.wLeftMotorSpeed = v.wRightMotorSpeed = on ? kSpeed[g_settings.rumble & 3] : 0;
    if (sSdl) sdl_motors(v.wLeftMotorSpeed);
    else pXInputSetState(0, &v);
}

static void rumble(int chan, int on) {
    if (chan != 0 || (!sSdl && !pXInputSetState)) return;
    sRumbling = on;
    set_motors(on);
}

/* A short buzz at the new strength, when the setting is changed: ended
 * at an input poll (read_pad) once it's been long enough. */
static ULONGLONG sPreviewEnd;

void input_rumble_preview(void) {
    if (!sSdl && !pXInputSetState) return;
    set_motors(1);
    sPreviewEnd = GetTickCount64() + 200;
}

static void preview_tick(void) {
    if (sPreviewEnd && GetTickCount64() >= sPreviewEnd) {
        sPreviewEnd = 0;
        set_motors(sRumbling);
    }
}

/* Scripted input goes to the Options tab first, as the controller's does. */
static int script_input(uint32_t held) { return options_script(held) || menu_is_open(); }

void input_init(void) {
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) {
        pXInputGetState = (DWORD(WINAPI*)(DWORD, XPadState*))(void*)GetProcAddress(x, "XInputGetState");
        pXInputSetState = (DWORD(WINAPI*)(DWORD, XPadVibration*))(void*)GetProcAddress(x, "XInputSetState");
    }
#ifdef RECOMP_SDL3
    sdl_init();
#endif
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
