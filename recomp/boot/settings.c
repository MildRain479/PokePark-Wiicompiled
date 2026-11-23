/* Settings: settings.ini, read at start and saved whenever one changes,
 * from the Options tab of the pause menu (options.c) and, on Windows, the
 * settings menu (menu_win32.c). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_settings.h"

void hle_set_frame_rate(int fps);   /* hle_game.c */
void gxb_set_output_rate(int fps);  /* gx_frames.c */

/* The frame rates offered. Above 60 the game still draws 60 frames a
 * second and the ones between are interpolated (gl_backend.c). */
static const int kFrameRates[] = {30, 60, 120, 144, 165, 240};
#define FRAME_RATES ((int)(sizeof(kFrameRates) / sizeof(kFrameRates[0])))

static int valid_frame_rate(int fps) {
    int i;
    for (i = FRAME_RATES - 1; i > 0 && kFrameRates[i] > fps; i--) {}
    return kFrameRates[i];
}

void hle_set_fov(int percent); /* hle_camera.c */
void hle_set_widescreen(int on); /* hle_nand.c */
static const char* const kAspectNames[ASPECTS] = {"4:3", "16:9", "16:10", "21:9"};
void hle_set_text_speed(int steps);  /* hle_modifiers.c */
void hle_set_berry_rate(int percent);
void hle_set_prices(int percent);
void hle_set_damage(int dealt, int taken);
void hle_set_chase_speed(int percent);
void hle_set_autosave(int on); /* hle_autosave.c */
void hle_set_sound_volumes(int music, int effects, int cries); /* hle_audio.c */

/* The gameplay settings' choices, in order. */
static const int kTextSpeeds[] = {1, 2, 3, 0};
static const int kBerries[] = {100, 150, 200, 300, 50};
static const int kPrices[] = {100, 50, 0, 200};
static const int kDamageDealt[] = {100, 150, 200, 300, 50};
static const int kDamageTaken[] = {100, 50, 0, 200, 300};
static const int kChaseSpeeds[] = {100, 75, 50, 125, 150};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int choice(const int* list, int n, int v) {
    int i;
    for (i = 0; i < n && list[i] != v; i++) {}
    return i < n ? i : 0;
}

static void apply_gameplay(void) {
    hle_set_text_speed(g_settings.text_speed);
    hle_set_berry_rate(g_settings.berries);
    hle_set_prices(g_settings.prices);
    hle_set_damage(g_settings.damage_dealt, g_settings.damage_taken);
    hle_set_chase_speed(g_settings.chase_speed);
    hle_set_autosave(g_settings.autosave);
}

static void apply_audio(void) {
    hle_set_sound_volumes(g_settings.music_volume, g_settings.effects_volume, g_settings.cries_volume);
}

