/* Button prompt icons (hle_prompts.c): which icon shows a Wii Remote
 * button, from what the player has bound to it on the device used last,
 * and the icon itself, a PNG from recomp/assets/prompts scaled to the
 * glyph it replaces. On Windows the PNGs are in a prompts folder beside
 * the program, decoded by WIC; on Android they are in the APK's assets,
 * decoded by AImageDecoder (window_android.c sets the asset manager). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#endif
#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/imagedecoder.h>
AAssetManager* g_android_assets;
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_input.h"
#include "host_settings.h"

#ifdef _WIN32
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

/* ------------------------------------------------------------------------ */
/* Which icon */

static const char* key_icon(int vk) {
#ifdef _WIN32
    static char name[32];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '1' && vk <= '9')) {
        snprintf(name, sizeof(name), "key_%c", (char)(vk >= 'A' ? vk - 'A' + 'a' : vk));
        return name;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(name, sizeof(name), "key_f%d", vk - VK_F1 + 1);
        return name;
    }
    switch (vk) {
    case VK_SPACE: return "key_space";
    case VK_RETURN: return "key_enter";
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return "key_ctrl";
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return "key_shift";
    case VK_TAB: return "key_tab";
    case VK_ESCAPE: return "key_esc";
    case VK_DELETE: return "key_delete";
    case VK_CAPITAL: return "key_capslock";
    case VK_OEM_MINUS: return "key_minus";
    case VK_OEM_PLUS: return "key_equal";
    case VK_ADD: return "key_plus";
    case VK_OEM_2: return "key_slash";
    case VK_OEM_PERIOD: return "key_period";
    case VK_OEM_COMMA: return "key_comma";
    case VK_UP: return "key_up";
    case VK_DOWN: return "key_down";
    case VK_LEFT: return "key_left";
    case VK_RIGHT: return "key_right";
    case VK_LBUTTON: return "mouse_left";
    case VK_RBUTTON: return "mouse_right";
    case VK_MBUTTON: return "mouse_middle";
    }
#endif
    (void)vk;
    return NULL;
}

/* A controller button's icon (b is where it is, as on an Xbox
 * controller), as the kind of controller (PAD_ICONS_*) labels the button
 * there: a PlayStation controller's shapes, a Nintendo one's letters (B at
 * the bottom) and shoulders. */
static const char* pad_icon(uint32_t b, int kind) {
    static const struct {
        uint32_t b;
        const char* name[3]; /* Xbox, PlayStation, Nintendo */
    } kIcons[] = {
        {XPAD_A, {"pad_a", "ps_cross", "pad_b"}},       {XPAD_B, {"pad_b", "ps_circle", "pad_a"}},
        {XPAD_X, {"pad_x", "ps_square", "pad_y"}},      {XPAD_Y, {"pad_y", "ps_triangle", "pad_x"}},
        {XPAD_LB, {"pad_lb", "ps_l1", "ns_l"}},         {XPAD_RB, {"pad_rb", "ps_r1", "ns_r"}},
        {XPAD_LT, {"pad_lt", "ps_l2", "ns_zl"}},        {XPAD_RT, {"pad_rt", "ps_r2", "ns_zr"}},
        {XPAD_START, {"pad_start", "ps_options", "ns_plus"}}, {XPAD_BACK, {"pad_back", "ps_create", "ns_minus"}},
        {XPAD_LTHUMB, {"pad_lstick", "pad_lstick", "pad_lstick"}}, {XPAD_RTHUMB, {"pad_rstick", "pad_rstick", "pad_rstick"}},
    };
    int i;
    /* The button the binding is on, with the Nintendo layout's swap. */
    b = settings_face_swap(b);
    for (i = 0; i < (int)(sizeof(kIcons) / sizeof(kIcons[0])); i++)
        if (kIcons[i].b == b) return kIcons[i].name[kind - PAD_ICONS_XBOX];
    return NULL;
}

/* The kind of controller whose icons show. */
static int pad_kind(void) { return g_settings.pad_icons == PAD_ICONS_AUTO ? input_pad_family() : g_settings.pad_icons; }

