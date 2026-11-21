/* The Wii Remote. On a Wii, WPAD talks to remotes over Bluetooth: it reads
 * their calibration, sets the report format and turns on the IR camera,
 * then parses a report every few milliseconds and hands it to the
 * sampling callback (KPAD's), which KPADRead turns into button edges,
 * acceleration and pointer data.
 *
 * Here WPAD's device-facing calls are answered on the host. Remote 0,
 * held sideways with no extension, connects a second after the game
 * first sets a connect callback, the way a paired remote reconnects at
 * boot (connecting at once would come before the game has registered its
 * own callback, and the game would never count the remote as connected).
 * A timer then stands in for its reports at 200 Hz; each report reads the
 * host's controls (host_input.h). KPAD runs unchanged on top. Remotes 1-3
 * are not connected. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"

int (*g_host_pad_read)(int chan, uint32_t* held);
int (*g_host_menu_read)(int chan, uint32_t* held);
int (*g_host_game_read)(int chan, uint32_t* held);
void (*g_host_pad_rumble)(int chan, int on);

#define WPAD_MAX_CHANS 4
#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER (-1)
#define WPAD_DEV_CORE 0
#define WPAD_DEV_NOT_FOUND 253
#define REPORT_HZ 200u

/* WPADStatus button bits. */
enum {
    WPAD_LEFT = 0x0001, WPAD_RIGHT = 0x0002, WPAD_DOWN = 0x0004, WPAD_UP = 0x0008,
    WPAD_PLUS = 0x0010, WPAD_2 = 0x0100, WPAD_1 = 0x0200, WPAD_B = 0x0400,
    WPAD_A = 0x0800, WPAD_MINUS = 0x1000,
};

/* One g in the accelerometer's units, as calibrated on a typical remote. */
#define ACC_ONE_G 104
/* One swing of a shake, in reports (200 a second): 30 ms. */
#define SWING_REPORTS 6

static struct {
    uint32_t connectCb, extensionCb, samplingCb;
    uint32_t format;
    uint32_t dpdCommand;
    int speakerOn;
} sChan[WPAD_MAX_CHANS];

static uint32_t sReports;
int (*g_host_script_input)(uint32_t held);
static int sConnecting, sConnected;

static int connected(int32_t chan) { return chan == 0 && sConnected; }

/* RECOMP_PAD_SCRIPT: scripted input for remote 0 on top of the host's,
 * for unattended runs. "<seconds>:<buttons> ...", buttons joined by "+"
 * (up down left right a b 1 2 plus minus home shake; on-screen
 * directions), each held from its time until the next entry, timed from
 * the remote's first report. "4:2 4.2: 9:a 9.2:" presses 2 at 4 s and A at 9 s. */
static struct ScriptEntry { double t; uint32_t held; } *sScript;
static int sScriptLen = -1;

static void load_script(void) {
    static const struct { const char* name; uint32_t bit; } kNames[] = {
        {"up", HOST_PAD_SCREEN_UP}, {"down", HOST_PAD_SCREEN_DOWN}, {"left", HOST_PAD_SCREEN_LEFT},
        {"right", HOST_PAD_SCREEN_RIGHT}, {"a", HOST_PAD_A}, {"b", HOST_PAD_B}, {"1", HOST_PAD_1},
        {"2", HOST_PAD_2}, {"plus", HOST_PAD_PLUS}, {"minus", HOST_PAD_MINUS}, {"home", HOST_PAD_HOME},
        {"shake", HOST_PAD_SHAKE}, {"shake2", HOST_PAD_SHAKE2},
    };
    const char* p = getenv("RECOMP_PAD_SCRIPT");
    int cap = 0;
    sScriptLen = 0;
    while (p && *p) {
        char* end;
        double t;
        uint32_t held = 0;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        t = strtod(p, &end);
        if (end == p || *end != ':') {
            fprintf(stderr, "wpad: RECOMP_PAD_SCRIPT: ignoring everything from \"%.20s\"\n", p);
            break;
        }
        if (sScriptLen == cap) {
            struct ScriptEntry* grown = realloc(sScript, (cap ? cap * 2 : 64) * sizeof(*sScript));
            if (!grown) break;
            sScript = grown;
            cap = cap ? cap * 2 : 64;
        }
        p = end + 1;
        while (*p && *p != ' ' && *p != ',') {
            size_t n = strcspn(p, "+ ,"), i;
            for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
                if (strlen(kNames[i].name) == n && strncmp(p, kNames[i].name, n) == 0) held |= kNames[i].bit;
            }
            p += n;
            if (*p == '+') p++;
        }
        sScript[sScriptLen].t = t;
        sScript[sScriptLen++].held = held;
    }
}

