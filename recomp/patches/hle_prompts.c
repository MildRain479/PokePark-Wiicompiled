/* Button prompts. The game's text shows a Wii Remote button with a tag
 * (0x1A, its size, group 5, then the button) that draws a character of
 * its font: U+E040 + n, the Wii's button glyphs (power, the d-pad, A, B,
 * HOME, +, -, 1, 2). Here those glyphs are redrawn, in the font itself,
 * as icons of what the player has bound to each button, on the keyboard
 * and mouse or the controller, whichever was used last (host_input.h);
 * a button with no icon keeps the game's glyph. They are redrawn when the
 * font is loaded and whenever the bindings or the device change; text
 * drawn after that shows them.
 *
 * A font (RFNT) is a header and blocks; its glyphs are cells of 4-bit
 * sheets (TGLP: cell size, baseline, sheet layout, and once loaded a
 * pointer to the sheets), found through its character maps (CMAP). */
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host_input.h"

int (*g_host_prompt_icon)(uint32_t button, int layer, uint8_t* coverage, int w, int h);
uint32_t (*g_host_prompt_generation)(void);

#define FIRST_ICON 0xE041 /* the d-pad; then A, B, HOME, +, -, 1, 2 */
#define ICONS 8
static const uint32_t kButton[ICONS] = {
    HOST_PAD_SCREEN_UP, HOST_PAD_A, HOST_PAD_B, HOST_PAD_HOME, HOST_PAD_PLUS, HOST_PAD_MINUS, HOST_PAD_1, HOST_PAD_2,
};

#define MAX_FONTS 4
#define MAX_CELL (64 * 64)

typedef struct {
    uint32_t font, sheets;
    int cw, ch, baseline, sheetSize, perRow, perSheet, sheetWidth, fmt;
    int glyph[ICONS];          /* glyph index, or -1 */
    int width[ICONS];          /* how much of its cell is drawn, from the left */
    uint8_t* original[ICONS];  /* the game's pixels, 0-15 per pixel, cw x ch */
} Font;

static Font sFonts[MAX_FONTS];
static int sFontCount;
static uint32_t sGeneration = 0xFFFFFFFFu;
/* The glyphs' layer (host_input.h's LAYER_*): the minigames' in their
 * lobbies and the minigames themselves, where the instructions are; the
 * park's elsewhere (dialogue, signs). */
static int sGlyphLayer;

