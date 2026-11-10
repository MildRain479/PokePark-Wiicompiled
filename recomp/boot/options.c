/* The Options tab of the pause menu: the settings (settings.c), in a
 * book like the PokéPark Pad's and drawn with its pictures and the game's
 * font, over the Pad. While the Pad is on its main page an "Options" tab
 * shows beside its title; right opens it (left and right do nothing on
 * the Pad), and left or Back on its first column goes back.
 *
 *   The left page lists the sections, the right page their settings.
 *   Up and down choose; left and right change a setting or move between a
 *   binding's two slots; Select rebinds (press the key, mouse button or
 *   controller button, or Esc to cancel); Delete, or Y, clears.
 *
 * Its controls are the game's, as bound (Select is 2, Back is 1, as the
 * Pad's bar says), and the arrow keys, Enter and Esc; the mouse clicks
 * what it points at. The book is drawn into an image laid over the
 * picture (gl_backend.c), only again when something on it changes,
 * leaving the bar at the bottom clear: the game's, which shows the
 * player's keys (hle_prompts.c), shows through. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_input.h"
#include "host_settings.h"

#include "canvas.h"

int hle_pause_main_page(void); /* hle_pause.c */
int hle_file_select_shown(void);
int hle_password_screen_shown(void);
void hle_password_unlock_all(void);
const uint32_t* hle_pause_image(const char* name, const char* alpha, int* w, int* h);
int hle_font_metrics(int* cellW, int* cellH, int* baseline); /* hle_prompts.c */
int hle_font_glyph(uint16_t code, uint8_t* cov, int* left, int* advance);
int display_scale(void); /* gx_frames.c: as applied */
int display_msaa(void);
int display_renderer(void); /* RENDERER_*: the one drawing */
int hle_widescreen(void);    /* the game in its 16:9 mode: from the start */
int texpack_count(void);     /* texpack.c: the pictures in the textures folder */
int gxb_display_hz(void);

/* ------------------------------------------------------------------------ */
/* What's on it */

#ifdef __ANDROID__
/* No keyboard: its section isn't there. Dual screen is, in its place,
 * only on a handheld with two screens. */
enum { SEC_DISPLAY, SEC_CONTROLLER, SEC_GAMEPLAY, SEC_DUAL, SECTIONS, SEC_KEYBOARD = -1 };
static const char* const kSection[SECTIONS] = {"Display", "Controller", "Gameplay", "Dual screen"};
static const int kSectionColor[SECTIONS] = {0, 2, 3, 1}; /* which of the Pad's button colors */
static int sections(void) { return second_screen_device() ? SECTIONS : SEC_DUAL; }
#else
enum { SEC_DISPLAY, SEC_KEYBOARD, SEC_CONTROLLER, SEC_GAMEPLAY, SECTIONS, SEC_DUAL = -1 };
static const char* const kSection[SECTIONS] = {"Display", "Keyboard", "Controller", "Gameplay"};
static const int kSectionColor[SECTIONS] = {0, 1, 2, 3};
static int sections(void) { return SECTIONS; }
#endif

/* A row: a setting (settings_step), a binding (an action), resetting the
 * controls, or closing the game (selected twice, as unsaved progress is
 * lost). */
enum { ROW_SETTING, ROW_BINDING, ROW_RESET, ROW_CLOSE };
typedef struct {
    int kind, which;
    const char* label;
} Row;

/* On Android the game fills the screen, and Vulkan always draws. */
static const Row kDisplay[] = {
#ifndef __ANDROID__
    {ROW_SETTING, SET_FULLSCREEN, "Fullscreen"},
#endif
    {ROW_SETTING, SET_SCALE, "Resolution"},
    {ROW_SETTING, SET_ASPECT, "Aspect ratio"},
    {ROW_SETTING, SET_MSAA, "Anti-aliasing"},     {ROW_SETTING, SET_FRAME_RATE, "Frame rate"},
    {ROW_SETTING, SET_FOV, "Field of view"},       {ROW_SETTING, SET_ANISO, "Filtering"},
    {ROW_SETTING, SET_TEXTURE_PACK, "Texture pack"},
#ifndef __ANDROID__
    {ROW_SETTING, SET_RENDERER, "Renderer"},
#endif
    {ROW_SETTING, SET_UPDATES, "Updates"},
    {ROW_CLOSE, 0, "Close game"},
};
static const Row kGameplay[] = {
    {ROW_SETTING, SET_TEXT_SPEED, "Text speed"},
    {ROW_SETTING, SET_BERRIES, "Berries"},
    {ROW_SETTING, SET_PRICES, "Prices"},
    {ROW_SETTING, SET_DAMAGE_DEALT, "Damage dealt"},
    {ROW_SETTING, SET_DAMAGE_TAKEN, "Damage taken"},
    {ROW_SETTING, SET_CHASE_SPEED, "Chase speed"},
    {ROW_SETTING, SET_AUTOSAVE, "Autosave"},
};
static const Row kDual[] = {
    {ROW_SETTING, SET_DUAL_SCREEN, "Dual screen"},
    {ROW_SETTING, SET_SWAP_SCREENS, "Swap screens"},
    {ROW_SETTING, SET_MINIMAP_TOP, "Map on main"},
    {ROW_SETTING, SET_FRIENDS_LIST, "Friends list"},
};
static const char* const kAction[ACT_COUNT] = {
    "Up", "Down", "Left", "Right", "2: Jump", "1: Dash", "A: Thunderbolt", "B: Camera", "+: Pad", "-: Map", "First person",
    "Shake", "Shake 2",
};