static uint32_t script_held(void) {
    static uint64_t start;
    double now;
    uint32_t held = 0;
    int i;
    if (sScriptLen < 0) load_script();
    if (!sScriptLen) return 0;
    if (!start) start = ppc_timebase();
    now = (double)(ppc_timebase() - start) / PPC_TB_HZ;
    for (i = 0; i < sScriptLen && sScript[i].t <= now; i++) held = sScript[i].held;
    return held;
}

/* What was held at the last read, HOME among it: it never reaches the game
 * as a button, but changes the camera (hle_camera.c). */
static uint32_t sHeld;
/* The same, as the menus have it bound (g_host_menu_read). */
static uint32_t sMenuHeld;

uint32_t hle_wpad_held(void) { return sHeld; }

/* Held sideways, d-pad on the left: the remote's UP points left on screen,
 * RIGHT points up. */
static uint16_t wpad_buttons(uint32_t held) {
    static const struct { uint32_t host; uint16_t wpad; } kMap[] = {
        {HOST_PAD_SCREEN_UP, WPAD_RIGHT}, {HOST_PAD_SCREEN_DOWN, WPAD_LEFT},
        {HOST_PAD_SCREEN_LEFT, WPAD_UP},  {HOST_PAD_SCREEN_RIGHT, WPAD_DOWN},
        {HOST_PAD_A, WPAD_A},             {HOST_PAD_B, WPAD_B},
        {HOST_PAD_1, WPAD_1},             {HOST_PAD_2, WPAD_2},
        {HOST_PAD_PLUS, WPAD_PLUS},       {HOST_PAD_MINUS, WPAD_MINUS},
    };
    uint16_t b = 0;
    size_t i;
    for (i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if (held & kMap[i].host) b |= kMap[i].wpad;
    }
    return b;
}

uint32_t hle_wpad_menu_buttons(void) { return wpad_buttons(sMenuHeld); }

/* Whether the remote is being shaken: while either shake key is held, and
 * for a few swings after one is pressed, so a tap is a whole shake. */
#define TAP_REPORTS (4 * SWING_REPORTS)
static int shaking(uint32_t held) {
    static uint32_t last, until;
    uint32_t keys = held & (HOST_PAD_SHAKE | HOST_PAD_SHAKE2);
    if (keys & ~last) until = sReports + TAP_REPORTS;
    last = keys;
    return keys || (int32_t)(until - sReports) > 0;
}

/* Reports go to the sampling callback, like the Bluetooth stack's receive
 * interrupt on a Wii. */
static void report(ppc_t* c) {
    sReports++;
    if (sChan[0].samplingCb) hle_interrupt(c, sChan[0].samplingCb, 0, 0);
}

static void connect(ppc_t* c) {
    sConnected = 1;
    HLE_LOG("WPAD: remote 0 connected");
    hle_timer_stop(HLE_TIMER_PAD);
    hle_timer_start(HLE_TIMER_PAD, PPC_TB_HZ / REPORT_HZ, report);
    if (sChan[0].connectCb) hle_interrupt(c, sChan[0].connectCb, 0, WPAD_ERR_NONE);
}

/* s32 WPADProbe(s32 chan, u32* type) */
void f_80247F10(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    int on = chan >= 0 && chan < WPAD_MAX_CHANS && connected(chan);
    if (c->r[4]) ST32(c->r[4], on ? WPAD_DEV_CORE : WPAD_DEV_NOT_FOUND);
    c->r[3] = on ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
}

/* WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback cb) */
void f_80248030(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    uint32_t cb = c->r[4], old = 0;
    if (chan >= 0 && chan < WPAD_MAX_CHANS) {
        old = sChan[chan].connectCb;
        sChan[chan].connectCb = cb;
        if (cb && chan == 0 && !sConnecting) {
            sConnecting = 1;
            hle_timer_start(HLE_TIMER_PAD, PPC_TB_HZ, connect);
        }
    }
    c->r[3] = old;
}

/* WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback cb) */
void f_802480C0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    uint32_t old = 0;
    if (chan >= 0 && chan < WPAD_MAX_CHANS) {
        old = sChan[chan].extensionCb;
        sChan[chan].extensionCb = c->r[4];
    }
    c->r[3] = old;
}

/* WPADSamplingCallback WPADSetSamplingCallback(s32 chan, WPADSamplingCallback cb) */
void f_80247FA0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    uint32_t old = 0;
    if (chan >= 0 && chan < WPAD_MAX_CHANS) {
        old = sChan[chan].samplingCb;
        sChan[chan].samplingCb = c->r[4];
    }
    c->r[3] = old;
}

/* u32 WPADGetDataFormat(s32 chan) */
void f_80248120(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    c->r[3] = chan >= 0 && chan < WPAD_MAX_CHANS ? sChan[chan].format : 0;
}

/* s32 WPADSetDataFormat(s32 chan, u32 format) */
void f_80248170(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    if (chan >= 0 && chan < WPAD_MAX_CHANS) sChan[chan].format = c->r[4];
    c->r[3] = connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
}

/* void WPADRead(s32 chan, WPADStatus* status): the latest report.
 * WPADStatus: u16 button; s16 accX, accY, accZ; DPDObject obj[4] (s16 x,
 * y; u16 size; u8 traceId; pad); u8 dev; s8 err. */
void f_802488A0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    uint32_t st = c->r[4], held = 0;
    int16_t ax = 0, ay = 0, az = ACC_ONE_G;
    int on = chan >= 0 && chan < WPAD_MAX_CHANS && connected(chan), motion = 0, i;
    int16_t acc[3], dots[4][2];
    hle_prompts_update();
    if (on && g_host_pad_read && !g_host_pad_read(chan, &held)) held = 0;
    /* In a minigame, the remote's buttons as the minigames have them bound. */
    if (on && hle_in_minigame() && g_host_game_read && !g_host_game_read(chan, &held)) held = 0;
    if (on && chan == 0) {
        uint32_t scripted = script_held(), menu = held;
        if (g_host_menu_read && !g_host_menu_read(chan, &menu)) menu = 0;
        if (!g_host_script_input || !g_host_script_input(scripted)) held |= scripted, menu |= scripted;
        if (hle_password_press()) held |= HOST_PAD_2, menu |= HOST_PAD_2;
        sHeld = held;
        sMenuHeld = menu;
    }
    if (on && chan == 0 && (motion = hle_motion(held, sReports, acc, dots)) != 0) {
        /* In minigames, the remote's motion as each asks (hle_motion.c). */
        ax = acc[0];
        ay = acc[1];
        az = acc[2];
    } else if (on && chan == 0 && shaking(held)) {
        /* Iron Tail: a shake up and down, out of the remote's face (KPAD's
         * y, which is what the park's scripts read; along its length they
         * saw nothing), +-2.5 g about 17 times a second. */
        az += (sReports / SWING_REPORTS) & 1 ? ACC_ONE_G * 5 / 2 : -ACC_ONE_G * 5 / 2;
    }
    memset(MA(st), 0, 0x2A);
    ST16(st + 0x00, wpad_buttons(held));
    ST16(st + 0x02, (uint16_t)ax);
    ST16(st + 0x04, (uint16_t)ay);
    ST16(st + 0x06, (uint16_t)az);
    /* IR points in view: the sensor bar's two dots, where a minigame
     * uses the pointer; otherwise none, and the pointer is off. */
    if (motion & 2) {
        for (i = 0; i < 2; i++) {
            ST16(st + 0x08 + 8 * i, (uint16_t)dots[i][0]);
            ST16(st + 0x0A + 8 * i, (uint16_t)dots[i][1]);
            ST16(st + 0x0C + 8 * i, 4); /* size */
            ST8(st + 0x0E + 8 * i, (uint8_t)i);
        }
    }
    ST8(st + 0x28, on ? WPAD_DEV_CORE : WPAD_DEV_NOT_FOUND);
    ST8(st + 0x29, (uint8_t)(on ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER));
}

