/* Drawing into an image on the CPU, for what is laid over the picture
 * (options.c) or shown on a second screen (friends.c): images scaled,
 * rounded boxes and text in the game's font (hle_prompts.c). Each file
 * that includes it has its own image, sPx (sW x sH, top row first,
 * 0xAARRGGBB, not premultiplied), and its own picture coordinates: x
 * and y there are X(x) and Y(y) in the image (sScale, sOffX, sOffY). */
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "translate.h"

int hle_font_metrics(int* cellW, int* cellH, int* baseline); /* hle_prompts.c */
int hle_font_glyph(uint16_t code, uint8_t* cov, int* left, int* advance);

/* The next character of text: UTF-8, or, a byte that doesn't begin a
 * sequence of it, Latin-1 ("PokéPark"). */
static uint16_t text_char(const char** s) {
    const uint8_t* p = (const uint8_t*)*s;
    uint32_t c = p[0];
    if (c >= 0xE0 && c < 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *s += 3;
        return (uint16_t)((c & 15) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u));
    }
    if (c >= 0xC2 && c < 0xE0 && (p[1] & 0xC0) == 0x80) {
        *s += 2;
        return (uint16_t)((c & 31) << 6 | (p[1] & 63u));
    }
    *s += 1;
    return (uint16_t)c;
}

typedef struct {
    float x0, y0, x1, y1;
} Rect;

static float sScale, sOffX, sOffY;

static float X(float x) { return sOffX + x * sScale; }
static float Y(float y) { return sOffY + y * sScale; }

static uint32_t* sPx;
static int sW, sH;

static void blend(uint32_t* d, uint32_t c, float alpha) {
    float sa = (c >> 24) / 255.0f * alpha, da = (*d >> 24) / 255.0f, oa = sa + da * (1 - sa);
    int k;
    uint32_t out;
    if (sa <= 0) return;
    out = (uint32_t)(oa * 255 + 0.5f) << 24;
    for (k = 0; k < 24; k += 8) {
        float sc = (float)(c >> k & 255), dc = (float)(*d >> k & 255);
        out |= (uint32_t)((sc * sa + dc * da * (1 - sa)) / oa + 0.5f) << k;
    }
    *d = out;
}

static uint32_t sample(const uint32_t* img, int w, int h, float u, float v) {
    int x0 = (int)floorf(u - 0.5f), y0 = (int)floorf(v - 0.5f), k, i;
    float fx = u - 0.5f - x0, fy = v - 0.5f - y0, acc[4] = {0}, wsum = 0;
    for (i = 0; i < 4; i++) {
        int x = x0 + (i & 1), y = y0 + (i >> 1);
        float wt = ((i & 1) ? fx : 1 - fx) * ((i >> 1) ? fy : 1 - fy);
        uint32_t c;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= w) x = w - 1;
        if (y >= h) y = h - 1;
        c = img[y * w + x];
        /* premultiplied while filtering, so clear texels add no colour */
        acc[3] += wt * (c >> 24);
        for (k = 0; k < 3; k++) acc[k] += wt * (c >> (8 * k) & 255) * (c >> 24) / 255.0f;
        wsum += wt;
    }
    if (acc[3] < 0.5f) return 0;
    return (uint32_t)(acc[3] + 0.5f) << 24 | (uint32_t)(acc[2] * 255 / acc[3] + 0.5f) << 16 |
           (uint32_t)(acc[1] * 255 / acc[3] + 0.5f) << 8 | (uint32_t)(acc[0] * 255 / acc[3] + 0.5f);
}

/* Draws part src of an image into dst (picture coordinates), its colour
 * times tint (0xRRGGBB), flipped left to right if mirror. */