/* Android's Controller section starts with the touch controls' row, where
 * a touch reaches it (touches don't scroll). */
#ifdef __ANDROID__
#define TOUCH_ROW 1
#else
#define TOUCH_ROW 0
#endif

static int row_count(int sec) {
    return sec == SEC_DISPLAY    ? (int)(sizeof(kDisplay) / sizeof(kDisplay[0]))
           : sec == SEC_GAMEPLAY ? (int)(sizeof(kGameplay) / sizeof(kGameplay[0]))
           : sec == SEC_DUAL     ? (int)(sizeof(kDual) / sizeof(kDual[0]))
           : sec == SEC_KEYBOARD ? ACT_COUNT + 3 /* camera speed, invert, reset */
                                 : TOUCH_ROW + ACT_COUNT + 2; /* (touch controls,) rumble, reset */
}

static Row row_at(int sec, int i) {
    Row r = {ROW_RESET, 0, "Reset all controls"};
    if (sec == SEC_DISPLAY) return kDisplay[i];
    if (sec == SEC_GAMEPLAY) return kGameplay[i];
    if (sec == SEC_DUAL) return kDual[i];
    if (TOUCH_ROW && sec == SEC_CONTROLLER && i-- == 0) {
        r.kind = ROW_SETTING;
        r.which = SET_TOUCH;
        r.label = "Touch controls";
        return r;
    }
    if (i < ACT_COUNT) {
        r.kind = ROW_BINDING;
        r.which = i;
        r.label = kAction[i];
    } else if (sec == SEC_KEYBOARD && i == ACT_COUNT) {
        r.kind = ROW_SETTING;
        r.which = SET_SENSITIVITY;
        r.label = "Camera speed";
    } else if (sec == SEC_CONTROLLER && i == ACT_COUNT) {
        r.kind = ROW_SETTING;
        r.which = SET_RUMBLE;
        r.label = "Rumble";
    } else if (sec == SEC_KEYBOARD && i == ACT_COUNT + 1) {
        r.kind = ROW_SETTING;
        r.which = SET_INVERT;
        r.label = "Invert camera";
    }
    return r;
}