static int volume_from(const char* p) {
    int v = (atoi(p) + 5) / 10 * 10;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

#define FOV_MIN 80
#define FOV_MAX 150

static void apply_frame_rate(void) {
    hle_set_frame_rate(g_settings.frame_rate);
    gxb_set_output_rate(g_settings.frame_rate);
}

HostSettings g_settings;
int g_language_started, g_cries_started;

/* ------------------------------------------------------------------------ */
/* Languages */

static const char* const kLanguageNames[LANGUAGES] = {"English", "Japanese", "French", "German", "Italian", "Spanish"};
static const char* const kLanguageIni[LANGUAGES] = {"english", "japanese", "french", "german", "italian", "spanish"};

const char* settings_language_name(int lang) { return lang >= 0 && lang < LANGUAGES ? kLanguageNames[lang] : ""; }

/* The pack's folder: languages/<name> beside the program (on Android in
 * the app's shared folder, beside R8AE01); RECOMP_LANGUAGES=<dir> in place
 * of languages. */
const char* settings_language_dir(int lang) {
    static char dir[1024];
    const char* e = getenv("RECOMP_LANGUAGES");
    if (e && *e) {
        snprintf(dir, sizeof(dir), "%s/%s", e, settings_language_name(lang));
    } else {
#ifdef _WIN32
        char* slash;
        GetModuleFileNameA(NULL, dir, sizeof(dir) - 64);
        slash = strrchr(dir, '\\');
        snprintf(slash ? slash + 1 : dir, 64, "languages/%s", settings_language_name(lang));
#else
        const char* shared = getenv("RECOMP_SHARED");
        snprintf(dir, sizeof(dir), "%s%slanguages/%s", shared ? shared : "", shared ? "/" : "",
                 settings_language_name(lang));
#endif
    }
    return dir;
}

static int language_from(const char* p) {
    int i;
    char word[16] = "";
    sscanf(p, " %15s", word);
    for (i = 0; i < LANGUAGES; i++) {
        if (!strcmp(word, kLanguageIni[i])) return i;
    }
    return LANGUAGE_EN;
}

static const char* const kActionIni[BIND_COUNT] = {
    "up", "down", "left", "right", "two", "one", "a", "b", "plus", "minus", "home", "shake", "shake2",
    "menu_two", "menu_one", "menu_a", "menu_b",
    "minigame_two", "minigame_one", "minigame_a", "minigame_b",
};

void settings_default_controls(void) {
#ifdef _WIN32
    /* The menus' and minigames' keys are the park's. */
    static const int kKeys[BIND_COUNT][BIND_SLOTS] = {
        {'W', VK_UP}, {'S', VK_DOWN}, {'A', VK_LEFT}, {'D', VK_RIGHT},
        {VK_SPACE, 'X'}, {VK_LBUTTON, 'Z'}, {VK_RBUTTON, VK_RETURN}, {'V', VK_LCONTROL},
        {'P', 0}, {'M', 0}, {VK_HOME, 'H'}, {'Q', VK_LSHIFT}, {'E', 0},
        {VK_SPACE, 'X'}, {VK_LBUTTON, 'Z'}, {VK_RBUTTON, VK_RETURN}, {'V', VK_LCONTROL},
        {VK_SPACE, 'X'}, {VK_LBUTTON, 'Z'}, {VK_RBUTTON, VK_RETURN}, {'V', VK_LCONTROL},
    };
#else
    static const int kKeys[BIND_COUNT][BIND_SLOTS]; /* no keyboard */
#endif
    /* In the menus B goes back (1), not X, the park's dash; in the
     * minigames RT shoots (A), and B too. */
    static const uint32_t kPad[BIND_COUNT][BIND_SLOTS] = {
        {XPAD_UP, 0}, {XPAD_DOWN, 0}, {XPAD_LEFT, 0}, {XPAD_RIGHT, 0},
        {XPAD_A, 0}, {XPAD_X, 0}, {XPAD_B, 0}, {XPAD_Y, XPAD_RT},
        {XPAD_START, 0}, {XPAD_BACK, 0}, {XPAD_RTHUMB, 0}, {XPAD_LB, 0}, {XPAD_RB, 0},
        {XPAD_A, 0}, {XPAD_B, 0}, {XPAD_X, 0}, {XPAD_Y, XPAD_RT},
        {XPAD_A, 0}, {XPAD_X, 0}, {XPAD_RT, XPAD_B}, {XPAD_Y, XPAD_LT},
    };
    memcpy(g_settings.key, kKeys, sizeof(kKeys));
    memcpy(g_settings.pad, kPad, sizeof(kPad));
}

static int valid_msaa(int n) { return n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; }
static int valid_aniso(int n) { return n >= 16 ? 16 : valid_msaa(n); }

/* ------------------------------------------------------------------------ */
/* Names */

void settings_key_name(int vk, char* out, size_t n) {
#ifdef _WIN32
    static const struct { int vk; const char* name; } kNames[] = {
        {VK_LBUTTON, "Left click"}, {VK_RBUTTON, "Right click"}, {VK_MBUTTON, "Middle click"},
        {VK_XBUTTON1, "Mouse 4"}, {VK_XBUTTON2, "Mouse 5"},
        {VK_LCONTROL, "Left Ctrl"}, {VK_RCONTROL, "Right Ctrl"}, {VK_LSHIFT, "Left Shift"},
        {VK_RSHIFT, "Right Shift"}, {VK_LMENU, "Left Alt"}, {VK_RMENU, "Right Alt"},
    };
    /* Keys whose scan code needs the extended bit to be told apart. */
    static const int kExtended[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                    VK_INSERT, VK_DELETE, VK_DIVIDE, VK_NUMLOCK};
    size_t i;
    LONG lp;
    if (!vk) {
        snprintf(out, n, "-");
        return;
    }
    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (kNames[i].vk == vk) {
            snprintf(out, n, "%s", kNames[i].name);
            return;
        }
    }
    lp = (LONG)(MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16);
    for (i = 0; i < sizeof(kExtended) / sizeof(kExtended[0]); i++) {
        if (kExtended[i] == vk) lp |= 1 << 24;
    }
    if (!GetKeyNameTextA(lp, out, (int)n)) snprintf(out, n, "Key %02X", vk);
#else
    if (vk) snprintf(out, n, "Key %02X", vk);
    else snprintf(out, n, "-");
#endif
}