static void draw_image(const uint32_t* img, int w, int h, Rect src, Rect dst, uint32_t tint, int mirror) {
    int x0 = (int)X(dst.x0), y0 = (int)Y(dst.y0), x1 = (int)ceilf(X(dst.x1)), y1 = (int)ceilf(Y(dst.y1)), x, y;
    float dw = X(dst.x1) - X(dst.x0), dh = Y(dst.y1) - Y(dst.y0);
    if (!img || dw <= 0 || dh <= 0) return;
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < sH; y++) {
        float v = src.y0 + (y + 0.5f - Y(dst.y0)) / dh * (src.y1 - src.y0);
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < sW; x++) {
            float t = (x + 0.5f - X(dst.x0)) / dw, u = src.x0 + (mirror ? 1 - t : t) * (src.x1 - src.x0);
            uint32_t c;
            if (t < 0 || t > 1 || v < src.y0 || v > src.y1) continue;
            c = sample(img, w, h, u, v);
            if (tint != 0xFFFFFF) {
                int k;
                uint32_t o = c & 0xFF000000u;
                for (k = 0; k < 24; k += 8) o |= ((c >> k & 255) * (tint >> k & 255) / 255) << k;
                c = o;
            }
            blend(&sPx[y * sW + x], c, 1.0f);
        }
    }
}

/* A rounded box of colour c (0xAARRGGBB), its edge softened. */
static void draw_box(Rect r, float radius, uint32_t c) {
    float rx0 = X(r.x0), ry0 = Y(r.y0), rx1 = X(r.x1), ry1 = Y(r.y1), rad = radius * sScale;
    int x, y;
    for (y = (int)ry0 < 0 ? 0 : (int)ry0; y < (int)ceilf(ry1) && y < sH; y++) {
        for (x = (int)rx0 < 0 ? 0 : (int)rx0; x < (int)ceilf(rx1) && x < sW; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float cx = px < rx0 + rad ? rx0 + rad : px > rx1 - rad ? rx1 - rad : px;
            float cy = py < ry0 + rad ? ry0 + rad : py > ry1 - rad ? ry1 - rad : py;
            float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - rad;
            float a = d < -1.5f ? 1.0f : d > 0 ? 0.0f : -d / 1.5f;
            if (a > 0) blend(&sPx[y * sW + x], c, a);
        }
    }
}

/* A triangle pointing up or down (dir 1, -1), centred at (cx, cy). */
static void draw_triangle(float cx, float cy, float size, int dir, uint32_t c) {
    int x, y;
    for (y = (int)Y(cy - size); y < (int)Y(cy + size) + 1; y++) {
        for (x = (int)X(cx - size); x < (int)X(cx + size) + 1; x++) {
            float u = ((x + 0.5f) - X(cx)) / (size * sScale), v = ((y + 0.5f) - Y(cy)) / (size * sScale) * dir;
            if (x < 0 || y < 0 || x >= sW || y >= sH) continue;
            if (v >= -0.6f && v <= 0.6f && fabsf(u) <= (v + 0.6f) / 1.2f) blend(&sPx[y * sW + x], c, 1.0f);
        }
    }
}

/* Text, in the game's font: size is its cell height in picture units. */
static int sCellW, sCellH, sBaseline;

/* Text is drawn, and measured, in the language the game's text is in
 * (translate.h), where it has a translation. */
static float text_width(const char* s, float size) {
    static uint8_t cov[64 * 64];
    float w = 0;
    int left, advance;
    if (sCellW * sCellH > (int)sizeof(cov)) return 0;
    for (s = tr(s); *s;) {
        if (hle_font_glyph(text_char(&s), cov, &left, &advance)) w += advance;
    }
    return w * size / sCellH;
}

/* Paints a coverage mask (bw x bh, 0-1) at window pixel (bx0, by0): the
 * edge, then the fill, each the coverage grown by a disc. */
static void paint_mask(const float* mask, int bw, int bh, int bx0, int by0, uint32_t fill, uint32_t edge,
                       float outline, int bold) {
    int j;
    for (j = 0; j < 2; j++) {
        float r = j == 0 ? outline * sScale + (bold ? 0.6f * sScale : 0) : (bold ? 0.6f * sScale : 0);
        uint32_t c = j == 0 ? edge : fill;
        int ri = (int)ceilf(r), a, b, n = 0, k, dx, dy;
        /* The disc's offsets and weights, the full ones first. */
        static int off[33 * 33][2];
        static float wt[33 * 33];
        if (j == 0 && outline <= 0) continue;
        if (ri > 16) ri = 16;
        for (k = 0; k < 2; k++) {
            for (dy = -ri; dy <= ri; dy++) {
                for (dx = -ri; dx <= ri; dx++) {
                    float d = sqrtf((float)(dx * dx + dy * dy));
                    if (d > r + 1 || (d <= r) != (k == 0)) continue;
                    off[n][0] = dx;
                    off[n][1] = dy;
                    wt[n++] = d <= r ? 1.0f : r + 1 - d;
                }
            }
        }
        for (b = 0; b < bh; b++) {
            int py = by0 + b;
            if (py < 0 || py >= sH) continue;
            for (a = 0; a < bw; a++) {
                int px = bx0 + a;
                float m = 0;
                if (px < 0 || px >= sW) continue;
                for (k = 0; k < n && m < 1; k++) {
                    int x = a + off[k][0], y = b + off[k][1];
                    float v;
                    if (x < 0 || y < 0 || x >= bw || y >= bh) continue;
                    v = mask[y * bw + x] * wt[k];
                    if (v > m) m = v;
                }
                if (m > 0) blend(&sPx[py * sW + px], c, m > 1 ? 1 : m);
            }
        }
    }
}