static void setting_text(int which, char* out, size_t n) {
    int s;
    switch (which) {
    case SET_FULLSCREEN: snprintf(out, n, g_settings.fullscreen ? "On" : "Off"); break;
    case SET_SCALE:
        s = g_settings.scale ? g_settings.scale : display_scale();
        snprintf(out, n, g_settings.scale ? "%dx" : "Auto (%dx)", s);
        break;
    case SET_MSAA:
        if (g_settings.msaa == 1) snprintf(out, n, "Off");
        else if (display_msaa() < g_settings.msaa) snprintf(out, n, "%dx (GPU %dx)", g_settings.msaa, display_msaa());
        else snprintf(out, n, "%dx", g_settings.msaa);
        break;
    case SET_FRAME_RATE: snprintf(out, n, "%d fps", g_settings.frame_rate); break;
    case SET_FOV: snprintf(out, n, "%d%%", g_settings.fov); break;
    case SET_SENSITIVITY: snprintf(out, n, "%.1f", g_settings.mouse_sensitivity); break;
    case SET_INVERT: snprintf(out, n, g_settings.invert_tilt ? "On" : "Off"); break;
    case SET_TOUCH: snprintf(out, n, g_settings.touch_controls ? "On" : "Off"); break;
    case SET_MINIMAP_TOP: snprintf(out, n, g_settings.minimap_top ? "On" : "Off"); break;
    case SET_AUTOSAVE: snprintf(out, n, g_settings.autosave ? "On" : "Off"); break;
    case SET_DUAL_SCREEN: snprintf(out, n, g_settings.dual_screen ? "On" : "Off"); break;
    case SET_SWAP_SCREENS: snprintf(out, n, g_settings.swap_screens ? "On" : "Off"); break;
    case SET_FRIENDS_LIST: snprintf(out, n, g_settings.friends_list ? "On" : "Off"); break;
    case SET_ANISO: snprintf(out, n, g_settings.aniso == 1 ? "Original" : "%dx", g_settings.aniso); break;
    case SET_TEXTURE_PACK:
        if (!texpack_count()) snprintf(out, n, "None found");
        else snprintf(out, n, g_settings.texture_pack ? "On (%d)" : "Off", texpack_count());
        break;
    case SET_ASPECT: {
        static const char* const kShape[ASPECTS] = {"4:3", "16:9", "16:10", "21:9"};
        snprintf(out, n, "%s%s", kShape[g_settings.aspect],
                 (g_settings.aspect == ASPECT_4_3) == hle_widescreen() ? " (restart)" : "");
        break;
    }
    case SET_RENDERER:
        snprintf(out, n, "%s%s", g_settings.renderer == RENDERER_VULKAN ? "Vulkan" : "OpenGL",
                 g_settings.renderer != display_renderer() ? " (restart)" : "");
        break;
    case SET_RUMBLE: {
        static const char* const kRumble[4] = {"Off", "Light", "Normal", "Strong"};
        snprintf(out, n, "%s", kRumble[g_settings.rumble & 3]);
        break;
    }
    case SET_TEXT_SPEED:
        if (g_settings.text_speed == 0) snprintf(out, n, "Instant");
        else snprintf(out, n, g_settings.text_speed == 1 ? "Original" : "%dx", g_settings.text_speed);
        break;
    case SET_BERRIES: snprintf(out, n, g_settings.berries == 100 ? "Original" : "%d%%", g_settings.berries); break;
    case SET_PRICES:
        if (g_settings.prices == 0) snprintf(out, n, "Free");
        else snprintf(out, n, g_settings.prices == 100 ? "Original" : "%d%%", g_settings.prices);
        break;
    case SET_DAMAGE_DEALT:
        snprintf(out, n, g_settings.damage_dealt == 100 ? "Original" : "%d%%", g_settings.damage_dealt);
        break;
    case SET_DAMAGE_TAKEN:
        if (g_settings.damage_taken == 0) snprintf(out, n, "None");
        else snprintf(out, n, g_settings.damage_taken == 100 ? "Original" : "%d%%", g_settings.damage_taken);
        break;
    case SET_CHASE_SPEED:
        snprintf(out, n, g_settings.chase_speed == 100 ? "Original" : "%d%%", g_settings.chase_speed);
        break;
    case SET_UPDATES:
        s = update_state();
        if (s == UPDATE_NEWER) snprintf(out, n, "%s is out", update_tag());
        else snprintf(out, n, "%s", s == UPDATE_OFF ? "Off" : s == UPDATE_CHECKING ? "Checking" : s == UPDATE_CURRENT ? "Up to date" : "Can't check");
        break;
    default: *out = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* State */

enum { CAPTURE_NONE, CAPTURE_KEY, CAPTURE_PAD };
#define VISIBLE_ROWS 8

static volatile int sOpen;
/* What it was opened from: the Pad, or the save file screen. */
enum { FROM_PAD, FROM_FILES };
static int sFrom;
static int sSection, sOnRows, sRow, sSlot, sTop, sCapture;
static volatile unsigned sChanges; /* what's drawn is redrawn when this changes */

int options_is_open(void) { return sOpen; }

static void changed(void) { sChanges++; }

/* "Close game" was selected once, and nothing has changed since. */
static unsigned sCloseArmed = ~0u;
static int close_armed(void) { return sCloseArmed == sChanges; }

/* Whether what it opens from is showing. */
static int shown(int from) { return from == FROM_PAD ? hle_pause_main_page() : hle_file_select_shown(); }

static void set_open(int open);

static void open_from(int from) {
    sFrom = from;
    set_open(1);
}

static void set_open(int open) {
    sOpen = open;
    sCapture = CAPTURE_NONE;
    sOnRows = 0;
    changed();
}

/* The button (-) on the save file screen, or the password screen's. */
static int file_button_shown(void) { return hle_file_select_shown() || hle_password_screen_shown(); }

static void file_button(void) {
    if (hle_file_select_shown()) open_from(FROM_FILES);
    else hle_password_unlock_all();
}

static void show_row(void) {
    if (sRow < sTop) sTop = sRow;
    if (sRow >= sTop + VISIBLE_ROWS) sTop = sRow - VISIBLE_ROWS + 1;
}

static void up_down(int d) {
    if (!sOnRows) {
        sSection = (sSection + d + sections()) % sections();
        sRow = sTop = sSlot = 0;
    } else {
        sRow = (sRow + d + row_count(sSection)) % row_count(sSection);
        show_row();
    }
    changed();
}

static void enter_rows(void) {
    sOnRows = 1;
    changed();
}

static void back(void) {
    if (sOnRows) {
        sOnRows = 0;
        changed();
    } else {
        set_open(0);
    }
}

static void left_right(int d) {
    Row r;
    if (!sOnRows) {
        if (d < 0) set_open(0);
        else enter_rows();
        return;
    }
    r = row_at(sSection, sRow);
    if (r.kind == ROW_SETTING) {
        settings_step(r.which, d);
    } else if (r.kind == ROW_BINDING && sSlot + d >= 0 && sSlot + d < BIND_SLOTS) {
        sSlot += d;
    } else if (d < 0) {
        sOnRows = 0;
    }
    changed();
}

static void select_(void) {
    Row r;
    if (!sOnRows) {
        enter_rows();
        return;
    }
    r = row_at(sSection, sRow);
    if (r.kind == ROW_SETTING && r.which == SET_UPDATES && update_state() == UPDATE_NEWER) {
        update_open(); /* its page, to download it from */
    } else if (r.kind == ROW_SETTING) {
        settings_step(r.which, 1);
    } else if (r.kind == ROW_BINDING) {
        sCapture = sSection == SEC_CONTROLLER ? CAPTURE_PAD : CAPTURE_KEY;
    } else if (r.kind == ROW_CLOSE) {
        if (close_armed()) exit(0); /* as closing the window does */
        changed();
        sCloseArmed = sChanges;
        return;
    } else {
        settings_default_controls();
        settings_save();
    }
    changed();
}

static void clear(void) {
    Row r = row_at(sSection, sRow);
    if (!sOnRows || r.kind != ROW_BINDING) return;
    if (sSection == SEC_CONTROLLER) g_settings.pad[r.which][sSlot] = 0;
    else g_settings.key[r.which][sSlot] = 0;
    settings_save();
    changed();
}

static void bind(int vk, uint32_t pad) {
    settings_bind(row_at(sSection, sRow).which, sSlot, vk, pad);
    sCapture = CAPTURE_NONE;
    changed();
}

/* ------------------------------------------------------------------------ */
/* Where things are: in a 1621 x 912 picture, where the Pad was measured,
 * scaled to the window's height and centred, as the game's picture is. */

#define REF_W 1621
#define REF_H 912

static const Rect kTab = {810, 46, 1046, 120};            /* the other tab, at the right page's top */
static const Rect kButton[4] = {
    {333, 150, 748, 230}, {320, 262, 737, 342}, {328, 374, 746, 454}, {328, 487, 746, 562}};
static const Rect kPage = {845, 40, 1325, 765};           /* the right page's writing */
#define ROWS_TOP 140.0f
#define ROW_PITCH 72.0f
#define ROW_HEIGHT 60.0f
#define BAR_TOP 771.0f /* the game's button bar, left clear */
#define BAR_BOTTOM 859.0f

static Rect row_rect(int visible) {
    Rect r = {kPage.x0, ROWS_TOP + visible * ROW_PITCH, kPage.x1, ROWS_TOP + visible * ROW_PITCH + ROW_HEIGHT};
    return r;
}

/* Where a binding row's slot k is centred. */
static float slot_x(int k) { return k ? 1262.0f : 1140.0f; }

static void draw_picture(const char* name, const char* alpha, Rect src, Rect dst, uint32_t tint) {
    int w, h;
    const uint32_t* img = hle_pause_image(name, alpha, &w, &h);
    if (src.x1 <= 0) {
        src.x0 = src.y0 = 0;
        src.x1 = (float)w;
        src.y1 = (float)h;
    }
    draw_image(img, w, h, src, dst, tint, 0);
}

/* ------------------------------------------------------------------------ */
/* The book */

#define BROWN 0xFF8C2A10u
#define GREEN 0xFF448822u
#define CREAM 0xFFF2EAD2u
#define INK 0xFF2A2A2Au
#define PURPLE 0xFF5B2A9Eu
#define WAIT 0xFFE05A00u

static const Rect kWhole = {0, 0, 0, 0};

/* The icon of what's bound to a remote button (HOST_PAD_*), white with
 * an edge, in dst; 0 if there's none. */
static int draw_button_icon(uint32_t button, Rect dst, uint32_t edge) {
    static uint8_t cov[96 * 96];
    static float mask[160 * 160];
    int n = 96, bw, bh, bx0, by0, i, j, pad = (int)ceilf(4 * sScale) + 1;
    float dw = X(dst.x1) - X(dst.x0), dh = Y(dst.y1) - Y(dst.y0);
    if (!g_host_prompt_icon || !g_host_prompt_icon(button, cov, n, n)) return 0;
    bw = (int)dw + 2 * pad;
    bh = (int)dh + 2 * pad;
    if (bw * bh > (int)(sizeof(mask) / sizeof(mask[0]))) return 0;
    bx0 = (int)X(dst.x0) - pad;
    by0 = (int)Y(dst.y0) - pad;
    memset(mask, 0, sizeof(mask));
    for (j = pad; j < bh - pad; j++) {
        for (i = pad; i < bw - pad; i++) {
            int u = (int)((i - pad + 0.5f) / dw * n), v = (int)((j - pad + 0.5f) / dh * n);
            if (u < n && v < n) mask[j * bw + i] = cov[v * n + u] / 255.0f;
        }
    }
    paint_mask(mask, bw, bh, bx0, by0, 0xFFFFFFFFu, edge, 3.0f, 0);
    return 1;
}

/* Beside the save file screen's Arcade plank, a plank like it: Options,
 * with the icon of what opens it (the - button's key). */
static const Rect kFileButton = {1332, 655, 1602, 748}; /* as tall as Arcade's */

static void draw_file_button(const char* name) {
    /* The plank in three: its ends, and a piece of its middle stretched,
     * past the Pikachu at its right. */
    static const Rect kLeft = {2, 5, 70, 58}, kMiddle = {100, 5, 140, 58}, kRight = {200, 5, 256, 58};
    float k = (kFileButton.y1 - kFileButton.y0) / (kLeft.y1 - kLeft.y0);
    float lx = kFileButton.x0 + (kLeft.x1 - kLeft.x0) * k, rx = kFileButton.x1 - (kRight.x1 - kRight.x0) * k;
    Rect l = {kFileButton.x0, kFileButton.y0, lx, kFileButton.y1}, m = {lx, kFileButton.y0, rx, kFileButton.y1};
    Rect r = {rx, kFileButton.y0, kFileButton.x1, kFileButton.y1};
    Rect icon = {kFileButton.x0 + 18, 676, kFileButton.x0 + 70, 728};
    int hasIcon;
    draw_picture("MaAtMenu", NULL, kLeft, l, 0xFFFFFF);
    draw_picture("MaAtMenu", NULL, kMiddle, m, 0xFFFFFF);
    draw_picture("MaAtMenu", NULL, kRight, r, 0xFFFFFF);
    hasIcon = draw_button_icon(HOST_PAD_MINUS, icon, 0xFF7A2020u);
    draw_text(name, hasIcon ? (icon.x1 + kFileButton.x1) / 2 : (kFileButton.x0 + kFileButton.x1) / 2, 703, 56,
              ALIGN_CENTRE, 0xFFFFFFFFu, 0xFF7A2020u, 4.0f, 1, hasIcon ? 175 : 220);
    /* Over it, a newer release's news (Options' Updates row opens its page). */
    if (update_state() == UPDATE_NEWER) {
        char news[64];
        snprintf(news, sizeof(news), "Update: %s", update_tag());
        draw_text(news, (kFileButton.x0 + kFileButton.x1) / 2, 632, 40, ALIGN_CENTRE, 0xFFFFFFFFu, 0xFF7A2020u, 3.0f,
                  1, kFileButton.x1 - kFileButton.x0);
    }
}

/* The other tab: a piece of the title strip, a little darker, with an
 * arrowhead pointing the way to it. */
static void draw_tab(const char* name, int right) {
    static const Rect kStrip = {40, 18, 270, 72}, kArrow = {3, 8, 28, 54};
    int w, h;
    const uint32_t* arrow;
    draw_picture("PmPocketbookL01", "PmPocketbookL01A", kStrip, kTab, 0xD8D0C0);
    draw_text(name, (kTab.x0 + kTab.x1) / 2 + (right ? -14 : 14), (kTab.y0 + kTab.y1) / 2 + 2, 38, ALIGN_CENTRE,
              BROWN, 0, 0, 1, kTab.x1 - kTab.x0 - 70);
    arrow = hle_pause_image("PmArrowheadMat", "PmArrowheadMatAlp", &w, &h);
    {
        Rect d = {right ? kTab.x1 - 34 : kTab.x0 + 14, kTab.y0 + 20, right ? kTab.x1 - 14 : kTab.x0 + 34, kTab.y1 - 16};
        draw_image(arrow, w, h, kArrow, d, 0xFFFFFF, right);
    }
}

/* What doesn't change: the pages, the title and the other tab. */
/* The left page reaches under the right one: from the save file screen
 * there's no Pad beneath to fill the spine between them. */
static const Rect kRightPage = {774, 7, 1499, 893}, kLeftPage = {205, 9, 840, 897};
static const Rect kLeaves = {1, 4, 63, 232}, kLeavesAt = {1342, 46, 1466, 504};

static void draw_pages(void) {
    draw_picture("PmPocketbookR", "PmPocketbookRA", kWhole, kRightPage, 0xFFFFFF);
    draw_picture("PmReaf01", "PmReaf01A", kLeaves, kLeavesAt, 0xFFFFFF);
    if (sFrom == FROM_PAD) draw_tab("Pok\xE9Park Pad", 0); /* the font's Latin-1 */
    draw_picture("PmPocketbookL01", "PmPocketbookL01A", kWhole, kLeftPage, 0xFFFFFF);
    draw_text("Options", 450, 104, 54, ALIGN_CENTRE, BROWN, 0, 0, 1, 0);
}

static void draw_book(void) {
    static const Rect kPocket[4] = {{8, 13, 295, 104}, {21, 113, 303, 218}, {21, 113, 303, 218}, {27, 223, 297, 327}};
    static const Rect kPocketAt[4] = {
        {262, 230, 778, 394}, {272, 342, 780, 534}, {272, 454, 780, 646}, {290, 560, 780, 760}};
    static const Rect kBlue[4] = {{8, 10, 120, 69}, {9, 90, 119, 145}, {8, 10, 120, 69}, {8, 170, 120, 229}};
    static const Rect kArrow = {3, 8, 28, 54};
    int i, n = row_count(sSection), aw, ah;
    const uint32_t* arrow = hle_pause_image("PmArrowheadMat", "PmArrowheadMatAlp", &aw, &ah);
    char buf[64];


    /* The sections, as the Pad's buttons: each in its pocket, the chosen
     * one orange. */
    if (sSection >= sections()) sSection = 0;
    for (i = 0; i < sections(); i++) {
        Rect src = kBlue[kSectionColor[i]];
        Rect at = kButton[i];
        if (i == sSection) {
            src.x0 += 128;
            src.x1 += 128;
        }
        draw_picture("PmMenuButton", NULL, src, at, sOnRows && i == sSection ? 0xE8E0E0 : 0xFFFFFF);
        draw_text(kSection[i], (at.x0 + at.x1) / 2 + 8, (at.y0 + at.y1) / 2 + 2, 60, ALIGN_CENTRE, 0xFFFFFFFFu, PURPLE,
                  3.5f, 1, at.x1 - at.x0 - 60);
        draw_picture("PmButtonPocket", NULL, kPocket[i], kPocketAt[i], 0xFFFFFF);
    }

    /* The section's settings, a bar each as the Pad's counts are shown. */
    draw_text(kSection[sSection], 1192, 88, 52, ALIGN_CENTRE, BROWN, 0, 0, 1, 0);
    if (sSection == SEC_KEYBOARD || sSection == SEC_CONTROLLER) {
        draw_text("Slot 1", slot_x(0), 124, 24, ALIGN_CENTRE, 0xFF8A7458u, 0, 0, 0, 0);
        draw_text("Slot 2", slot_x(1), 124, 24, ALIGN_CENTRE, 0xFF8A7458u, 0, 0, 0, 0);
    }
    for (i = sTop; i < n && i < sTop + VISIBLE_ROWS; i++) {
        Row r = row_at(sSection, i);
        Rect bar = row_rect(i - sTop);
        float my = (bar.y0 + bar.y1) / 2;
        int chosen = sOnRows && i == sRow;
        draw_box(bar, 10, chosen ? 0xE0FFC070u : 0xB6CCBB99u);
        draw_text(r.label, bar.x0 + 14, my + 2, r.kind == ROW_BINDING ? 40 : 42, ALIGN_LEFT, GREEN, CREAM, 2.0f, 1,
                  r.kind == ROW_SETTING ? 215 : 210);
        if (r.kind == ROW_SETTING) {
            float cx = 1190;
            setting_text(r.which, buf, sizeof(buf));
            draw_text(buf, cx, my + 2, 40, ALIGN_CENTRE, INK, 0, 0, 1, 170);
            if (chosen) {
                Rect l = {cx - 112, my - 16, cx - 96, my + 16}, rr = {cx + 96, my - 16, cx + 112, my + 16};
                draw_image(arrow, aw, ah, kArrow, l, 0xFFFFFF, 0);
                draw_image(arrow, aw, ah, kArrow, rr, 0xFFFFFF, 1);
            }
        } else if (r.kind == ROW_CLOSE) {
            if (close_armed()) draw_text("Again to close", 1190, my + 2, 34, ALIGN_CENTRE, WAIT, 0, 0, 1, 230);
        } else if (r.kind == ROW_BINDING) {
            int k;
            for (k = 0; k < BIND_SLOTS; k++) {
                float cx = slot_x(k);
                int none;
                if (chosen && k == sSlot) {
                    Rect b = {cx - 58, bar.y0 + 7, cx + 58, bar.y1 - 7};
                    draw_box(b, 8, 0xE6FFFFFFu);
                }
                if (chosen && k == sSlot && sCapture != CAPTURE_NONE) {
                    draw_text(sCapture == CAPTURE_KEY ? "Press a key" : "Press a button", cx, my + 2, 30, ALIGN_CENTRE,
                              WAIT, 0, 0, 1, 112);
                    continue;
                }
                if (sSection == SEC_CONTROLLER) {
                    snprintf(buf, sizeof(buf), "%s", settings_pad_name(g_settings.pad[r.which][k]));
                } else {
                    settings_key_name(g_settings.key[r.which][k], buf, sizeof(buf));
                }
                none = !strcmp(buf, "-");
                draw_text(buf, cx, my + 2, 34, ALIGN_CENTRE, none ? 0xFF9A8A70u : INK, 0, 0, !none, 112);
            }
        }
    }
    /* More above or below. */
    if (sTop > 0) draw_triangle((kPage.x0 + kPage.x1) / 2, ROWS_TOP - 8, 9, 1, PURPLE);
    if (sTop + VISIBLE_ROWS < n) draw_triangle((kPage.x0 + kPage.x1) / 2, ROWS_TOP + VISIBLE_ROWS * ROW_PITCH - 4, 9, -1, PURPLE);
    /* What the keys do here, when it's not plain. */
    if (sOnRows && row_at(sSection, sRow).kind == ROW_SETTING && row_at(sSection, sRow).which == SET_UPDATES &&
        update_state() == UPDATE_NEWER) {
        draw_text("Select: open its download page", (kPage.x0 + kPage.x1) / 2, 752, 28, ALIGN_CENTRE, 0xFF7A6448u, 0,
                  0, 0, 0);
    }
    if (sOnRows && row_at(sSection, sRow).kind == ROW_BINDING) {
        draw_text(sCapture != CAPTURE_NONE ? "Esc: cancel" : input_controller_last() ? "Select: rebind    Y: clear"
                                                                                   : "Select: rebind    Delete: clear",
                  (kPage.x0 + kPage.x1) / 2, 752, 28, ALIGN_CENTRE, 0xFF7A6448u, 0, 0, 0, 0);
    }
}

static unsigned sDraws; /* times the image was drawn again */

unsigned options_draws(void) { return sDraws; }

const uint32_t* options_draw(int w, int h) {
    static unsigned drawn = ~0u;
    static int drawnW, drawnH, drawnWhat, drawnUpdate, cw;
    int what = sOpen ? 2 : hle_pause_main_page() ? 1 : hle_file_select_shown() ? 3 : hle_password_screen_shown() ? 4 : 0;
    int y;
    if (sOpen && !shown(sFrom)) { /* what it was opened from went (the scene changed) */
        set_open(0);
        what = 0;
    }
    if (!what || w <= 0 || h <= 0 || !hle_font_metrics(&sCellW, &sCellH, &sBaseline) ||
        !hle_pause_image("PmPocketbookL01", NULL, &cw, &cw))
        return NULL;
    /* (The update check's answer comes in by itself.) */
    if (what == drawnWhat && drawn == sChanges && w == drawnW && h == drawnH && drawnUpdate == update_state() && sPx)
        return sPx;
    if (w != sW || h != sH || !sPx) {
        free(sPx);
        sPx = (uint32_t*)malloc((size_t)w * h * 4);
        if (!sPx) return NULL;
        sW = w;
        sH = h;
    }
    sDraws++;
    drawn = sChanges;
    drawnW = w;
    drawnH = h;
    drawnWhat = what;
    drawnUpdate = update_state();
    sScale = (float)w / REF_W < (float)h / REF_H ? (float)w / REF_W : (float)h / REF_H;
    sOffX = (w - REF_W * sScale) / 2;
    sOffY = (h - REF_H * sScale) / 2;
    memset(sPx, 0, (size_t)w * h * 4);
    if (what == 1) {
        draw_tab("Options", 1);
        return sPx;
    }
    if (what >= 3) {
        draw_file_button(what == 3 ? "Options" : "Unlock all");
        return sPx;
    }
    {
        /* The pages, drawn once for this size. */
        static uint32_t* pages;
        static int pagesW, pagesH, pagesFrom;
        if (!pages || pagesW != w || pagesH != h || pagesFrom != sFrom) {
            pagesFrom = sFrom;
            free(pages);
            draw_pages();
            pages = (uint32_t*)malloc((size_t)w * h * 4);
            if (pages) memcpy(pages, sPx, (size_t)w * h * 4);
            pagesW = w;
            pagesH = h;
        } else {
            memcpy(sPx, pages, (size_t)w * h * 4);
        }
    }
    draw_book();
    for (y = (int)Y(BAR_TOP); y < (int)ceilf(Y(BAR_BOTTOM)) && y < h; y++) {
        if (y >= 0) memset(sPx + (size_t)y * w, 0, (size_t)w * 4);
    }
    return sPx;
}

/* ------------------------------------------------------------------------ */
/* Input */

/* Whether keyboard key vk is bound to act. */
static int key_is(int vk, int act) {
    int k;
    for (k = 0; k < BIND_SLOTS; k++) {
        if (g_settings.key[act][k] == vk) return 1;
    }
    return 0;
}

static int pad_is(uint32_t pressed, int act) {
    int k;
    for (k = 0; k < BIND_SLOTS; k++) {
        if (g_settings.pad[act][k] && (pressed & g_settings.pad[act][k])) return 1;
    }
    return 0;
}

static int in(Rect r, float x, float y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

/* A click at window pixel (mx, my); returns 1 if it was on something. */
static int click(int mx, int my) {
    float x, y;
    int i;
    if (sScale <= 0) return 0;
    x = (mx - sOffX) / sScale;
    y = (my - sOffY) / sScale;
    if (sFrom == FROM_PAD && in(kTab, x, y)) {
        set_open(0);
        return 1;
    }
    for (i = 0; i < sections(); i++) {
        if (in(kButton[i], x, y)) {
            sSection = i;
            sOnRows = 0;
            sRow = sTop = sSlot = 0;
            changed();
            return 1;
        }
    }
    for (i = 0; i < VISIBLE_ROWS && sTop + i < row_count(sSection); i++) {
        Rect r = row_rect(i);
        Row row;
        if (!in(r, x, y)) continue;
        row = row_at(sSection, sTop + i);
        sOnRows = 1;
        if (sRow != sTop + i || row.kind == ROW_BINDING) {
            sRow = sTop + i;
            if (row.kind == ROW_BINDING) {
                sSlot = x >= (slot_x(0) + slot_x(1)) / 2;
                sCapture = sSection == SEC_CONTROLLER ? CAPTURE_PAD : CAPTURE_KEY;
            }
            changed();
        } else {
            select_();
        }
        return 1;
    }
    return 0;
}

#ifdef _WIN32
/* The key a WM_KEYDOWN means, with left and right modifiers told apart. */
static int key_of(WPARAM wp, LPARAM lp) {
    int extended = (int)((lp >> 24) & 1);
    if (wp == VK_SHIFT) return (int)MapVirtualKeyA((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    if (wp == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (wp == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return (int)wp;
}

int options_window_message(unsigned msg, uintptr_t wp, intptr_t lp) {
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, repeat = down && (lp & (1 << 30)), vk = 0;
    if (down) vk = key_of(wp, lp);
    if (msg == WM_LBUTTONDOWN) vk = VK_LBUTTON;
    if (msg == WM_RBUTTONDOWN) vk = VK_RBUTTON;
    if (msg == WM_MBUTTONDOWN) vk = VK_MBUTTON;
    if (msg == WM_XBUTTONDOWN) vk = HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    if (!sOpen) {
        /* Right, on the Pad's main page, opens the tab; on the save file
         * and password screens, their button (-) or a click on it. */
        if (down && !repeat && (vk == VK_RIGHT || key_is(vk, ACT_RIGHT)) && hle_pause_main_page()) {
            open_from(FROM_PAD);
            return 1;
        }
        if (!file_button_shown() || repeat) return 0;
        if ((down && key_is(vk, ACT_MINUS)) ||
            (msg == WM_LBUTTONDOWN && sScale > 0 &&
             in(kFileButton, ((short)LOWORD(lp) - sOffX) / sScale, ((short)HIWORD(lp) - sOffY) / sScale))) {
            file_button();
            return 1;
        }
        return 0;
    }
    if (sCapture != CAPTURE_NONE) {
        if (repeat) return 1;
        if (vk == VK_ESCAPE || vk == VK_BACK) {
            sCapture = CAPTURE_NONE;
            changed();
        } else if (vk && sCapture == CAPTURE_KEY) {
            bind(vk, 0);
        }
        return down || vk;
    }
    if (vk >= VK_LBUTTON && vk <= VK_XBUTTON2 && vk != VK_CANCEL && click((short)LOWORD(lp), (short)HIWORD(lp)))
        return 1;
    if (!vk) return 0;
    if (vk == VK_UP || key_is(vk, ACT_UP)) up_down(-1);
    else if (vk == VK_DOWN || key_is(vk, ACT_DOWN)) up_down(1);
    else if (vk == VK_LEFT || key_is(vk, ACT_LEFT)) left_right(-1);
    else if (vk == VK_RIGHT || key_is(vk, ACT_RIGHT)) left_right(1);
    else if (repeat) return 1;
    else if (vk == VK_RETURN || key_is(vk, ACT_2)) select_();
    else if (vk == VK_ESCAPE || vk == VK_BACK || key_is(vk, ACT_1)) back();
    else if (vk == VK_DELETE) clear();
    return 1;
}
#else
/* A tap at window pixel (x, y): on the tab or the save file screen's
 * button, it opens; open, it works as a click. Returns 1 if it was on
 * something. */
int options_touch(int x, int y) {
    float rx, ry;
    if (sScale <= 0) return sOpen;
    rx = (x - sOffX) / sScale;
    ry = (y - sOffY) / sScale;
    if (sOpen) {
        /* A touch off the book (the footer's Back among them) closes it:
         * a phone may have no button to. */
        if (!click(x, y) && !in(kLeftPage, rx, ry) && !in(kRightPage, rx, ry)) set_open(0);
        return 1;
    }
    if (hle_pause_main_page() && in(kTab, rx, ry)) {
        open_from(FROM_PAD);
        return 1;
    }
    if (file_button_shown() && in(kFileButton, rx, ry)) {
        file_button();
        return 1;
    }
    return 0;
}
#endif

void options_controller(uint32_t raw, uint32_t nav) {
    static uint32_t lastRaw, lastNav;
    uint32_t pressed = raw & ~lastRaw, navPressed = nav & ~lastNav;
    lastRaw = raw;
    lastNav = nav;
    if (!sOpen) {
        if ((navPressed & XPAD_RIGHT) && hle_pause_main_page()) open_from(FROM_PAD);
        else if (pad_is(pressed, ACT_MINUS) && file_button_shown()) file_button();
        return;
    }
    if (sCapture == CAPTURE_PAD) {
        if (pressed) bind(0, pressed & (0u - pressed)); /* the lowest one */
        return;
    }
    if (sCapture != CAPTURE_NONE) return;
    if (navPressed & XPAD_UP) up_down(-1);
    if (navPressed & XPAD_DOWN) up_down(1);
    if (navPressed & XPAD_LEFT) left_right(-1);
    if (navPressed & XPAD_RIGHT) left_right(1);
    if ((pressed & XPAD_A) || pad_is(pressed, ACT_2)) select_();
    else if ((pressed & XPAD_B) || pad_is(pressed, ACT_1)) back();
    else if (pressed & XPAD_Y) clear();
}

/* Wii Remote buttons held (HOST_PAD_* bits), from a script or the touch
 * controls, each with its own last and swallow. */
static int remote_input(uint32_t held, uint32_t* last, int* swallow) {
    uint32_t p = held & ~*last;
    *last = held;
    if (!held) *swallow = 0;
    if (!sOpen) {
        if ((p & HOST_PAD_SCREEN_RIGHT) && hle_pause_main_page()) open_from(FROM_PAD);
        else if ((p & HOST_PAD_MINUS) && file_button_shown()) file_button();
        if (sOpen) *swallow = 1;
        return *swallow; /* what the tab took stays its until let go */
    }
    *swallow = 1;
    if (p & HOST_PAD_SCREEN_UP) up_down(-1);
    if (p & HOST_PAD_SCREEN_DOWN) up_down(1);
    if (p & HOST_PAD_SCREEN_LEFT) left_right(-1);
    if (p & HOST_PAD_SCREEN_RIGHT) left_right(1);
    if (p & HOST_PAD_2) select_();
    if (p & HOST_PAD_1) back();
    if (p & HOST_PAD_MINUS) clear();
    return 1;
}

int options_script(uint32_t held) {
    static uint32_t last;
    static int swallow;
    return remote_input(held, &last, &swallow);
}

int options_touch_buttons(uint32_t held) {
    static uint32_t last;
    static int swallow;
    return remote_input(held, &last, &swallow);
}