const char* settings_pad_name(uint32_t b) {
    static const struct { uint32_t bit; const char* name; } kNames[] = {
        {XPAD_UP, "D-pad up"}, {XPAD_DOWN, "D-pad down"}, {XPAD_LEFT, "D-pad left"}, {XPAD_RIGHT, "D-pad right"},
        {XPAD_START, "Start"}, {XPAD_BACK, "Back"}, {XPAD_LTHUMB, "L stick click"}, {XPAD_RTHUMB, "R stick click"},
        {XPAD_LB, "LB"}, {XPAD_RB, "RB"}, {XPAD_A, "A"}, {XPAD_B, "B"}, {XPAD_X, "X"}, {XPAD_Y, "Y"},
        {XPAD_LT, "LT"}, {XPAD_RT, "RT"},
    };
    size_t i;
    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (kNames[i].bit == b) return kNames[i].name;
    }
    return "-";
}

/* A controller button's name as the controller in hand labels it (the
 * kind the icons show), the Nintendo layout's swap taken. */
const char* settings_pad_label(uint32_t b) {
    static const struct {
        uint32_t bit;
        const char* name[2]; /* PlayStation, Nintendo */
    } kOther[] = {
        {XPAD_A, {"Cross", "B"}}, {XPAD_B, {"Circle", "A"}}, {XPAD_X, {"Square", "Y"}}, {XPAD_Y, {"Triangle", "X"}},
        {XPAD_LB, {"L1", "L"}},   {XPAD_RB, {"R1", "R"}},    {XPAD_LT, {"L2", "ZL"}},  {XPAD_RT, {"R2", "ZR"}},
        {XPAD_START, {"Options", "+"}}, {XPAD_BACK, {"Create", "-"}},
    };
    int kind = g_settings.pad_icons == PAD_ICONS_AUTO ? input_pad_family() : g_settings.pad_icons;
    size_t i;
    b = settings_face_swap(b);
    if (kind != PAD_ICONS_XBOX) {
        for (i = 0; i < sizeof(kOther) / sizeof(kOther[0]); i++)
            if (kOther[i].bit == b) return kOther[i].name[kind - PAD_ICONS_PLAYSTATION];
    }
    return settings_pad_name(b);
}