/* void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAcc* acc) */
void f_80247D40(ppc_t* c) {
    ST16(c->r[5] + 0, ACC_ONE_G);
    ST16(c->r[5] + 2, ACC_ONE_G);
    ST16(c->r[5] + 4, ACC_ONE_G);
}

/* s32 WPADControlDpd(s32 chan, u32 command, WPADCallback cb) */
void f_8024A990(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    if (chan >= 0 && chan < WPAD_MAX_CHANS) sChan[chan].dpdCommand = c->r[4];
    if (c->r[5]) hle_post(c->r[5], (uint32_t)chan, connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER, NULL, 0);
    c->r[3] = connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
}

/* BOOL WPADIsDpdEnabled(s32 chan) */
void f_8024A8A0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    c->r[3] = chan >= 0 && chan < WPAD_MAX_CHANS && sChan[chan].dpdCommand != 0;
}

/* u8 WPADGetDpdFormat(s32 chan) */
void f_8024A8F0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    c->r[3] = chan >= 0 && chan < WPAD_MAX_CHANS ? sChan[chan].dpdCommand : 0;
}

/* void WPADControlMotor(s32 chan, u32 command): 1 starts the rumble. */
void f_802484B0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    if (connected(chan) && g_host_pad_rumble) g_host_pad_rumble(chan, c->r[4] == 1);
}

/* s32 WPADControlLed(s32 chan, u8 pattern, WPADCallback cb) */
void f_80248600(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    if (c->r[5]) hle_post(c->r[5], (uint32_t)chan, connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER, NULL, 0);
    c->r[3] = connected(chan) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
}

/* s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback cb).
 * WPADInfo: BOOL dpd, speaker, attach, lowBat, nearempty; u8 battery,
 * led, protocol, firmware. */
void f_80248290(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    uint32_t info = c->r[4];
    if (!connected(chan)) {
        c->r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
        return;
    }
    memset(MA(info), 0, 0x18);
    ST32(info + 0x00, sChan[chan].dpdCommand != 0);
    ST32(info + 0x04, (uint32_t)sChan[chan].speakerOn);
    ST8(info + 0x14, 4);             /* battery: full */
    ST8(info + 0x15, (uint8_t)(1 << chan)); /* player LED */
    if (c->r[5]) hle_post(c->r[5], (uint32_t)chan, WPAD_ERR_NONE, NULL, 0);
    c->r[3] = WPAD_ERR_NONE;
}

/* u8 WPADGetRadioSensitivity(s32 chan): a strong signal. */
void f_80246BC0(ppc_t* c) { c->r[3] = connected((int32_t)c->r[3]) ? 100 : 0; }

/* s32 WPADDisconnect(s32 chan): the remote stays connected. */
void f_80247E50(ppc_t* c) { c->r[3] = WPAD_ERR_NONE; }

/* The speaker. Sound sent to it is dropped. */

/* s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback cb):
 * command 0 turns it off, 1 on. */
void f_80248DF0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    if (!connected(chan)) {
        c->r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
        return;
    }
    if (c->r[4] == 0) sChan[chan].speakerOn = 0;
    else if (c->r[4] == 1) sChan[chan].speakerOn = 1;
    if (c->r[5]) hle_post(c->r[5], (uint32_t)chan, WPAD_ERR_NONE, NULL, 0);
    c->r[3] = WPAD_ERR_NONE;
}

/* BOOL WPADIsSpeakerEnabled(s32 chan) */
void f_80248DA0(ppc_t* c) {
    int32_t chan = (int32_t)c->r[3];
    c->r[3] = chan >= 0 && chan < WPAD_MAX_CHANS && sChan[chan].speakerOn;
}

/* BOOL WPADCanSendStreamData(s32 chan) */
void f_8024A5C0(ppc_t* c) { c->r[3] = connected((int32_t)c->r[3]); }

/* s32 WPADSendStreamData(s32 chan, void* data, u16 len) */
void f_8024A650(ppc_t* c) { c->r[3] = connected((int32_t)c->r[3]) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER; }
