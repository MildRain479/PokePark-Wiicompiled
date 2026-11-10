/* The game's window, which a device (gx_device.h) shows its pictures in.
 *
 * Environment:
 *   RECOMP_GX_HIDDEN=1         do not show the window */
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gx_device.h"
#include "host_settings.h"

static HWND sWnd;
static int sHidden, sFullscreen;
/* EFB pixels to screen pixels. In 16:9 the game squeezes a wide picture
 * into the EFB's 640 columns for the TV to stretch out: its cameras
 * project a 16:9 picture onto the 640x456 it shows. So it is shown at
 * 16:9. (Shown 4/3 as wide, 1.87:1, it was 5% too wide, with bars above
 * and below.) */
static double sPixelAspect = 1;
static WINDOWPLACEMENT sWindowed;

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) exit(0);
    /* Alt+Enter toggles fullscreen; Alt alone doesn't open the window
     * menu, which would hold the game's keys. */
    if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {
        if (!(lp & (1 << 30))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            settings_save();
        }
        return 0;
    }
    if (msg == WM_SYSCHAR || (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU)) return 0;
    if (menu_window_message(h, msg, wp, lp)) return 0;
    return DefWindowProcA(h, msg, wp, lp);
}

void win_create(void) {
    WNDCLASSA wc;
    RECT r = {0, 0, 640 * 2, 456 * 2}; /* the 456 lines the game shows, so the picture fills it */
    const char* v;
    extern int hle_widescreen(void);
    if (hle_widescreen()) {
        sPixelAspect = 16.0 / 9.0 * 456.0 / 640.0;
        r.right = (LONG)(r.right * sPixelAspect + 0.5);
    }
    {
        /* At the canvas's shape (the aspect ratio setting), made smaller
         * if that's more than the screen has room for. */
        RECT work;
        int mx, my;
        gx_canvas(&mx, &my);
        r.right += (LONG)(4 * mx * sPixelAspect + 0.5);
        r.bottom += 4 * my;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            double k = 0.9 * (work.right - work.left) / r.right, kh = 0.9 * (work.bottom - work.top) / r.bottom;
            if (kh < k) k = kh;
            if (k < 1) {
                r.right = (LONG)(r.right * k);
                r.bottom = (LONG)(r.bottom * k);
            }
        }
    }
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "PokeParkRecomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    sWnd = CreateWindowA("PokeParkRecomp", "PokePark (recompiled)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    v = getenv("RECOMP_GX_HIDDEN");
    sHidden = v && *v && *v != '0';
    if (!sHidden) ShowWindow(sWnd, SW_SHOW);
}

void* win_handle(void) { return sWnd; }
int win_hidden(void) { return sHidden; }
double win_pixel_aspect(void) { return sPixelAspect; }

void win_size(int* w, int* h) {
    RECT r;
    GetClientRect(sWnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

void win_set_fullscreen(int on) {
    DWORD style = (DWORD)GetWindowLongA(sWnd, GWL_STYLE);
    if (sHidden || on == sFullscreen) return;
    sFullscreen = on;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        sWindowed.length = sizeof(sWindowed);
        GetWindowPlacement(sWnd, &sWindowed);
        GetMonitorInfoA(MonitorFromWindow(sWnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(sWnd, GWL_STYLE, (LONG)((style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
        SetWindowPos(sWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(sWnd, GWL_STYLE, (LONG)((style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(sWnd, &sWindowed);
        SetWindowPos(sWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

void win_set_title(const char* title) { SetWindowTextA(sWnd, title); }

void win_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* The compositor's own rate is another monitor's when there are several.
 * Whole numbers just under a multiple of 10 are the NTSC-style rates (119
 * is 119.88 Hz). Asked again every few seconds, as the window can move. */
double win_display_hz(void) {
    static double hz;
    static DWORD checked;
    DWORD now = GetTickCount();
    MONITORINFOEXA mi;
    DEVMODEA dm;
    if (checked && now - checked < 3000) return hz;
    checked = now ? now : 1;
    hz = 0;
    mi.cbSize = sizeof(mi);
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoA(MonitorFromWindow(sWnd, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO*)&mi) &&
        EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        hz = dm.dmDisplayFrequency;
        if (dm.dmDisplayFrequency % 10 == 9) hz = (hz + 1) * 1000.0 / 1001.0;
    }
    return hz;
}

int gxb_display_hz(void) { return (int)(win_display_hz() + 0.5); }