static const uint32_t kBit[BIND_COUNT] = {
    HOST_PAD_SCREEN_UP, HOST_PAD_SCREEN_DOWN, HOST_PAD_SCREEN_LEFT, HOST_PAD_SCREEN_RIGHT, HOST_PAD_2, HOST_PAD_1,
    HOST_PAD_A, HOST_PAD_B, HOST_PAD_PLUS, HOST_PAD_MINUS, HOST_PAD_HOME, HOST_PAD_SHAKE, HOST_PAD_SHAKE2,
    HOST_PAD_2, HOST_PAD_1, HOST_PAD_A, HOST_PAD_B, /* the menus' */
    HOST_PAD_2, HOST_PAD_1, HOST_PAD_A, HOST_PAD_B, /* the minigames' */
};

/* The icon for one button (HOST_PAD_*), in a layer (LAYER_*: the menus and
 * minigames bind 2, 1, A and B apart), or NULL to keep the game's glyph. */
static const char* icon_for(uint32_t button, int layer) {
    int a, k, pad = input_controller_last();
    int last = LAYER_END(layer);
    for (a = LAYER_FIRST(layer); a < last && kBit[a] != button; a++) {}
    if (a == last && layer != LAYER_PARK) { /* not one of the layer's own: the park's */
        for (a = 0; a < ACT_COUNT && kBit[a] != button; a++) {}
        last = ACT_COUNT;
    }
    if (a == last) return NULL;
    for (k = 0; k < BIND_SLOTS; k++) {
        const char* n = pad ? pad_icon(g_settings.pad[a][k], pad_kind()) : key_icon(g_settings.key[a][k]);
        if (n) return n;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* The icons themselves */

typedef struct {
    char name[32];
    int w, h, ok;
    uint8_t* alpha; /* w x h, top row first */
} Icon;

static Icon sIcons[128];
static int sIconCount;

#ifdef _WIN32
static int decode_png(const char* name, Icon* icon) {
    static IWICImagingFactory* factory;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    WCHAR wpath[MAX_PATH];
    char path[MAX_PATH], *slash;
    UINT w = 0, h = 0;
    uint8_t* bgra;
    int ok = 0, i;
    if (!factory) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                    (void**)&factory)))
            return 0;
    }
    GetModuleFileNameA(NULL, path, sizeof(path) - 64);
    slash = strrchr(path, '\\');
    snprintf(slash ? slash + 1 : path, 64, "prompts\\%s.png", name);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(factory, wpath, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && w && h && w <= 1024 && h <= 1024) {
        bgra = (uint8_t*)malloc((size_t)w * h * 4);
        if (bgra && SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, bgra))) {
            icon->alpha = (uint8_t*)malloc((size_t)w * h);
            for (i = 0; i < (int)(w * h); i++) icon->alpha[i] = bgra[4 * i + 3];
            icon->w = (int)w;
            icon->h = (int)h;
            ok = 1;
        }
        free(bgra);
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    return ok;
}
#elif defined(__ANDROID__)
static int decode_png(const char* name, Icon* icon) {
    char path[64];
    AAsset* a;
    AImageDecoder* dec = NULL;
    const AImageDecoderHeaderInfo* info;
    uint8_t* rgba = NULL;
    int ok = 0, w, h, i;
    snprintf(path, sizeof(path), "prompts/%s.png", name);
    if (!g_android_assets || !(a = AAssetManager_open(g_android_assets, path, AASSET_MODE_STREAMING))) return 0;
    if (AImageDecoder_createFromAAsset(a, &dec) == ANDROID_IMAGE_DECODER_SUCCESS &&
        AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888) == ANDROID_IMAGE_DECODER_SUCCESS &&
        AImageDecoder_setUnpremultipliedRequired(dec, 1) == ANDROID_IMAGE_DECODER_SUCCESS) {
        info = AImageDecoder_getHeaderInfo(dec);
        w = AImageDecoderHeaderInfo_getWidth(info);
        h = AImageDecoderHeaderInfo_getHeight(info);
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && (rgba = (uint8_t*)malloc((size_t)w * h * 4)) &&
            AImageDecoder_decodeImage(dec, rgba, (size_t)w * 4, (size_t)w * h * 4) == ANDROID_IMAGE_DECODER_SUCCESS) {
            icon->alpha = (uint8_t*)malloc((size_t)w * h);
            for (i = 0; i < w * h; i++) icon->alpha[i] = rgba[4 * i + 3];
            icon->w = w;
            icon->h = h;
            ok = 1;
        }
    }
    free(rgba);
    if (dec) AImageDecoder_delete(dec);
    AAsset_close(a);
    return ok;
}
#else
static int decode_png(const char* name, Icon* icon) {
    (void)name;
    (void)icon;
    return 0;
}
#endif