void settings_binding_name(int act, int controller, char* out, size_t n) {
    int k;
    if (n) *out = 0;
    for (k = 0; k < BIND_SLOTS; k++) {
        if (controller && g_settings.pad[act][k]) {
            snprintf(out, n, "%s", settings_pad_label(g_settings.pad[act][k]));
            return;
        }
        if (!controller && g_settings.key[act][k]) {
            settings_key_name(g_settings.key[act][k], out, n);
            return;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* settings.ini */

static char sPath[1024];

static const char* settings_path(void) {
    if (!sPath[0]) {
        const char* e = getenv("RECOMP_SETTINGS");
        char* slash;
        if (e && *e) {
            snprintf(sPath, sizeof(sPath), "%s", e);
        } else {
#ifdef _WIN32
            GetModuleFileNameA(NULL, sPath, sizeof(sPath) - 16);
            slash = strrchr(sPath, '\\');
            strcpy(slash ? slash + 1 : sPath, "settings.ini");
#else
            (void)slash;
            strcpy(sPath, "settings.ini");
#endif
        }
    }
    return sPath;
}

static uint32_t sSaves;

uint32_t settings_generation(void) { return sSaves; }

void settings_save(void) {
    sSaves++;
    FILE* f = fopen(settings_path(), "w");
    int i;
    if (!f) return;
    fprintf(f, "; PokePark recomp settings, also changed in Options in the pause menu.\n\n");
    fprintf(f, "[display]\n");
#ifndef __ANDROID__
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
#endif
    fprintf(f, "scale = %d ; internal resolution: 0 fits the window, or 1-%d\n", g_settings.scale, MAX_SCALE);
    fprintf(f, "msaa = %d ; anti-aliasing samples: 1 (off), 2, 4 or 8\n", g_settings.msaa);
    fprintf(f, "frame_rate = %d ; 30 (the game's own), 60, or 120, 144, 165, 240 (interpolated)\n",
            g_settings.frame_rate);
    fprintf(f, "fov = %d ; field of view in the park, percent of the game's: %d-%d\n", g_settings.fov, FOV_MIN,
            FOV_MAX);
    fprintf(f, "aniso = %d ; anisotropic texture filtering: 1 (off), 2, 4, 8 or 16\n", g_settings.aniso);
    fprintf(f, "aspect = %s ; the picture's shape: 4:3 (the game's own, from the next start), 16:9, 16:10 or 21:9\n",
            kAspectNames[g_settings.aspect]);
    fprintf(f, "texture_pack = %d ; 1: Dolphin texture packs in the textures folder replace the game's textures\n",
            g_settings.texture_pack);
#ifdef __ANDROID__
    fprintf(f, "\n[dual_screen] ; only on a handheld with two screens; elsewhere all off\n");
    fprintf(f, "on = %d ; the second screen used, for the Pad's pages and the map\n", g_settings.dual_screen);
    fprintf(f, "swap = %d ; the game on the second screen, the Pad's pages on the main one\n", g_settings.swap_screens);
    fprintf(f, "minimap_top = %d ; the minimap on the game's screen too\n", g_settings.minimap_top);
    fprintf(f, "friends_list = %d ; the friends list beside the map; 0: the map alone\n\n", g_settings.friends_list);
#else
    fprintf(f, "renderer = %s ; opengl or vulkan, from the next start\n\n",
            g_settings.renderer == RENDERER_VULKAN ? "vulkan" : "opengl");
#endif
    fprintf(f, "[gameplay]\n");
    fprintf(f, "text_speed = %d ; dialogue: 1 (the game's), 2, 3 times as fast, or 0: a page at once\n",
            g_settings.text_speed);
    fprintf(f, "berries = %d ; berries collected, percent of the game's: 50, 100, 150, 200 or 300\n", g_settings.berries);
    fprintf(f, "prices = %d ; berries spent, percent of the game's: 0 (free), 50, 100 or 200\n", g_settings.prices);
    fprintf(f, "damage_dealt = %d ; damage Pikachu deals in battles, percent of the game's: 50, 100, 150, 200 or 300\n",
            g_settings.damage_dealt);
    fprintf(f, "damage_taken = %d ; damage Pikachu takes in battles, percent of the game's: 0 (none), 50, 100, 200 or 300\n",
            g_settings.damage_taken);
    fprintf(f, "chase_speed = %d ; the speed of the Pokemon Pikachu chases, percent of the game's: 50, 75, 100, 125 or 150\n",
            g_settings.chase_speed);
    fprintf(f, "autosave = %d ; 1: save the game on making a friend or entering an area\n\n", g_settings.autosave);
    fprintf(f, "[language]\n");
    fprintf(f, "text = %s ; english, or japanese, french, german, italian or spanish: a language pack in languages/<Name> (made from that disc), from the next start\n\n",
            kLanguageIni[g_settings.language]);
    fprintf(f, "[audio] ; volumes in percent: 0-100, in tens\n");
    fprintf(f, "music = %d\n", g_settings.music_volume);
    fprintf(f, "effects = %d ; the sounds that aren't music or cries\n", g_settings.effects_volume);
    fprintf(f, "cries_volume = %d ; the Pokemon's voices\n", g_settings.cries_volume);
    fprintf(f, "cries = %s ; the Pokemon's voices: english, or japanese, french or german (from the language pack), from the next start\n\n",
            kLanguageIni[g_settings.cries]);
    fprintf(f, "[updates]\n");
    fprintf(f, "check = %d ; 1: at start, ask GitHub whether a newer version is out\n\n", g_settings.check_updates);
#ifndef __ANDROID__ /* no mouse or keyboard */
    fprintf(f, "[mouse]\n");
    fprintf(f, "sensitivity = %.2f ; camera turn, times the default\n", g_settings.mouse_sensitivity);
    fprintf(f, "invert_tilt = %d\n", g_settings.invert_tilt);
    fprintf(f, "invert_turn = %d\n\n", g_settings.invert_turn);
    fprintf(f, "[keyboard] ; Windows virtual-key codes, two per button (0: none)\n");
    for (i = 0; i < BIND_COUNT; i++) {
        char a[32], b[32];
        settings_key_name(g_settings.key[i][0], a, sizeof(a));
        settings_key_name(g_settings.key[i][1], b, sizeof(b));
        fprintf(f, "%s = 0x%02X 0x%02X ; %s, %s\n", kActionIni[i], g_settings.key[i][0], g_settings.key[i][1], a, b);
    }
    fprintf(f, "\n");
#endif
    fprintf(f, "[controller] ; XInput buttons, two per button (0x10000: LT, 0x20000: RT)\n");
    fprintf(f, "rumble = %d ; 0 off, 1 light, 2 normal, 3 strong\n", g_settings.rumble);
    fprintf(f, "dead_zone = %d ; the sticks', percent of the way over\n", g_settings.stick_dead_zone);
    fprintf(f, "camera_speed = %.2f ; the right stick's camera turn, times the default\n", g_settings.stick_speed);
#ifdef __ANDROID__ /* (on Windows, under [mouse], for both) */
    fprintf(f, "invert_tilt = %d\n", g_settings.invert_tilt);
    fprintf(f, "invert_turn = %d\n", g_settings.invert_turn);
#endif
    fprintf(f, "icons = %d ; the button prompts: 0 as the controller is, 1 Xbox, 2 PlayStation, 3 Nintendo\n",
            g_settings.pad_icons);
    fprintf(f, "nintendo_layout = %d ; 1: A and B, X and Y swapped (the bindings below are by where the button is)\n",
            g_settings.nintendo_layout);
    fprintf(f, "gyro_aim = %d ; 1: turning the controller moves the minigames' pointer\n", g_settings.gyro_aim);
    fprintf(f, "gyro_speed = %.2f\n", g_settings.gyro_speed);
    fprintf(f, "gyro_steer = %d ; 1: tilting the controller steers in the flying, swimming and sliding minigames\n",
            g_settings.gyro_steer);
#ifdef __ANDROID__
    fprintf(f, "touch_controls = %d ; 1 shown while the touch screen was used last, 0 never\n",
            g_settings.touch_controls);
#endif
    for (i = 0; i < BIND_COUNT; i++) {
        fprintf(f, "%s = 0x%04X 0x%04X ; %s, %s\n", kActionIni[i], g_settings.pad[i][0], g_settings.pad[i][1],
                settings_pad_name(g_settings.pad[i][0]), settings_pad_name(g_settings.pad[i][1]));
    }
    fclose(f);
}

/* A speed, times the default: 0.1 to 3, in tenths. */
static float tenths_from(const char* p) {
    float s = (float)atof(p);
    return s < 0.1f ? 0.1f : s > 3.0f ? 3.0f : s;
}

static void tenths_step(float* v, int d) {
    int t = (int)(*v * 10 + 0.5f) + d;
    *v = (t < 1 ? 1 : t > 30 ? 30 : t) / 10.0f;
}

void settings_load(void) {
    char line[256], section[32] = "";
    int sawShake2 = 0, i; /* the second shake key's lines: 1 keyboard, 2 controller */
    int sawLayer = 0;     /* the menus' and minigames' keyboard lines (bits 1 << layer) */
    FILE* f;
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.msaa = 1;
    g_settings.aniso = 1;
    g_settings.renderer = RENDERER_VULKAN; /* OpenGL draws if it can't start */
    g_settings.aspect = ASPECT_16_9;
    g_settings.texture_pack = 1;
    g_settings.rumble = 2;
    g_settings.touch_controls = 1;
    g_settings.dual_screen = 1;
    g_settings.friends_list = 1;
    g_settings.text_speed = 1;
    g_settings.berries = 100;
    g_settings.prices = 100;
    g_settings.damage_dealt = 100;
    g_settings.damage_taken = 100;
    g_settings.chase_speed = 100;
    g_settings.frame_rate = 30;
    g_settings.fov = 100;
    g_settings.mouse_sensitivity = 1.0f;
    g_settings.stick_dead_zone = 24; /* about XInput's own suggestion */
    g_settings.stick_speed = 1.0f;
    g_settings.gyro_speed = 1.0f;
    g_settings.check_updates = 1;
    g_settings.music_volume = g_settings.effects_volume = g_settings.cries_volume = 100;
    settings_default_controls();
    f = fopen(settings_path(), "r");
    if (!f) {
        settings_save();
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *c, *end, name[32];
        int i;
        unsigned long a, b;
        if ((c = strchr(line, ';'))) *c = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            sscanf(p, "[%31[^]]", section);
            continue;
        }
        if (!(c = strchr(p, '='))) continue;
        *c = '\0';
        if (sscanf(p, "%31s", name) != 1) continue;
        p = c + 1;
        if (!strcmp(section, "dual_screen")) {
            if (!strcmp(name, "on")) g_settings.dual_screen = atoi(p) != 0;
            if (!strcmp(name, "swap")) g_settings.swap_screens = atoi(p) != 0;
            if (!strcmp(name, "minimap_top")) g_settings.minimap_top = atoi(p) != 0;
            if (!strcmp(name, "friends_list")) g_settings.friends_list = atoi(p) != 0;
        }
        if (!strcmp(section, "display")) {
            if (!strcmp(name, "fullscreen")) g_settings.fullscreen = atoi(p) != 0;
            if (!strcmp(name, "scale")) g_settings.scale = atoi(p) < 0 ? 0 : atoi(p) > MAX_SCALE ? MAX_SCALE : atoi(p);
            if (!strcmp(name, "msaa")) g_settings.msaa = valid_msaa(atoi(p));
            if (!strcmp(name, "aniso")) g_settings.aniso = valid_aniso(atoi(p));
            if (!strcmp(name, "renderer")) g_settings.renderer = strstr(p, "vulkan") ? RENDERER_VULKAN : RENDERER_OPENGL;
            if (!strcmp(name, "texture_pack")) g_settings.texture_pack = atoi(p) != 0;
            if (!strcmp(name, "aspect")) {
                char shape[8] = "";
                sscanf(p, " %7s", shape);
                for (i = 0; i < ASPECTS && strcmp(shape, kAspectNames[i]); i++) {}
                if (i < ASPECTS) g_settings.aspect = i;
            }
            if (!strcmp(name, "frame_rate")) g_settings.frame_rate = valid_frame_rate(atoi(p));
            if (!strcmp(name, "minimap_top")) g_settings.minimap_top = atoi(p) != 0; /* (where it was) */
            if (!strcmp(name, "fov")) {
                int v = atoi(p) / 5 * 5;
                g_settings.fov = v < FOV_MIN ? FOV_MIN : v > FOV_MAX ? FOV_MAX : v;
            }
        } else if (!strcmp(section, "gameplay")) {
            if (!strcmp(name, "text_speed")) g_settings.text_speed = kTextSpeeds[choice(kTextSpeeds, COUNT(kTextSpeeds), atoi(p))];
            if (!strcmp(name, "berries")) g_settings.berries = kBerries[choice(kBerries, COUNT(kBerries), atoi(p))];
            if (!strcmp(name, "prices")) g_settings.prices = kPrices[choice(kPrices, COUNT(kPrices), atoi(p))];
            if (!strcmp(name, "damage_dealt"))
                g_settings.damage_dealt = kDamageDealt[choice(kDamageDealt, COUNT(kDamageDealt), atoi(p))];
            if (!strcmp(name, "damage_taken"))
                g_settings.damage_taken = kDamageTaken[choice(kDamageTaken, COUNT(kDamageTaken), atoi(p))];
            if (!strcmp(name, "chase_speed"))
                g_settings.chase_speed = kChaseSpeeds[choice(kChaseSpeeds, COUNT(kChaseSpeeds), atoi(p))];
            if (!strcmp(name, "autosave")) g_settings.autosave = atoi(p) != 0;
        } else if (!strcmp(section, "mouse")) {
            if (!strcmp(name, "sensitivity")) {
                float s = (float)atof(p);
                g_settings.mouse_sensitivity = s < 0.1f ? 0.1f : s > 3.0f ? 3.0f : s;
            }
            if (!strcmp(name, "invert_tilt")) g_settings.invert_tilt = atoi(p) != 0;
            if (!strcmp(name, "invert_turn")) g_settings.invert_turn = atoi(p) != 0;
        } else if (!strcmp(section, "language") && !strcmp(name, "text")) {
            g_settings.language = language_from(p);
        } else if (!strcmp(section, "audio")) {
            if (!strcmp(name, "cries")) g_settings.cries = language_from(p);
            if (!strcmp(name, "music")) g_settings.music_volume = volume_from(p);
            if (!strcmp(name, "effects")) g_settings.effects_volume = volume_from(p);
            if (!strcmp(name, "cries_volume")) g_settings.cries_volume = volume_from(p);
        } else if (!strcmp(section, "updates") && !strcmp(name, "check")) {
            g_settings.check_updates = atoi(p) != 0;
        } else if (!strcmp(section, "controller") && !strcmp(name, "rumble")) {
            g_settings.rumble = atoi(p) < 0 ? 0 : atoi(p) > 3 ? 3 : atoi(p);
        } else if (!strcmp(section, "controller") && !strcmp(name, "touch_controls")) {
            g_settings.touch_controls = atoi(p) != 0;
        } else if (!strcmp(section, "controller") && !strcmp(name, "dead_zone")) {
            g_settings.stick_dead_zone = atoi(p) < 0 ? 0 : atoi(p) > 50 ? 50 : atoi(p);
        } else if (!strcmp(section, "controller") && !strcmp(name, "camera_speed")) {
            g_settings.stick_speed = tenths_from(p);
        } else if (!strcmp(section, "controller") && !strcmp(name, "gyro_speed")) {
            g_settings.gyro_speed = tenths_from(p);
        } else if (!strcmp(section, "controller") && !strcmp(name, "icons")) {
            g_settings.pad_icons = atoi(p) < 0 || atoi(p) >= PAD_ICONS ? 0 : atoi(p);
        } else if (!strcmp(section, "controller") && !strcmp(name, "nintendo_layout")) {
            g_settings.nintendo_layout = atoi(p) != 0;
        } else if (!strcmp(section, "controller") && !strcmp(name, "gyro_aim")) {
            g_settings.gyro_aim = atoi(p) != 0;
        } else if (!strcmp(section, "controller") && !strcmp(name, "gyro_steer")) {
            g_settings.gyro_steer = atoi(p) != 0;
        } else if (!strcmp(section, "controller") && !strcmp(name, "invert_tilt")) {
            g_settings.invert_tilt = atoi(p) != 0;
        } else if (!strcmp(section, "controller") && !strcmp(name, "invert_turn")) {
            g_settings.invert_turn = atoi(p) != 0;
        } else if (!strcmp(section, "keyboard") || !strcmp(section, "controller")) {
            for (i = 0; i < BIND_COUNT; i++) {
                if (strcmp(name, kActionIni[i])) continue;
                if (i == ACT_SHAKE2) sawShake2 |= section[0] == 'k' ? 1 : 2;
                if (i >= ACT_COUNT && section[0] == 'k') sawLayer |= 1 << ACT_LAYER(i);
                a = strtoul(p, &end, 0);
                b = strtoul(end, NULL, 0);
                if (section[0] == 'k') {
                    g_settings.key[i][0] = (int)(a & 0xFF);
                    g_settings.key[i][1] = (int)(b & 0xFF);
                } else {
                    g_settings.pad[i][0] = (uint32_t)a;
                    g_settings.pad[i][1] = (uint32_t)b;
                }
            }
        }
    }
    fclose(f);
    /* Files from before the second shake key gave its default (RB) to
     * shake: it moves over. */
    if (!(sawShake2 & 2)) {
        for (i = 0; i < ACT_COUNT; i++) {
            int k;
            for (k = 0; k < BIND_SLOTS; k++) {
                if (i != ACT_SHAKE2 && g_settings.pad[i][k] == g_settings.pad[ACT_SHAKE2][0]) g_settings.pad[i][k] = 0;
            }
        }
    }
    if (!(sawShake2 & 1)) {
        for (i = 0; i < ACT_COUNT; i++) {
            int k;
            for (k = 0; k < BIND_SLOTS; k++) {
                if (i != ACT_SHAKE2 && g_settings.key[i][k] == g_settings.key[ACT_SHAKE2][0]) g_settings.key[i][k] = 0;
            }
        }
    }
    /* Files from before the menus and minigames had keys of their own:
     * theirs are the park's, as they were. */
    for (i = LAYER_MENUS; i < LAYERS; i++) {
        int k;
        if (sawLayer & (1 << i)) continue;
        for (k = 0; k < LAYER_ACTS; k++)
            memcpy(g_settings.key[LAYER_FIRST(i) + k], g_settings.key[ACT_2 + k], sizeof(g_settings.key[0]));
    }
    apply_frame_rate();
    hle_set_fov(g_settings.fov);
    hle_set_widescreen(g_settings.aspect != ASPECT_4_3);
    apply_gameplay();
    apply_audio();
}

void settings_step(int which, int d) {
    static const int kMsaa[] = {1, 2, 4, 8};
    int i;
    switch (which) {
    case SET_FULLSCREEN: g_settings.fullscreen = !g_settings.fullscreen; break;
    case SET_SCALE: g_settings.scale = (g_settings.scale + d + MAX_SCALE + 1) % (MAX_SCALE + 1); break;
    case SET_MSAA:
        for (i = 0; i < 4 && kMsaa[i] != g_settings.msaa; i++) {}
        g_settings.msaa = kMsaa[(i + d + 4) % 4];
        break;
    case SET_FRAME_RATE:
        for (i = 0; i < FRAME_RATES && kFrameRates[i] != g_settings.frame_rate; i++) {}
        g_settings.frame_rate = kFrameRates[(i + d + FRAME_RATES) % FRAME_RATES];
        apply_frame_rate();
        break;
    case SET_FOV:
        g_settings.fov = g_settings.fov + 5 * d < FOV_MIN ? FOV_MIN : g_settings.fov + 5 * d > FOV_MAX ? FOV_MAX : g_settings.fov + 5 * d;
        hle_set_fov(g_settings.fov);
        break;
    case SET_SENSITIVITY: {
        int tenths = (int)(g_settings.mouse_sensitivity * 10 + 0.5f) + d;
        g_settings.mouse_sensitivity = (tenths < 1 ? 1 : tenths > 30 ? 30 : tenths) / 10.0f;
        break;
    }
    case SET_INVERT: g_settings.invert_tilt = !g_settings.invert_tilt; break;
    case SET_INVERT_TURN: g_settings.invert_turn = !g_settings.invert_turn; break;
    case SET_DEAD_ZONE:
        g_settings.stick_dead_zone += 2 * d;
        g_settings.stick_dead_zone = g_settings.stick_dead_zone < 0 ? 0 : g_settings.stick_dead_zone > 50 ? 50 : g_settings.stick_dead_zone;
        break;
    case SET_STICK_SPEED: tenths_step(&g_settings.stick_speed, d); break;
    case SET_GYRO_SPEED: tenths_step(&g_settings.gyro_speed, d); break;
    case SET_GYRO_AIM: g_settings.gyro_aim = !g_settings.gyro_aim; break;
    case SET_PAD_ICONS: g_settings.pad_icons = (g_settings.pad_icons + d + PAD_ICONS) % PAD_ICONS; break;
    case SET_NINTENDO_LAYOUT: g_settings.nintendo_layout = !g_settings.nintendo_layout; break;
    case SET_GYRO_STEER: g_settings.gyro_steer = !g_settings.gyro_steer; break;
    case SET_UPDATES:
        g_settings.check_updates = !g_settings.check_updates;
        update_check();
        break;
    case SET_ANISO: {
        static const int kAniso[] = {1, 2, 4, 8, 16};
        for (i = 0; i < 5 && kAniso[i] != g_settings.aniso; i++) {}
        g_settings.aniso = kAniso[(i + d + 5) % 5];
        break;
    }
    case SET_RENDERER: g_settings.renderer = !g_settings.renderer; break;
    case SET_ASPECT: g_settings.aspect = (g_settings.aspect + d + ASPECTS) % ASPECTS; break;
    case SET_TEXTURE_PACK: g_settings.texture_pack = !g_settings.texture_pack; break;
    case SET_AUTOSAVE:
        g_settings.autosave = !g_settings.autosave;
        apply_gameplay();
        break;
    case SET_LANGUAGE: g_settings.language = (g_settings.language + d + LANGUAGES) % LANGUAGES; break;
    case SET_CRIES: g_settings.cries = (g_settings.cries + d + LANGUAGES) % LANGUAGES; break;
    case SET_MUSIC_VOLUME:
    case SET_EFFECTS_VOLUME:
    case SET_CRIES_VOLUME: {
        int* v = which == SET_MUSIC_VOLUME     ? &g_settings.music_volume
                 : which == SET_EFFECTS_VOLUME ? &g_settings.effects_volume
                                               : &g_settings.cries_volume;
        *v = *v + 10 * d < 0 ? 0 : *v + 10 * d > 100 ? 100 : *v + 10 * d;
        apply_audio();
        break;
    }
    case SET_TOUCH: g_settings.touch_controls = !g_settings.touch_controls; break;
    case SET_MINIMAP_TOP: g_settings.minimap_top = !g_settings.minimap_top; break;
    case SET_DUAL_SCREEN: g_settings.dual_screen = !g_settings.dual_screen; break;
    case SET_SWAP_SCREENS: g_settings.swap_screens = !g_settings.swap_screens; break;
    case SET_FRIENDS_LIST: g_settings.friends_list = !g_settings.friends_list; break;
    case SET_RUMBLE:
        g_settings.rumble = (g_settings.rumble + d + 4) % 4;
        input_rumble_preview();
        break;
    case SET_TEXT_SPEED:
        g_settings.text_speed = kTextSpeeds[(choice(kTextSpeeds, COUNT(kTextSpeeds), g_settings.text_speed) + d +
                                             COUNT(kTextSpeeds)) % COUNT(kTextSpeeds)];
        apply_gameplay();
        break;
    case SET_BERRIES:
        g_settings.berries =
            kBerries[(choice(kBerries, COUNT(kBerries), g_settings.berries) + d + COUNT(kBerries)) % COUNT(kBerries)];
        apply_gameplay();
        break;
    case SET_PRICES:
        g_settings.prices =
            kPrices[(choice(kPrices, COUNT(kPrices), g_settings.prices) + d + COUNT(kPrices)) % COUNT(kPrices)];
        apply_gameplay();
        break;
    case SET_DAMAGE_DEALT:
        g_settings.damage_dealt = kDamageDealt[(choice(kDamageDealt, COUNT(kDamageDealt), g_settings.damage_dealt) + d +
                                                COUNT(kDamageDealt)) % COUNT(kDamageDealt)];
        apply_gameplay();
        break;
    case SET_DAMAGE_TAKEN:
        g_settings.damage_taken = kDamageTaken[(choice(kDamageTaken, COUNT(kDamageTaken), g_settings.damage_taken) + d +
                                                COUNT(kDamageTaken)) % COUNT(kDamageTaken)];
        apply_gameplay();
        break;
    case SET_CHASE_SPEED:
        g_settings.chase_speed = kChaseSpeeds[(choice(kChaseSpeeds, COUNT(kChaseSpeeds), g_settings.chase_speed) + d +
                                               COUNT(kChaseSpeeds)) % COUNT(kChaseSpeeds)];
        apply_gameplay();
        break;
    default: return;
    }
    settings_save();
}

void settings_bind(int act, int slot, int vk, uint32_t pad) {
    int i, k, layer = ACT_LAYER(act);
    for (i = LAYER_FIRST(layer); i < LAYER_END(layer); i++) {
        for (k = 0; k < BIND_SLOTS; k++) {
            if (vk && g_settings.key[i][k] == vk) g_settings.key[i][k] = 0;
            if (pad && g_settings.pad[i][k] == pad) g_settings.pad[i][k] = 0;
        }
    }
    if (vk) g_settings.key[act][slot] = vk;
    else g_settings.pad[act][slot] = pad;
    settings_save();
}