enum { ALIGN_LEFT, ALIGN_CENTRE, ALIGN_RIGHT };

/* Draws s with its middle at y, filled with fill, edged outline wide (in
 * picture units) with edge, and, if bold, thickened a little. Shrinks it
 * to fit in maxWidth. */
static void draw_text(const char* s, float x, float y, float size, int align, uint32_t fill, uint32_t edge,
                      float outline, int bold, float maxWidth) {
    static uint8_t cov[64 * 64];
    static float* mask;
    static int maskSize;
    float width = text_width(s, size), k, pen;
    int bx0, by0, bw, bh, i, j, pad;
    if (sCellW * sCellH > (int)sizeof(cov)) return;
    s = tr(s);
    if (maxWidth > 0 && width > maxWidth) {
        size *= maxWidth / width;
        width = maxWidth;
    }
    k = size / sCellH;
    x = align == ALIGN_CENTRE ? x - width / 2 : align == ALIGN_RIGHT ? x - width : x;
    pad = (int)ceilf((outline + 2) * sScale) + 1;
    bx0 = (int)X(x) - pad;
    by0 = (int)Y(y - size / 2) - pad;
    bw = (int)(width * sScale) + 2 * pad + 2;
    bh = (int)(size * sScale) + 2 * pad + 2;
    if (bw * bh > maskSize) {
        free(mask);
        maskSize = bw * bh;
        mask = (float*)malloc((size_t)maskSize * sizeof(float));
        if (!mask) {
            maskSize = 0;
            return;
        }
    }
    memset(mask, 0, (size_t)bw * bh * sizeof(float));
    /* The glyphs' coverage, scaled. */
    for (pen = x; *s;) {
        int left, advance, gx0, gy0, gx1, gy1;
        if (!hle_font_glyph(text_char(&s), cov, &left, &advance)) continue;
        gx0 = (int)X(pen + left * k) - bx0;
        gy0 = (int)Y(y - size / 2) - by0;
        gx1 = gx0 + (int)(sCellW * k * sScale) + 1;
        gy1 = gy0 + (int)(size * sScale) + 1;
        for (j = gy0 < 0 ? 0 : gy0; j < gy1 && j < bh; j++) {
            float v = (j - gy0 + 0.5f) / (k * sScale);
            for (i = gx0 < 0 ? 0 : gx0; i < gx1 && i < bw; i++) {
                float u = (i - gx0 + 0.5f) / (k * sScale), c;
                int ux = (int)u, vy = (int)v;
                float fx = u - ux, fy = v - vy;
                if (ux >= sCellW || vy >= sCellH) continue;
                c = cov[vy * sCellW + ux] * (1 - fx) * (1 - fy) +
                    (ux + 1 < sCellW ? cov[vy * sCellW + ux + 1] : 0) * fx * (1 - fy) +
                    (vy + 1 < sCellH ? cov[(vy + 1) * sCellW + ux] : 0) * (1 - fx) * fy +
                    (ux + 1 < sCellW && vy + 1 < sCellH ? cov[(vy + 1) * sCellW + ux + 1] : 0) * fx * fy;
                c /= 255.0f;
                if (c > mask[j * bw + i]) mask[j * bw + i] = c;
            }
        }
        pen += advance * k;
    }
    paint_mask(mask, bw, bh, bx0, by0, fill, edge, outline, bold);
}