/* A pixel of a 4-bit (I4) sheet: 8x8 tiles of 32 bytes, row by row. */
static uint32_t i4_byte(const Font* f, int sheet, int x, int y) {
    int tiles = f->sheetWidth / 8;
    return f->sheets + (uint32_t)sheet * f->sheetSize + (uint32_t)(((y / 8) * tiles + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2);
}

static int get_px(const Font* f, int sheet, int x, int y) {
    uint8_t b = LD8(i4_byte(f, sheet, x, y));
    return x % 2 ? b & 15 : b >> 4;
}

static void set_px(const Font* f, int sheet, int x, int y, int v) {
    uint32_t a = i4_byte(f, sheet, x, y);
    uint8_t b = LD8(a);
    ST8(a, (uint8_t)(x % 2 ? (b & 0xF0) | v : (b & 0x0F) | (v << 4)));
}

/* Where a glyph's cell is: its sheet and top-left pixel. */
static void cell_of(const Font* f, int glyph, int* sheet, int* x0, int* y0) {
    int k = glyph % f->perSheet;
    *sheet = glyph / f->perSheet;
    *x0 = (k % f->perRow) * (f->cw + 1);
    *y0 = (k / f->perRow) * (f->ch + 1);
}

/* The glyph a character maps to in font data at base (block by block). */
static int glyph_of(uint32_t base, uint32_t size, uint16_t code) {
    uint32_t pos = base + LD16(base + 0xC), end = base + size;
    while (pos + 8 <= end) {
        uint32_t blockSize = LD32(pos + 4);
        if (LD32(pos) == 0x434D4150) { /* CMAP */
            uint16_t lo = LD16(pos + 8), hi = LD16(pos + 10), method = LD16(pos + 12);
            if (code >= lo && code <= hi) {
                if (method == 0) return LD16(pos + 0x14) + (code - lo); /* direct */
                if (method == 1) return (int16_t)LD16(pos + 0x14 + 2 * (code - lo)) == -1 ? -1 : LD16(pos + 0x14 + 2 * (code - lo));
                if (method == 2) { /* scan */
                    int n = LD16(pos + 0x14), i;
                    for (i = 0; i < n; i++) {
                        if (LD16(pos + 0x16 + 4 * i) == code) return LD16(pos + 0x18 + 4 * i);
                    }
                }
            }
        }
        if (blockSize < 8) break;
        pos += blockSize;
    }
    return -1;
}

static void draw_icons(Font* f) {
    static uint8_t cov[MAX_CELL];
    int i, x, y;
    for (i = 0; i < ICONS; i++) {
        int sheet, x0, y0, s, ox, oy, drew = 0;
        if (f->glyph[i] < 0) continue;
        cell_of(f, f->glyph[i], &sheet, &x0, &y0);
        if (!f->original[i]) {
            f->original[i] = (uint8_t*)malloc((size_t)f->cw * f->ch);
            for (y = 0; y < f->ch; y++) {
                for (x = 0; x < f->cw; x++) f->original[i][y * f->cw + x] = (uint8_t)get_px(f, sheet, x0 + x, y0 + y);
            }
        }
        /* Square, up to the baseline, as the Wii's glyphs sit, and within
         * the part of the cell that's drawn (its glyph width, 35 of 40
         * columns in the game's font): wide icons, like the space bar's,
         * would lose their right edge past it. */
        s = f->baseline < f->width[i] ? f->baseline : f->width[i];
        s -= 2;
        ox = (f->width[i] - s) / 2;
        oy = f->baseline - s - 1;
        if (oy < 0) oy = 0;
        if (s > 0 && s * s <= MAX_CELL && g_host_prompt_icon) drew = g_host_prompt_icon(kButton[i], sGlyphLayer, cov, s, s);
        for (y = 0; y < f->ch; y++) {
            for (x = 0; x < f->cw; x++) {
                int v = f->original[i][y * f->cw + x];
                if (drew) {
                    int ix = x - ox, iy = y - oy;
                    v = ix >= 0 && iy >= 0 && ix < s && iy < s ? (cov[iy * s + ix] * 15 + 127) / 255 : 0;
                }
                set_px(f, sheet, x0 + x, y0 + y, v);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The game's text font, for menus drawn over the picture (menu_win32.c):
 * the first font loaded with the button glyphs, the one messages use. */

/* A glyph's widths (CWDH): where its pixels start, how many columns of its
 * cell are drawn, and how far the pen moves; 0 if it has none. */
static int glyph_widths(const Font* f, int glyph, int* left, int* width, int* advance) {
    uint32_t size = LD32(f->font + 8), pos;
    for (pos = f->font + LD16(f->font + 0xC); pos + 8 <= f->font + size; pos += LD32(pos + 4)) {
        if (LD32(pos + 4) < 8) break;
        if (LD32(pos) == 0x43574448) { /* CWDH */
            int first = LD16(pos + 8), last = LD16(pos + 10);
            if (glyph >= first && glyph <= last) {
                uint32_t e = pos + 0x10 + 3 * (uint32_t)(glyph - first);
                *left = (int8_t)LD8(e);
                *width = LD8(e + 1);
                *advance = LD8(e + 2);
                return 1;
            }
        }
    }
    return 0;
}

int hle_font_metrics(int* cellW, int* cellH, int* baseline) {
    if (!sFontCount) return 0;
    *cellW = sFonts[0].cw;
    *cellH = sFonts[0].ch;
    *baseline = sFonts[0].baseline;
    return 1;
}

int hle_font_glyph(uint16_t code, uint8_t* cov, int* left, int* advance) {
    const Font* f = &sFonts[0];
    int glyph, sheet, x0, y0, x, y, width;
    if (!sFontCount || (glyph = glyph_of(f->font, LD32(f->font + 8), code)) < 0 ||
        !glyph_widths(f, glyph, left, &width, advance))
        return 0;
    cell_of(f, glyph, &sheet, &x0, &y0);
    for (y = 0; y < f->ch; y++) {
        for (x = 0; x < f->cw; x++) cov[y * f->cw + x] = (uint8_t)(get_px(f, sheet, x0 + x, y0 + y) * 17);
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Buttons in pictures. The menus' button bar ("+ Skip", "1 Back", "2
 * Next") draws its buttons from a texture named KeyButton (256x256, 8-bit
 * colour indices into RGB5A3 colours) in each menu's model (lobbies, start
 * menu, pause, photos); the bubble over Pikachu's head ("2 Talk", "2 Pick
 * Up", "2 Check") from FukidashiMini (256x128) in the field menu's; the
 * title screen's "Please Press 2" is TitleParts (512x128). Each button
 * there is redrawn as the icon for what is bound to it, in the
 * colours the texture has: a white key, outlined in the button's own
 * colour (the bar's, a coloured ring) or dark grey (the others, all
 * greys), with a white edge, as the words beside it are drawn. */

#define MAX_COLOURS 256
#define MAX_PICTURES 8
#define EDGE 2 /* the white edge around an icon */

typedef struct {
    int x0, y0, x1, y1; /* inclusive */
    uint32_t button;
} Spot;

static const Spot kBarSpots[] = {
    {2, 2, 38, 37, HOST_PAD_1},       {2, 42, 38, 77, HOST_PAD_2},      {1, 82, 37, 117, HOST_PAD_PLUS},
    {1, 122, 37, 157, HOST_PAD_MINUS}, {3, 164, 36, 195, HOST_PAD_B},   {2, 202, 37, 237, HOST_PAD_A},
    {42, 202, 77, 237, HOST_PAD_SCREEN_UP}, /* the d-pad */
};
static const Spot kBubbleSpots[] = {
    {10, 39, 36, 67, HOST_PAD_2}, {18, 69, 44, 97, HOST_PAD_2}, {9, 99, 35, 127, HOST_PAD_2}, /* with their glow */
};
static const Spot kTitleSpots[] = {
    {296, 2, 331, 37, HOST_PAD_2},
};

static const struct Kind {
    const char* name;
    int w, h;
    const Spot* spots;
    int spotCount;
    uint32_t line; /* the key's outline, 0xRRGGBB, or ~0u for the button's colour */
    int layer;     /* whose buttons it shows (LAYER_*): the menus' (the bar, the title), the park's (talk) */
} kKinds[] = {
    {"KeyButton", 256, 256, kBarSpots, (int)(sizeof(kBarSpots) / sizeof(kBarSpots[0])), ~0u, 1},
    {"FukidashiMini", 256, 128, kBubbleSpots, (int)(sizeof(kBubbleSpots) / sizeof(kBubbleSpots[0])), 0x414141, 0},
    {"TitleParts", 512, 128, kTitleSpots, (int)(sizeof(kTitleSpots) / sizeof(kTitleSpots[0])), 0x414141, 1},
};
#define KINDS ((int)(sizeof(kKinds) / sizeof(kKinds[0])))

typedef struct {
    const struct Kind* kind;
    uint32_t tex, image;
    int colours;
    uint8_t rgba[MAX_COLOURS][4];
    uint8_t* original; /* the game's indices, w x h */
} Picture;

static Picture sPictures[MAX_PICTURES];
static int sPictureCount;

/* A texel of an 8-bit (CI8) texture w wide: 8x4 tiles of 32 bytes, row by row. */
static uint32_t ci8_byte(uint32_t image, int w, int x, int y) {
    return image + (uint32_t)(((y / 4) * (w / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8);
}

static int name_is(uint32_t at, const char* s) {
    for (; *s; s++, at++) {
        if (LD8(at) != (uint8_t)*s) return 0;
    }
    return LD8(at) == 0;
}

/* Which kind of picture the TEX0 at tex is, if any (its memory may have
 * been freed and reused since). */
static const struct Kind* kind_of(uint32_t tex) {
    int i;
    if (LD32(tex) != 0x54455830 || LD32(tex + 0x20) != 9) return NULL; /* TEX0, CI8 */
    for (i = 0; i < KINDS; i++) {
        if (name_is(tex + LD32(tex + 0x14), kKinds[i].name) && LD16(tex + 0x1C) == kKinds[i].w &&
            LD16(tex + 0x1E) == kKinds[i].h)
            return &kKinds[i];
    }
    return NULL;
}

/* The colour nearest r g b a, weighing its colour by how opaque it is. */
static int nearest(const Picture* b, int r, int g, int bl, int a) {
    int i, best = 0;
    long bestD = -1;
    for (i = 0; i < b->colours; i++) {
        const uint8_t* p = b->rgba[i];
        long da = (long)(p[3] - a) * 2, dr = p[0] - r, dg = p[1] - g, db = p[2] - bl;
        long d = da * da + (dr * dr + dg * dg + db * db) * (a + p[3]) / 510;
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

static void draw_spot(Picture* b, const Spot* spot) {
    enum { MAX_BOX = 40 };
    static uint8_t cov[MAX_BOX * MAX_BOX], inside[MAX_BOX * MAX_BOX];
    static int stack[MAX_BOX * MAX_BOX];
    int W = b->kind->w, x0 = spot->x0, y0 = spot->y0;
    int w = spot->x1 - x0 + 1, h = spot->y1 - y0 + 1;
    int s = (w < h ? w : h) - 2 * EDGE, ox = (w - s) / 2, oy = (h - s) / 2;
    int x, y, i, n = 0, transparent, count[MAX_COLOURS] = {0};
    uint8_t line[3];
    if (s <= 0 || w > MAX_BOX || h > MAX_BOX) return;
    /* On the keyboard the d-pad would be four keys too small to read here:
     * the game's cross, which says "directions" as well, stays. (The
     * generation's low bit: the controller was used last.) */
    if (!g_host_prompt_icon || (spot->button == HOST_PAD_SCREEN_UP && !(sGeneration & 1)) ||
        !g_host_prompt_icon(spot->button, b->kind->layer, cov, s, s)) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) ST8(ci8_byte(b->image, W, x0 + x, y0 + y), b->original[(y0 + y) * W + x0 + x]);
        }
        return;
    }
    if (b->kind->line != ~0u) {
        line[0] = (uint8_t)(b->kind->line >> 16);
        line[1] = (uint8_t)(b->kind->line >> 8);
        line[2] = (uint8_t)b->kind->line;
    } else {
        /* The ring's colour: the commonest strong one, opaque, in the game's button. */
        int ring = -1;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) count[b->original[(y0 + y) * W + x0 + x]]++;
        }
        for (i = 0; i < b->colours; i++) {
            const uint8_t* p = b->rgba[i];
            int hi = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
            int lo = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            if (p[3] == 255 && hi - lo > 100 && (ring < 0 || count[i] > count[ring])) ring = i;
        }
        if (ring < 0) return;
        memcpy(line, b->rgba[ring], 3);
    }
    transparent = nearest(b, 0, 0, 0, 0);
    /* Inside the key: what can't be reached from the box's edge without
     * crossing its outline. */
    memset(inside, 1, (size_t)s * s);
    for (i = 0; i < s * s; i++) {
        x = i % s;
        y = i / s;
        if ((x == 0 || y == 0 || x == s - 1 || y == s - 1) && cov[i] < 128) {
            inside[i] = 0;
            stack[n++] = i;
        }
    }
    while (n) {
        static const int kStep[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        int at = stack[--n], d;
        for (d = 0; d < 4; d++) {
            int nx = at % s + kStep[d][0], ny = at / s + kStep[d][1], j = ny * s + nx;
            if (nx < 0 || ny < 0 || nx >= s || ny >= s || !inside[j] || cov[j] >= 128) continue;
            inside[j] = 0;
            stack[n++] = j;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int ix = x - ox, iy = y - oy, ink = 0, white = 0, dx, dy, r, g, bl, a;
            if (ix >= 0 && iy >= 0 && ix < s && iy < s) {
                ink = cov[iy * s + ix];
                if (inside[iy * s + ix]) white = 255;
            }
            /* The edge: the key, grown by EDGE. */
            for (dy = -EDGE; dy <= EDGE; dy++) {
                for (dx = -EDGE; dx <= EDGE; dx++) {
                    int jx = ix + dx, jy = iy + dy, v;
                    if (dx * dx + dy * dy > EDGE * EDGE + 1 || jx < 0 || jy < 0 || jx >= s || jy >= s) continue;
                    v = inside[jy * s + jx] ? 255 : cov[jy * s + jx];
                    if (v > white) white = v;
                }
            }
            /* Solid or clear: the bar's part-clear colours are the ring's,
             * which would fringe the white edge. */
            a = ink > white ? ink : white;
            a = a >= 96 ? 255 : 0;
            r = (255 * (255 - ink) + line[0] * ink) / 255;
            g = (255 * (255 - ink) + line[1] * ink) / 255;
            bl = (255 * (255 - ink) + line[2] * ink) / 255;
            ST8(ci8_byte(b->image, W, x0 + x, y0 + y), (uint8_t)(a ? nearest(b, r, g, bl, a) : transparent));
        }
    }
}

static void draw_pictures(void) {
    int i, k;
    for (i = 0; i < sPictureCount; i++) {
        Picture* b = &sPictures[i];
        if (kind_of(b->tex) != b->kind) { /* gone */
            free(b->original);
            sPictures[i--] = sPictures[--sPictureCount];
            continue;
        }
        for (k = 0; k < b->kind->spotCount; k++) draw_spot(b, &b->kind->spots[k]);
    }
}

/* void ResTex::Init(): a texture of a model file (TEX0), as the file is
 * set up after loading. */
void f_801B3090(ppc_t* c) {
    uint32_t tex = c->r[3] ? LD32(c->r[3]) : 0, file, size, pos, plt = 0, pal;
    const struct Kind* kind;
    Picture* b;
    int i, x, y;
    f_801B3090_orig(c);
    if (tex) hle_pause_texture(tex);
    if (!tex || !(kind = kind_of(tex))) return;
    for (i = 0; i < sPictureCount; i++) {
        if (sPictures[i].tex == tex) break;
    }
    if (i == sPictureCount) {
        /* Its colours: the PLT0 of the same name, in the same file. */
        file = tex + LD32(tex + 0xC);
        if (LD32(file) != 0x62726573) return; /* bres */
        size = LD32(file + 8);
        for (pos = file; pos + 0x20 <= file + size; pos += 4) {
            if (LD32(pos) == 0x504C5430 && name_is(pos + LD32(pos + 0x14), kind->name)) { /* PLT0 */
                plt = pos;
                break;
            }
        }
        if (!plt || LD32(plt + 0x18) != 2 || sPictureCount == MAX_PICTURES) return;
        b = &sPictures[sPictureCount];
        b->original = (uint8_t*)malloc((size_t)kind->w * kind->h);
        if (!b->original) return;
        b->kind = kind;
        b->tex = tex;
        b->image = tex + LD32(tex + 0x10);
        b->colours = LD16(plt + 0x1C) < MAX_COLOURS ? LD16(plt + 0x1C) : MAX_COLOURS;
        pal = plt + LD32(plt + 0x10);
        for (x = 0; x < b->colours; x++) {
            uint16_t v = LD16(pal + 2 * x);
            uint8_t* p = b->rgba[x];
            if (v & 0x8000) {
                p[0] = (uint8_t)((v >> 10 & 31) * 255 / 31);
                p[1] = (uint8_t)((v >> 5 & 31) * 255 / 31);
                p[2] = (uint8_t)((v & 31) * 255 / 31);
                p[3] = 255;
            } else {
                p[0] = (uint8_t)((v >> 8 & 15) * 17);
                p[1] = (uint8_t)((v >> 4 & 15) * 17);
                p[2] = (uint8_t)((v & 15) * 17);
                p[3] = (uint8_t)((v >> 12 & 7) * 255 / 7);
            }
        }
        sPictureCount++;
        HLE_LOG("prompts: %s texture %08X", kind->name, tex);
    }
    /* Loaded again (or for the first time): as the game has it. */
    b = &sPictures[i];
    for (y = 0; y < kind->h; y++) {
        for (x = 0; x < kind->w; x++) b->original[y * kind->w + x] = LD8(ci8_byte(b->image, kind->w, x, y));
    }
    sGeneration = 0xFFFFFFFFu; /* draw it */
}

/* Redraws the glyphs and the pictures if what they should show has changed.
 * Called on each report of the remote (hle_wpad.c). */
void hle_prompts_update(void) {
    uint32_t gen;
    int i;
    if ((!sFontCount && !sPictureCount) || !g_host_prompt_generation) return;
    gen = g_host_prompt_generation();
    if (gen == sGeneration && hle_minigame_text() * 2 == sGlyphLayer) return;
    sGeneration = gen;
    sGlyphLayer = hle_minigame_text() * 2; /* LAYER_GAMES, or LAYER_PARK */
    for (i = 0; i < sFontCount; i++) draw_icons(&sFonts[i]);
    draw_pictures();
}

/* void ResFontBase::SetResourceBuffer(void* font, FontInformation*): a
 * font, loaded and its offsets made pointers. */
void f_801FA730(ppc_t* c) {
    uint32_t base = c->r[4], size, pos, image;
    Font* f;
    int i, have = 0;
    f_801FA730_orig(c);
    /* RFNT, or RFNU once unpacked (its offsets made pointers). */
    if (!base || (LD32(base) != 0x52464E54 && LD32(base) != 0x52464E55)) return;
    for (i = 0; i < sFontCount; i++) {
        if (sFonts[i].font == base) return;
    }
    if (sFontCount == MAX_FONTS) return;
    size = LD32(base + 8);
    for (pos = base + LD16(base + 0xC); pos + 8 <= base + size && LD32(pos) != 0x54474C50; pos += LD32(pos + 4)) { /* TGLP */
        if (LD32(pos + 4) < 8) return;
    }
    if (pos + 0x20 > base + size) return;
    f = &sFonts[sFontCount];
    memset(f, 0, sizeof(*f));
    f->font = base;
    f->cw = LD8(pos + 8);
    f->ch = LD8(pos + 9);
    f->baseline = (int8_t)LD8(pos + 10);
    f->sheetSize = (int)LD32(pos + 12);
    f->fmt = LD16(pos + 18);
    f->perRow = LD16(pos + 20);
    f->perSheet = LD16(pos + 20) * LD16(pos + 22);
    f->sheetWidth = LD16(pos + 24);
    image = LD32(pos + 28);
    f->sheets = image >= 0x80000000u ? image : base + image;
    if (f->fmt != 0 || f->cw * f->ch > MAX_CELL || !f->perSheet) return; /* I4 only */
    for (i = 0; i < ICONS; i++) {
        int left, width, advance;
        f->glyph[i] = glyph_of(base, size, (uint16_t)(FIRST_ICON + i));
        f->width[i] = f->cw;
        if (f->glyph[i] >= 0) {
            have = 1;
            if (glyph_widths(f, f->glyph[i], &left, &width, &advance) && width > 0 && width < f->cw)
                f->width[i] = width;
        }
    }
    if (!have) return;
    sFontCount++;
    HLE_LOG("prompts: font %08X, cells %dx%d, button glyphs from %d", base, f->cw, f->ch, f->glyph[0]);
    sGeneration = 0xFFFFFFFFu; /* draw them */
}