static Icon* load_icon(const char* name) {
    int i;
    for (i = 0; i < sIconCount; i++) {
        if (!strcmp(sIcons[i].name, name)) return sIcons[i].ok ? &sIcons[i] : NULL;
    }
    if (sIconCount == (int)(sizeof(sIcons) / sizeof(sIcons[0]))) return NULL;
    Icon* icon = &sIcons[sIconCount++];
    snprintf(icon->name, sizeof(icon->name), "%s", name);
    icon->ok = decode_png(name, icon);
    return icon->ok ? icon : NULL;
}

/* Draws icon as coverage (0-255) into the w x h box at (bx, by) of out
 * (stride wide): as large as fits at its own shape, centred, scaled down
 * by averaging. */
static void draw_icon(const Icon* icon, uint8_t* out, int stride, int bx, int by, int w, int h) {
    int x, y, dw, dh, ox, oy;
    dw = w;
    dh = icon->h * w / icon->w;
    if (dh > h) {
        dh = h;
        dw = icon->w * h / icon->h;
    }
    ox = bx + (w - dw) / 2;
    oy = by + (h - dh) / 2;
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            int x0 = x * icon->w / dw, x1 = (x + 1) * icon->w / dw, y0 = y * icon->h / dh, y1 = (y + 1) * icon->h / dh;
            int sx, sy, sum = 0, n = 0;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (sy = y0; sy < y1; sy++) {
                for (sx = x0; sx < x1; sx++) {
                    sum += icon->alpha[sy * icon->w + sx];
                    n++;
                }
            }
            out[(oy + y) * stride + ox + x] = (uint8_t)(sum / n);
        }
    }
}

/* The icon for button, as coverage (0-255) in a w x h box, top row first.
 * HOST_PAD_SCREEN_UP stands for the d-pad as a whole: on the controller
 * the left stick (which moves too), on the keyboard its four keys in a
 * cross, if each has an icon. Returns 0 to keep the game's glyph. */
static int prompt_icon(uint32_t button, int layer, uint8_t* out, int w, int h) {
    Icon* icon;
    memset(out, 0, (size_t)w * h);
    if (button == HOST_PAD_SCREEN_UP && !input_controller_last()) {
        static const int kCell[4][2] = {{1, 0}, {1, 2}, {0, 1}, {2, 1}}; /* up, down, left, right */
        Icon* arrows[4];
        int i;
        for (i = 0; i < 4; i++) {
            const char* name = icon_for(kBit[i], 0);
            if (!(arrows[i] = name ? load_icon(name) : NULL)) return 0;
        }
        for (i = 0; i < 4; i++) draw_icon(arrows[i], out, w, kCell[i][0] * w / 3, kCell[i][1] * h / 3, w / 3, h / 3);
        return 1;
    }
    {
        const char* name = button == HOST_PAD_SCREEN_UP ? "pad_lstick" : icon_for(button, layer);
        if (!(icon = name ? load_icon(name) : NULL)) return 0;
    }
    draw_icon(icon, out, w, 0, 0, w, h);
    return 1;
}

/* Changes whenever the icons to show may have: the bindings were saved,
 * the other device was used, or another kind of controller. */
static uint32_t prompt_generation(void) {
    return settings_generation() * 8 + (uint32_t)pad_kind() * 2 + (uint32_t)input_controller_last();
}

static void button_name(uint32_t button, char* out, int n) {
    int a;
    for (a = 0; a < ACT_COUNT && kBit[a] != button; a++) {}
    if (a == ACT_COUNT) {
        if (n) *out = 0;
        return;
    }
    settings_binding_name(a, input_controller_last(), out, (size_t)n);
}

void prompts_init(void) {
    g_host_prompt_icon = prompt_icon;
    g_host_prompt_generation = prompt_generation;
    g_host_button_name = button_name;
}
