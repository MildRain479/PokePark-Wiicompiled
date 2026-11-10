/* GX command processor: decodes the FIFO written through the write-gather
 * pipe and keeps the GPU's register state.
 *
 * Commands (first byte):
 *   0x00        NOP
 *   0x08 r v32  load CP register r
 *   0x10 h32 .. load XF registers/memory: (count-1) << 16 | address
 *   0x20..0x38  load indexed XF data (matrices) from a CP array
 *   0x40 a s    call a display list in memory
 *   0x48        invalidate the vertex cache
 *   0x61 v32    load BP register (v >> 24)
 *   0x80..0xBF  draw: (primitive | vertex format), u16 count, vertices
 * Register layouts follow the hardware; see Dolphin's VideoCommon for a
 * reference description. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define LOG(...) do { if (sLog) { fprintf(stderr, "gx: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* Pixel-engine interrupts, raised when the commands that request them are
 * decoded (the GPU finishes instantly). */
#define INTERRUPT_PE_TOKEN 18
#define INTERRUPT_PE_FINISH 19
#define PE_TOKEN_REG 0xCC00100Eu
void hle_raise_interrupt(int n);
void hle_raise_interrupt_ex(int n, void (*before)(uint32_t data), uint32_t data);

/* Each token interrupt reports its own token, even when several are
 * queued before the first is handled. */
static void set_token(uint32_t token) { ST16(PE_TOKEN_REG, token); }

static int sLog;
static int sReady;

/* ------------------------------------------------------------------------ */
/* Register state */

static uint32_t cp[256];
static uint32_t bp[256];
static uint32_t bp_mask = 0xFFFFFF;
static uint32_t xf[0x1100];
/* XF memory 0x000-0x67F and registers 0x1000-0x105F, packed for the vertex
 * shader (gx.h), and the words written since the last draw. */
static uint32_t sXfPacked[GX_XF_WORDS];
static int sXfDirtyLo = 0, sXfDirtyHi = GX_XF_WORDS;

static void flush_batch(void);
/* Set when anything build_state reads may have changed: draws in between
 * reuse the state built last. */
static int sStateDirty = 1;
/* hle_gx.c marks the draws of the layer the minimap is drawn in (with
 * the berry counter in the park, and on menus the menu): those whose
 * origin lands in the minimap's corner, from its map on, are the
 * minimap's (GXDrawState's screen 1). */
static int sScreenMarked, sMapDrawn;

static void xf_write(uint32_t addr, uint32_t value) {
    int i;
    if (addr >= 0x1100) return;
    xf[addr] = value;
    if (addr < GX_XF_MEM_WORDS) i = (int)addr;
    else if (addr >= 0x1000 && addr < 0x1000 + GX_XF_REG_WORDS) i = GX_XF_MEM_WORDS + (int)(addr - 0x1000);
    else return;
    if (sXfPacked[i] == value) return;
    /* The pending batch is drawn with the XF state as it is now. */
    flush_batch();
    sXfPacked[i] = value;
    if (addr >= 0x101A && addr <= 0x101F) sStateDirty = 1; /* the viewport */
    if (i < sXfDirtyLo) sXfDirtyLo = i;
    if (i + 1 > sXfDirtyHi) sXfDirtyHi = i + 1;
}
static uint8_t tmem[1 << 20];

static float xf_float(uint32_t addr) {
    float f;
    memcpy(&f, &xf[addr], 4);
    return f;
}

/* TEV color registers: the initial PREV/REG0..2 values and the konst
 * colors, signed 11-bit (color) / 8-bit (konst) per component. */
static int16_t tev_color[4][4];
static int16_t tev_konst[4][4];

/* ------------------------------------------------------------------------ */
/* FIFO capture */

static uint8_t* sFifo;
static size_t sFifoLen, sFifoCap;
static size_t sNeed;

static void fifo_append(uint32_t value, int size) {
    int i;
    if (sFifoLen + 4 > sFifoCap) {
        sFifoCap = sFifoCap ? sFifoCap * 2 : (1u << 20);
        sFifo = (uint8_t*)realloc(sFifo, sFifoCap);
    }
    for (i = size - 1; i >= 0; i--) sFifo[sFifoLen++] = (uint8_t)(value >> (8 * i));
    /* Decode each command as soon as it is complete, as the GPU would:
     * indexed matrix loads, vertex arrays and textures are read from
     * memory the game reuses soon after. */
    if (sFifoLen >= sNeed) gx_flush();
}

/* Big-endian reads, as one load and a byte swap (ppc.h). */
static uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return BSWAP32(v);
}
static uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return BSWAP16(v);
}

/* ------------------------------------------------------------------------ */
/* Vertex formats */

enum { ATTR_NONE, ATTR_DIRECT, ATTR_INDEX8, ATTR_INDEX16 };

typedef struct VtxFormat {
    int pnmtx, texmtx[8];   /* per-vertex matrix indices present */
    int pos, nrm, col[2], tex[8];   /* ATTR_* */
    int pos_cnt, pos_fmt, pos_shift;
    int nrm_cnt, nrm_fmt, nrm_index3;
    int col_cnt[2], col_fmt[2];
    int tex_cnt[8], tex_fmt[8], tex_shift[8];
    int size;
    /* Worked out once for read_vertex: component counts and sizes, the
     * fixed-point scales, and which texture matrices and coordinates are
     * present (bit i for each). */
    int pos_n, pos_cs, nrm_cs, nrm_bytes, tex_n[8], tex_cs[8], texmtx_mask, tex_mask;
    float pos_scale, nrm_scale, tex_scale[8];
    /* Where each attribute sits in a vertex's bytes. */
    int off_pnmtx, off_texmtx[8], off_pos, off_nrm, off_col[2], off_tex[8];
} VtxFormat;

static int comp_size(int fmt) {
    static const int s[8] = {1, 1, 2, 2, 4, 0, 0, 0};
    return s[fmt & 7];
}

static int color_size(int fmt) {
    static const int s[8] = {2, 3, 4, 2, 3, 4, 0, 0};
    return s[fmt & 7];
}

static int index_size(int type) { return type == ATTR_INDEX8 ? 1 : 2; }

static void get_format(int vat, VtxFormat* f) {
    uint32_t lo = cp[0x50], hi = cp[0x60];
    uint32_t a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    int i, size = 0;
    memset(f, 0, sizeof(*f));
    f->pnmtx = lo & 1;
    for (i = 0; i < 8; i++) f->texmtx[i] = (lo >> (1 + i)) & 1;
    f->pos = (lo >> 9) & 3;
    f->nrm = (lo >> 11) & 3;
    f->col[0] = (lo >> 13) & 3;
    f->col[1] = (lo >> 15) & 3;
    for (i = 0; i < 8; i++) f->tex[i] = (hi >> (2 * i)) & 3;

    f->pos_cnt = a & 1;
    f->pos_fmt = (a >> 1) & 7;
    f->pos_shift = (a >> 4) & 31;
    f->nrm_cnt = (a >> 9) & 1;
    f->nrm_fmt = (a >> 10) & 7;
    f->col_cnt[0] = (a >> 13) & 1;
    f->col_fmt[0] = (a >> 14) & 7;
    f->col_cnt[1] = (a >> 17) & 1;
    f->col_fmt[1] = (a >> 18) & 7;
    f->tex_cnt[0] = (a >> 21) & 1;
    f->tex_fmt[0] = (a >> 22) & 7;
    f->tex_shift[0] = (a >> 25) & 31;
    f->nrm_index3 = (a >> 31) & 1;
    f->tex_cnt[1] = b & 1;
    f->tex_fmt[1] = (b >> 1) & 7;
    f->tex_shift[1] = (b >> 4) & 31;
    f->tex_cnt[2] = (b >> 9) & 1;
    f->tex_fmt[2] = (b >> 10) & 7;
    f->tex_shift[2] = (b >> 13) & 31;
    f->tex_cnt[3] = (b >> 18) & 1;
    f->tex_fmt[3] = (b >> 19) & 7;
    f->tex_shift[3] = (b >> 22) & 31;
    f->tex_cnt[4] = (b >> 27) & 1;
    f->tex_fmt[4] = (b >> 28) & 7;
    f->tex_shift[4] = c & 31;
    f->tex_cnt[5] = (c >> 5) & 1;
    f->tex_fmt[5] = (c >> 6) & 7;
    f->tex_shift[5] = (c >> 9) & 31;
    f->tex_cnt[6] = (c >> 14) & 1;
    f->tex_fmt[6] = (c >> 15) & 7;
    f->tex_shift[6] = (c >> 18) & 31;
    f->tex_cnt[7] = (c >> 23) & 1;
    f->tex_fmt[7] = (c >> 24) & 7;
    f->tex_shift[7] = (c >> 27) & 31;

    if (f->pnmtx) size++;
    for (i = 0; i < 8; i++) size += f->texmtx[i];
    if (f->pos == ATTR_DIRECT) size += comp_size(f->pos_fmt) * (f->pos_cnt ? 3 : 2);
    else if (f->pos) size += index_size(f->pos);
    if (f->nrm == ATTR_DIRECT) size += comp_size(f->nrm_fmt) * (f->nrm_cnt ? 9 : 3);
    else if (f->nrm) size += index_size(f->nrm) * (f->nrm_cnt && f->nrm_index3 ? 3 : 1);
    for (i = 0; i < 2; i++) {
        if (f->col[i] == ATTR_DIRECT) size += color_size(f->col_fmt[i]);
        else if (f->col[i]) size += index_size(f->col[i]);
    }
    for (i = 0; i < 8; i++) {
        if (f->tex[i] == ATTR_DIRECT) size += comp_size(f->tex_fmt[i]) * (f->tex_cnt[i] ? 2 : 1);
        else if (f->tex[i]) size += index_size(f->tex[i]);
    }
    f->size = size;

    f->pos_n = f->pos_cnt ? 3 : 2;
    f->pos_cs = comp_size(f->pos_fmt);
    f->pos_scale = 1.0f / (float)(1u << f->pos_shift);
    f->nrm_cs = comp_size(f->nrm_fmt);
    f->nrm_scale = f->nrm_fmt == 1 ? 1.0f / 64 : f->nrm_fmt == 3 ? 1.0f / 16384 : 1.0f;
    f->nrm_bytes = f->nrm == ATTR_DIRECT ? f->nrm_cs * (f->nrm_cnt ? 9 : 3)
                                         : index_size(f->nrm) * (f->nrm_cnt && f->nrm_index3 ? 3 : 1);
    size = 0;
    f->off_pnmtx = size;
    size += f->pnmtx;
    for (i = 0; i < 8; i++) {
        f->off_texmtx[i] = size;
        size += f->texmtx[i];
    }
    f->off_pos = size;
    if (f->pos == ATTR_DIRECT) size += f->pos_cs * f->pos_n;
    else if (f->pos) size += index_size(f->pos);
    f->off_nrm = size;
    if (f->nrm) size += f->nrm_bytes;
    for (i = 0; i < 2; i++) {
        f->off_col[i] = size;
        if (f->col[i] == ATTR_DIRECT) size += color_size(f->col_fmt[i]);
        else if (f->col[i]) size += index_size(f->col[i]);
    }
    for (i = 0; i < 8; i++) {
        f->off_tex[i] = size;
        if (f->tex[i] == ATTR_DIRECT) size += comp_size(f->tex_fmt[i]) * (f->tex_cnt[i] ? 2 : 1);
        else if (f->tex[i]) size += index_size(f->tex[i]);
    }
    for (i = 0; i < 8; i++) {
        f->tex_n[i] = f->tex_cnt[i] ? 2 : 1;
        f->tex_cs[i] = comp_size(f->tex_fmt[i]);
        f->tex_scale[i] = 1.0f / (float)(1u << f->tex_shift[i]);
        if (f->texmtx[i]) f->texmtx_mask |= 1 << i;
        if (f->tex[i]) f->tex_mask |= 1 << i;
    }
}

/* The vertex formats, worked out again only when their registers change
 * (a draw command names one; the park sends 22,000 a frame). */
static VtxFormat sFormats[8];
static uint32_t sFormatRegs[8][5];
static uint8_t sFormatValid[8];

static const VtxFormat* format(int vat) {
    uint32_t regs[5];
    regs[0] = cp[0x50];
    regs[1] = cp[0x60];
    regs[2] = cp[0x70 + vat];
    regs[3] = cp[0x80 + vat];
    regs[4] = cp[0x90 + vat];
    if (!sFormatValid[vat] || memcmp(regs, sFormatRegs[vat], sizeof(regs)) != 0) {
        get_format(vat, &sFormats[vat]);
        memcpy(sFormatRegs[vat], regs, sizeof(regs));
        sFormatValid[vat] = 1;
    }
    return &sFormats[vat];
}

/* Colors widen to 8 bits per channel as the GPU widens them, repeating
 * the high bits in the low ones. */
static void read_color(const uint8_t* p, int fmt, uint8_t out[4]) {
    uint32_t v;
    switch (fmt) {
    case 0: /* RGB565 */
        v = rd16(p);
        out[0] = (uint8_t)(((v >> 8) & 0xF8) | ((v >> 13) & 7));
        out[1] = (uint8_t)(((v >> 3) & 0xFC) | ((v >> 9) & 3));
        out[2] = (uint8_t)(((v << 3) & 0xF8) | ((v >> 2) & 7));
        out[3] = 255;
        break;
    case 1: /* RGB888 */
    case 2: /* RGB888x */
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = 255;
        break;
    case 3: /* RGBA4444 */
        out[0] = (uint8_t)((p[0] >> 4) * 17);
        out[1] = (uint8_t)((p[0] & 15) * 17);
        out[2] = (uint8_t)((p[1] >> 4) * 17);
        out[3] = (uint8_t)((p[1] & 15) * 17);
        break;
    case 4: /* RGBA6666 */
        v = ((uint32_t)p[0] << 16) | (p[1] << 8) | p[2];
        out[0] = (uint8_t)((((v >> 18) & 63) << 2) | ((v >> 22) & 3));
        out[1] = (uint8_t)((((v >> 12) & 63) << 2) | ((v >> 16) & 3));
        out[2] = (uint8_t)((((v >> 6) & 63) << 2) | ((v >> 10) & 3));
        out[3] = (uint8_t)(((v & 63) << 2) | ((v >> 4) & 3));
        break;
    default: /* RGBA8888 */
        memcpy(out, p, 4);
        break;
    }
}

/* Indexed attribute data lives in main memory: array base + index * stride. */
static const uint8_t* array_ptr(int array, uint32_t index) {
    return MA((cp[0xA0 + array] & 0x3FFFFFFF) + index * (cp[0xB0 + array] & 0xFF));
}

/* Vertices are read an attribute at a time across a whole draw, so the
 * choices that depend only on the format are made once per attribute. */

/* Where vertex i's value of an attribute is: in the vertex itself, or in
 * its array at the index the vertex holds. */
#define ATTR_SRC(i)                                                                                         \
    (type == ATTR_DIRECT ? p + (size_t)(i) * vsize + off                                                    \
                         : MA(base + (type == ATTR_INDEX8 ? p[(size_t)(i) * vsize + off]                    \
                                                          : rd16(p + (size_t)(i) * vsize + off)) * stride))

/* Reads n components of format fmt into the floats at byte offset field
 * of each vertex. */
static void read_floats(GXVertex* v, int count, const uint8_t* p, int vsize, int off, int type, int array, int fmt,
                        float scale, int n, size_t field) {
    uint32_t base = cp[0xA0 + array] & 0x3FFFFFFF, stride = cp[0xB0 + array] & 0xFF;
    int i, k;
    /* The kind of source (in the vertex, or indexed) chosen once for all
     * the vertices, not for each. */
#define EACH(expr)                                                                                          \
    if (type == ATTR_DIRECT) {                                                                              \
        const uint8_t* src = p + off;                                                                       \
        for (i = 0; i < count; i++, src += vsize) {                                                         \
            float* d = (float*)((uint8_t*)&v[i] + field);                                                   \
            for (k = 0; k < n; k++) d[k] = (expr);                                                          \
        }                                                                                                   \
    } else {                                                                                                \
        for (i = 0; i < count; i++) {                                                                       \
            const uint8_t* src = ATTR_SRC(i);                                                               \
            float* d = (float*)((uint8_t*)&v[i] + field);                                                   \
            for (k = 0; k < n; k++) d[k] = (expr);                                                          \
        }                                                                                                   \
    }
    switch (fmt) {
    case 0: EACH(src[k] * scale) break;
    case 1: EACH((int8_t)src[k] * scale) break;
    case 2: EACH(rd16(src + 2 * k) * scale) break;
    case 3: EACH((int16_t)rd16(src + 2 * k) * scale) break;
    default: {
        uint32_t u;
        EACH((u = rd32(src + 4 * k), *(float*)&u))
        break;
    }
    }
#undef EACH
}

static void read_colors(GXVertex* v, int count, const uint8_t* p, int vsize, int off, int type, int array, int fmt,
                        int which) {
    uint32_t base = cp[0xA0 + array] & 0x3FFFFFFF, stride = cp[0xB0 + array] & 0xFF;
    int i;
    if (type == ATTR_DIRECT) {
        const uint8_t* src = p + off;
        for (i = 0; i < count; i++, src += vsize) read_color(src, fmt, v[i].color[which]);
    } else {
        for (i = 0; i < count; i++) read_color(ATTR_SRC(i), fmt, v[i].color[which]);
    }
}

/* Reads count vertices of format f from p into v, which hold the defaults
 * for what the format lacks (vertex_template). */
static void read_vertices(const uint8_t* p, const VtxFormat* f, GXVertex* v, int count) {
    int vsize = f->size, i, k;
    if (f->pnmtx) {
        for (k = 0; k < count; k++) v[k].mtx[0] = p[(size_t)k * vsize + f->off_pnmtx];
    }
    for (i = 0; i < 8; i++) {
        if (!(f->texmtx_mask & (1 << i))) continue;
        for (k = 0; k < count; k++) v[k].mtx[1 + i] = p[(size_t)k * vsize + f->off_texmtx[i]];
    }
    if (f->pos) {
        read_floats(v, count, p, vsize, f->off_pos, f->pos, 0, f->pos_fmt, f->pos_scale, f->pos_n,
                    offsetof(GXVertex, pos));
    }
    if (f->nrm) {
        /* Of normal, binormal and tangent, only the normal: with three
         * indices, the first. */
        read_floats(v, count, p, vsize, f->off_nrm, f->nrm, 1, f->nrm_fmt, f->nrm_scale, 3, offsetof(GXVertex, nrm));
    }
    for (i = 0; i < 2; i++) {
        if (f->col[i]) read_colors(v, count, p, vsize, f->off_col[i], f->col[i], 2 + i, f->col_fmt[i], i);
    }
    for (i = 0; i < 8; i++) {
        if (!(f->tex_mask & (1 << i))) continue;
        read_floats(v, count, p, vsize, f->off_tex[i], f->tex[i], 4 + i, f->tex_fmt[i], f->tex_scale[i], f->tex_n[i],
                    offsetof(GXVertex, tex) + (size_t)i * 2 * sizeof(float));
    }
}

/* What a vertex holds before its attributes are read: the default matrix
 * indices (MATINDEX_A and B), white for absent colors, zeros. */
static void vertex_template(GXVertex* t) {
    uint32_t mi_a = xf[0x1018], mi_b = xf[0x1019];
    int i;
    memset(t, 0, sizeof(*t));
    t->mtx[0] = (uint8_t)(mi_a & 0x3F);
    for (i = 0; i < 8; i++) t->mtx[1 + i] = (uint8_t)(i < 4 ? (mi_a >> (6 + 6 * i)) & 0x3F : (mi_b >> (6 * (i - 4))) & 0x3F);
    memset(t->color, 255, sizeof(t->color));
}

/* ------------------------------------------------------------------------ */
/* Draw state for the backend */

static uint64_t sFrame;
static long sDebugFrame; /* defined with its value below */

typedef struct TexEntry {
    uint64_t key;
    uint32_t handle, hash;
    uint32_t image_hash; /* of the texture's memory alone, without its palette */
    int pix;             /* its layout (GX_PIX_...) */
    uint64_t checked, hashed; /* the frames it was last looked at, and hashed */
} TexEntry;

/* Which pages (4 KB) of MEM1 and MEM2 have been written, and when: the
 * frame after the write, as Windows' write watch reports them at each
 * frame's start. A texture is hashed again only when its memory has been
 * written since it last was; most never are, and hashing every texture
 * every frame took 4% of the time in the park. */
#define MEM1_PAGES (0x01800000u >> 12)
#define MEM2_PAGES (0x04000000u >> 12)
static uint32_t sPageWritten[MEM1_PAGES + MEM2_PAGES];
static int sWriteWatch = -1; /* whether it works: 1, 0, or -1 not yet known */

static int page_of(uint32_t phys) {
    if (phys < 0x01800000u) return (int)(phys >> 12);
    if (phys - 0x10000000u < 0x04000000u) return (int)(MEM1_PAGES + ((phys - 0x10000000u) >> 12));
    return -1;
}

/* At each frame's start: the pages written since the last. */
static void note_writes(uint32_t frame) {
#ifdef _WIN32
    static void* pages[4096];
    static const uint32_t kRange[2][2] = {{0x00000000u, 0x01800000u}, {0x10000000u, 0x04000000u}};
    int r;
    if (sWriteWatch == 0) return;
    for (r = 0; r < 2; r++) {
        uint8_t* base = g_mem + kRange[r][0];
        for (;;) {
            ULONG_PTR count = sizeof(pages) / sizeof(pages[0]), i;
            ULONG granularity;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, kRange[r][1], pages, &count, &granularity) != 0) {
                sWriteWatch = 0; /* memory without write watch: hash every frame */
                return;
            }
            sWriteWatch = 1;
            for (i = 0; i < count; i++) {
                int pg = page_of(kRange[r][0] + (uint32_t)((uint8_t*)pages[i] - base));
                if (pg >= 0) sPageWritten[pg] = frame;
            }
            if (count < sizeof(pages) / sizeof(pages[0])) break;
        }
    }
#else
    (void)frame;
    sWriteWatch = 0;
#endif
}

/* Whether guest memory [addr, addr + size) may have been written since frame. */
static int written_since(uint32_t addr, int size, uint64_t frame) {
    uint32_t phys = addr & 0x3FFFFFFFu, end = phys + (uint32_t)size - 1;
    int a = page_of(phys), b = page_of(end), i;
    if (sWriteWatch != 1 || a < 0 || b < 0 || b - a != (int)((end >> 12) - (phys >> 12))) return 1;
    for (i = a; i <= b; i++) {
        if (sPageWritten[i] > (uint32_t)frame) return 1;
    }
    return 0;
}
#define TEX_CACHE 4096
#define TEX_WAYS 4
static TexEntry sTexCache[TEX_CACHE];

/* Spreads every bit of a key over the result (splitmix64's finalizer). The
 * set index needs the address bits, which sit high in the key: without
 * this, all textures of one size and format shared a set and kept
 * evicting each other. */
static uint64_t mix64(uint64_t k) {
    k ^= k >> 30;
    k *= 0xBF58476D1CE4E5B9ull;
    k ^= k >> 27;
    k *= 0x94D049BB133111EBull;
    return k ^ (k >> 31);
}

/* Hashes every byte: textures are checked for changes on every frame they
 * are used. Four independent lanes of eight bytes, so the multiplies
 * overlap instead of each waiting for the last. */
static uint64_t hash_bytes64(const uint8_t* p, size_t n) {
    const uint64_t m = 0xFF51AFD7ED558CCDull;
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ (uint64_t)n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull,
             h3 = 0x27D4EB2F165667C5ull, h;
    size_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        h0 = (h0 ^ w[0]) * m;
        h1 = (h1 ^ w[1]) * m;
        h2 = (h2 ^ w[2]) * m;
        h3 = (h3 ^ w[3]) * m;
        h0 ^= h0 >> 29;
        h1 ^= h1 >> 29;
        h2 ^= h2 >> 29;
        h3 ^= h3 >> 29;
    }
    h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * m;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t hash_bytes(const uint8_t* p, int n) {
    uint64_t h = hash_bytes64(p, (size_t)n);
    return (uint32_t)(h ^ (h >> 32));
}

/* EFB copies made to textures, looked up before main memory. */
typedef struct CopyTex {
    uint32_t addr, handle;
    int width, height;
    int bytes;        /* size of the copy in guest memory */
    uint32_t mem_hash; /* of that memory when the copy was made */
    uint64_t checked;  /* frame its memory was last checked in, plus 1 */
    uint64_t made;     /* frame it was last made in, plus 1 */
} CopyTex;
static CopyTex sCopies[64];
static int sCopyCount;

/* Size in guest memory of an EFB copy of the given copy format (the
 * texture copy formats, R4 to GB8), from its bytes per texel. */
static int copy_bytes(int fmt, int width, int height) {
    static const int kTexFmt[16] = {0, 1, 1, 3, 3, 3, 6, 1, 1, 1, 1, 3, 3, 1, 1, 1}; /* I4, I8, IA8, RGBA8 sizes */
    return gx_texture_size(kTexFmt[fmt & 15], width, height);
}

/* The copy lives on the GPU only: guest memory at its address keeps what
 * was there. If that memory changes later, the game has put something
 * else there (a texture loaded from disc, say), and the copy no longer
 * stands for it. */
static int copy_still_valid(CopyTex* t) {
    /* Once a frame: a full-screen copy is over a megabyte, and a frame can
     * sample it many times. */
    if (t->checked == sFrame + 1) return 1;
    if (t->bytes > 0 && hash_bytes(MA(t->addr), t->bytes) != t->mem_hash) return 0;
    t->checked = sFrame + 1;
    return 1;
}

/* Registers a copy; returns 1 if it should go to guest memory too: when
 * it wasn't also made the frame before. The game's CPU reads such copies
 * (a photo, saved as a JPEG); the ones made every frame (reflections,
 * blur) only the GPU samples, and reading those back would hold it up. */
int gx_register_copy(uint32_t addr, uint32_t handle, int width, int height, int fmt) {
    int i, again;
    if (getenv("RECOMP_GX_COPY_LOG")) {
        static uint32_t seen[256];
        static int nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != addr; k++) {}
        if (k == nseen && nseen < 256) {
            seen[nseen++] = addr;
            fprintf(stderr, "gx: EFB copy target %08X %dx%d fmt %d (frame %llu, %d registered)\n", addr, width, height,
                    fmt, (unsigned long long)sFrame, sCopyCount);
        }
    }
    for (i = 0; i < sCopyCount; i++) {
        if (sCopies[i].addr == addr) break;
    }
    again = i < sCopyCount && sCopies[i].made >= sFrame; /* this frame or the last */
    if (i == sCopyCount) {
        if (sCopyCount == 64) i = 0;
        else sCopyCount++;
    }
    sCopies[i].made = sFrame + 1;
    sCopies[i].addr = addr;
    sCopies[i].handle = handle;
    sCopies[i].width = width;
    sCopies[i].height = height;
    sCopies[i].bytes = copy_bytes(fmt, width, height);
    sCopies[i].mem_hash = sCopies[i].bytes > 0 ? hash_bytes(MA(addr), sCopies[i].bytes) : 0;
    sCopies[i].checked = sFrame + 1;
    sStateDirty = 1; /* draws sampling addr now get the copy */
    return !again;
}

/* Puts the copy to addr, as read back (w x h RGBA8), into guest memory in
 * its format, unless the game has written there since it was made (the
 * memory is something else now) and the game hasn't asked for it (force).
 * It stays the copy for draws. */
void gx_copy_write_back(uint32_t addr, int fmt, int intensity, int w, int h, int stride, const uint8_t* rgba, int force) {
    int i;
    for (i = 0; i < sCopyCount && sCopies[i].addr != addr; i++) {}
    if (!force) {
        if (i == sCopyCount || sCopies[i].bytes <= 0) return;
        if (hash_bytes(MA(addr), sCopies[i].bytes) != sCopies[i].mem_hash) return;
    }
    gx_encode_copy(MA(addr), fmt, intensity, w, h, stride, rgba);
    if (i < sCopyCount && sCopies[i].bytes > 0) sCopies[i].mem_hash = hash_bytes(MA(addr), sCopies[i].bytes);
    if (getenv("RECOMP_GX_COPY_LOG"))
        fprintf(stderr, "gx: EFB copy %08X %dx%d fmt %d written to memory (frame %llu)\n", addr, w, h, fmt,
                (unsigned long long)sFrame);
}

/* Saves w x h RGBA pixels as a 32-bit BMP, top row first. */
static void save_bmp(const char* path, const uint8_t* rgba, int width, int height) {
    uint8_t hdr[54] = {'B', 'M'};
    int i;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    *(uint32_t*)(hdr + 2) = 54 + (uint32_t)(width * height * 4);
    hdr[10] = 54;
    hdr[14] = 40;
    *(int32_t*)(hdr + 18) = width;
    *(int32_t*)(hdr + 22) = -height;
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    for (i = 0; i < width * height; i++) {
        uint8_t bgra[4] = {rgba[4 * i + 2], rgba[4 * i + 1], rgba[4 * i], rgba[4 * i + 3]};
        fwrite(bgra, 1, 4, f);
    }
    fclose(f);
}

static uint32_t texture_for_map(int map) {
    int r = map < 4 ? map : map - 4 + 0x20;
    uint32_t mode0 = bp[0x80 + r], mode1 = bp[0x84 + r], img0 = bp[0x88 + r], img3 = bp[0x94 + r],
             tlut = bp[0x98 + r];
    int width = (img0 & 0x3FF) + 1, height = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    uint32_t addr = (img3 & 0xFFFFFF) << 5;
    int size = gx_texture_size(fmt, width, height), levels = 1, i;
    uint64_t key;
    TexEntry* e;
    uint32_t h;
    if ((long)sFrame == sDebugFrame) {
        int copy = -1;
        for (i = 0; i < sCopyCount; i++) {
            if (sCopies[i].addr == addr) copy = i;
        }
        fprintf(stderr, "   map %d: %08X fmt %d %dx%d mode %X tlut %X%s", map, addr, fmt, width, height, mode0, tlut,
                copy >= 0 ? " (EFB copy" : "\n");
        if (copy >= 0) fprintf(stderr, " %dx%d)\n", sCopies[copy].width, sCopies[copy].height);
    }
    for (i = 0; i < sCopyCount; i++) {
        if (sCopies[i].addr != addr) continue;
        if (copy_still_valid(&sCopies[i])) return sCopies[i].handle;
        /* Stale: forget it and use what is in memory now. */
        sCopies[i] = sCopies[--sCopyCount];
        break;
    }
    if (!addr || size <= 0) return 0;
    /* With a mipmap filter, the levels down to the LOD limit follow the
     * texture in memory, each half the last's size: all are used (as the
     * Wii would), not just the first, which shimmers in the distance. */
    if ((mode0 >> 5) & 3) {
        int lw = width, lh = height, most = ((mode1 >> 8) & 0xFF) / 16 + 1;
        while (levels < most && levels < 11 && (lw > 1 || lh > 1)) {
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
            size += gx_texture_size(fmt, lw, lh);
            levels++;
        }
    }
    key = ((uint64_t)addr << 32) ^ ((uint64_t)fmt << 28) ^ ((uint64_t)width << 14) ^ (uint64_t)height ^
          ((uint64_t)(tlut & 0xFFF) << 40) ^ ((uint64_t)(mode0 & 0x1F) << 52) ^
          ((uint64_t)((mode0 >> 5) & 0xFFFF) << 18) ^ ((uint64_t)levels << 59);
    {
        /* Four ways per set, so textures used together (a movie's Y, U and
         * V planes) don't evict each other; a miss takes the way used
         * longest ago. */
        TexEntry* set = &sTexCache[(size_t)(mix64(key) % (TEX_CACHE / TEX_WAYS)) * TEX_WAYS];
        int w;
        e = NULL;
        for (w = 0; w < TEX_WAYS && !e; w++) {
            if (set[w].handle && set[w].key == key) e = &set[w];
        }
        for (w = 0; w < TEX_WAYS && !e; w++) {
            if (!set[w].handle) e = &set[w];
        }
        if (!e) {
            e = &set[0];
            for (w = 1; w < TEX_WAYS; w++) {
                if (set[w].checked < e->checked) e = &set[w];
            }
        }
    }
    if (e->handle && e->key == key && e->checked == sFrame) return e->handle;
    {
        uint32_t image;
        if (e->handle && e->key == key && !written_since(addr, size, e->hashed)) {
            image = e->image_hash;
        } else {
            image = hash_bytes(MA(addr), size);
            e->hashed = sFrame;
        }
        e->image_hash = image;
        h = image;
    }
    if (fmt >= 8 && fmt <= 10) h ^= hash_bytes(&tmem[((tlut & 0x3FF) << 9) & 0xFFFFF], fmt == 8 ? 32 : 512);
    if (e->key != key || e->hash != h || !e->handle) {
        uint8_t* rgba = (uint8_t*)malloc((size_t)width * height * 8); /* the levels below take a third more */
        /* RECOMP_GX_DUMP_TEX=<address>: save that texture, as decoded, to
         * tex_<address>.bmp (once). RECOMP_GX_DUMP_TEXTURES=<dir>: save
         * every texture, as decoded, to <dir>/<hash>_<width>x<height>_
         * <format>.bmp, once each: the hash is the one a replacement would
         * be found by. Saved ones are decoded to RGBA8; the rest to the
         * smallest layout that holds them. */
        const char* want = getenv("RECOMP_GX_DUMP_TEX");
        const char* dir = getenv("RECOMP_GX_DUMP_TEXTURES");
        int pix = want || (dir && *dir) ? GX_PIX_RGBA8 : gx_texture_pix(fmt, MA(addr), width, height, levels, gxb_texture_pixels());
        {
            int lw = width, lh = height, l;
            uint32_t from = addr;
            uint8_t* to = rgba;
            for (l = 0; l < levels; l++) {
                if (pix == GX_PIX_RGBA8)
                    gx_decode_texture(to, MA(from), fmt, lw, lh, &tmem[((tlut & 0x3FF) << 9) & 0xFFFFF], (tlut >> 10) & 3);
                else gx_decode_texture_pix(to, MA(from), fmt, lw, lh, pix);
                from += (uint32_t)gx_texture_size(fmt, lw, lh);
                to += gx_pix_size(pix, lw, lh);
                lw = lw > 1 ? lw / 2 : 1;
                lh = lh > 1 ? lh / 2 : 1;
            }
        }
        if (pix == GX_PIX_RGBA8) {
            static int dumped;
            char path[512];
            if (want && !dumped && (strtoul(want, NULL, 16) & 0x3FFFFFFF) == (addr & 0x3FFFFFFF)) {
                snprintf(path, sizeof(path), "tex_%08X.bmp", addr);
                save_bmp(path, rgba, width, height);
                fprintf(stderr, "gx: saved %s (fmt %d %dx%d)\n", path, fmt, width, height);
                dumped = 1;
            }
            if (dir && *dir) {
                FILE* f;
                snprintf(path, sizeof(path), "%s/%08x_%dx%d_%d.bmp", dir, h, width, height, fmt);
                if ((f = fopen(path, "rb")) != NULL) fclose(f);
                else save_bmp(path, rgba, width, height);
            }
        }
        if (e->handle && e->key == key && e->pix == pix) {
            /* Same texture, new contents (a movie frame, say): the key
             * covers size and sampling, so update it in place. */
            flush_batch();
            gxb_texture_update(e->handle, pix, width, height, levels, rgba);
        } else {
            /* The way goes to another texture. The old one is freed (or
             * GPU memory grows without bound) once the frame is
             * presented: draws made this frame may still use it. */
            if (e->handle) gxb_texture_free(e->handle);
            e->handle = gxb_texture(key, pix, width, height, levels, rgba, mode0);
        }
        free(rgba);
        e->pix = pix;
        e->key = key;
        e->hash = h;
    }
    e->checked = sFrame;
    return e->handle;
}

/* A fog register's float: 11 bits of mantissa, 8 of exponent and a sign,
 * a float's top 20 bits. */
static float fog_float(uint32_t r) {
    uint32_t bits = ((r >> 19) & 1) << 31 | ((r >> 11) & 0xFF) << 23 | (r & 0x7FF) << 12;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void build_state(GXDrawState* st) {
    uint32_t genmode = bp[0x00], ac = bp[0xF3], blend = bp[0x41], zmode = bp[0x40];
    int i, k;
    memset(st, 0, sizeof(*st));
    st->num_stages = (int)((genmode >> 10) & 15) + 1;
    st->num_texcoords = (int)(genmode & 15);
    for (i = 0; i < st->num_stages; i++) {
        GXTevStage* s = &st->stage[i];
        uint32_t ce = bp[0xC0 + 2 * i], ae = bp[0xC1 + 2 * i];
        uint32_t order = bp[0x28 + i / 2] >> ((i & 1) * 12);
        uint32_t ksel = bp[0xF6 + i / 2] >> ((i & 1) * 10);
        s->color_in[0] = (ce >> 12) & 15;
        s->color_in[1] = (ce >> 8) & 15;
        s->color_in[2] = (ce >> 4) & 15;
        s->color_in[3] = ce & 15;
        s->color_bias = (ce >> 16) & 3;
        s->color_op = (ce >> 18) & 1;
        s->color_clamp = (ce >> 19) & 1;
        s->color_scale = (ce >> 20) & 3;
        s->color_dest = (ce >> 22) & 3;
        s->alpha_in[0] = (ae >> 13) & 7;
        s->alpha_in[1] = (ae >> 10) & 7;
        s->alpha_in[2] = (ae >> 7) & 7;
        s->alpha_in[3] = (ae >> 4) & 7;
        s->ras_swap = ae & 3;
        s->tex_swap = (ae >> 2) & 3;
        s->alpha_bias = (ae >> 16) & 3;
        s->alpha_op = (ae >> 18) & 1;
        s->alpha_clamp = (ae >> 19) & 1;
        s->alpha_scale = (ae >> 20) & 3;
        s->alpha_dest = (ae >> 22) & 3;
        s->texmap = order & 7;
        s->texcoord = (order >> 3) & 7;
        s->tex_enable = (order >> 6) & 1;
        s->channel = (order >> 7) & 7;
        s->kcolor_sel = (ksel >> 4) & 31;
        s->kalpha_sel = (ksel >> 9) & 31;
    }
    for (i = 0; i < 4; i++) {
        uint32_t a = bp[0xF6 + 2 * i], b = bp[0xF7 + 2 * i];
        st->swap_table[i][0] = a & 3;
        st->swap_table[i][1] = (a >> 2) & 3;
        st->swap_table[i][2] = b & 3;
        st->swap_table[i][3] = (b >> 2) & 3;
        for (k = 0; k < 4; k++) {
            st->tev_reg[i][k] = tev_color[i][k] / 255.0f;
            st->konst[i][k] = tev_konst[i][k] / 255.0f;
        }
    }
    st->ind_stages = (int)((genmode >> 16) & 7);
    for (i = 0; i < st->num_stages; i++) {
        st->ind_cmd[i] = bp[0x10 + i] & 0x1FFFFF;
        if (st->ind_cmd[i]) st->ind_any = 1;
    }
    if (st->ind_any) {
        for (i = 0; i < 4; i++) {
            uint32_t ss = bp[0x25 + i / 2] >> ((i & 1) * 8);
            st->ind_map[i] = (int)((bp[0x27] >> (6 * i)) & 7);
            st->ind_coord[i] = (int)((bp[0x27] >> (6 * i + 3)) & 7);
            st->ind_scale[i][0] = (int)(ss & 15);
            st->ind_scale[i][1] = (int)((ss >> 4) & 15);
            if (i < st->ind_stages && !st->texture[st->ind_map[i]]) st->texture[st->ind_map[i]] = texture_for_map(st->ind_map[i]);
        }
        for (i = 0; i < 3; i++) {
            uint32_t a = bp[0x06 + 3 * i], b = bp[0x07 + 3 * i], c = bp[0x08 + 3 * i];
            const uint32_t col[3] = {a, b, c};
            for (k = 0; k < 3; k++) {
                st->ind_mtx[i][0][k] = (int)(col[k] << 21) >> 21;
                st->ind_mtx[i][1][k] = (int)((col[k] >> 11) << 21) >> 21;
            }
            st->ind_mtx_scale[i] = (int)(((a >> 22) & 3) | ((b >> 22) & 3) << 2 | ((c >> 22) & 3) << 4);
        }
        for (i = 0; i < 8; i++) {
            st->tc_size[i][0] = (int)(bp[0x30 + 2 * i] & 0xFFFF) + 1;
            st->tc_size[i][1] = (int)(bp[0x31 + 2 * i] & 0xFFFF) + 1;
        }
    }
    for (i = 0; i < st->num_stages; i++) {
        const GXTevStage* s = &st->stage[i];
        if (s->tex_enable && !st->texture[s->texmap]) st->texture[s->texmap] = texture_for_map(s->texmap);
    }
    st->alpha_ref[0] = ac & 0xFF;
    st->alpha_ref[1] = (ac >> 8) & 0xFF;
    st->alpha_func[0] = (ac >> 16) & 7;
    st->alpha_func[1] = (ac >> 19) & 7;
    st->alpha_logic = (ac >> 22) & 3;
    st->blend_enable = blend & 1;
    st->logic_enable = (blend >> 1) & 1;
    st->color_update = (blend >> 3) & 1;
    st->alpha_update = (blend >> 4) & 1;
    st->blend_dst = (blend >> 5) & 7;
    st->blend_src = (blend >> 8) & 7;
    st->blend_subtract = (blend >> 11) & 1;
    st->logic_op = (blend >> 12) & 15;
    st->z_enable = zmode & 1;
    st->z_func = (zmode >> 1) & 7;
    st->z_update = (zmode >> 4) & 1;
    st->efb_alpha = (bp[0x43] & 7) == 1;
    st->fog_type = (bp[0xF1] >> 21) & 7;
    st->fog_ortho = (bp[0xF1] >> 20) & 1;
    st->fog_a = fog_float(bp[0xEE]);
    st->fog_b_mag = (int)(bp[0xEF] & 0xFFFFFF);
    st->fog_b_shift = (int)(bp[0xF0] & 31);
    st->fog_c = fog_float(bp[0xF1]);
    st->fog_color[0] = ((bp[0xF2] >> 16) & 0xFF) / 255.0f;
    st->fog_color[1] = ((bp[0xF2] >> 8) & 0xFF) / 255.0f;
    st->fog_color[2] = (bp[0xF2] & 0xFF) / 255.0f;
    st->fog_range = st->fog_type && ((bp[0xE8] >> 10) & 1);
    if (st->fog_range) {
        st->fog_center = (float)(bp[0xE8] & 0x3FF) - 342.0f;
        for (i = 0; i < 10; i++) st->fog_k[i] = (float)((bp[0xE9 + i / 2] >> (12 * (i & 1))) & 0xFFF) / 256.0f;
    }
    st->cull_mode = (genmode >> 14) & 3;
    {
        float wd = xf_float(0x101A), ht = xf_float(0x101B), zr = xf_float(0x101C);
        float xo = xf_float(0x101D), yo = xf_float(0x101E), fz = xf_float(0x101F);
        st->viewport[0] = xo - 342.0f - wd;
        st->viewport[1] = yo - 342.0f + ht;
        st->viewport[2] = 2.0f * wd;
        st->viewport[3] = -2.0f * ht;
        st->viewport[4] = (fz - zr) / 16777216.0f;
        st->viewport[5] = fz / 16777216.0f;
    }
    {
        uint32_t tl = bp[0x20], br = bp[0x21], off = bp[0x59];
        int ox = (int)(off & 0x3FF) * 2 - 342, oy = (int)((off >> 10) & 0x3FF) * 2 - 342;
        st->scissor[0] = (int)((tl >> 12) & 0x7FF) - 342 - ox;
        st->scissor[1] = (int)(tl & 0x7FF) - 342 - oy;
        st->scissor[2] = (int)((br >> 12) & 0x7FF) - 342 - ox + 1;
        st->scissor[3] = (int)(br & 0x7FF) - 342 - oy + 1;
    }
}

/* ------------------------------------------------------------------------ */
/* Draws */

static unsigned sDrawCount, sVertexCount;

/* RECOMP_GX_DEBUG_FRAME=n prints every draw of frame n. */
static long sDebugFrame = -1;
static unsigned sOpCount[256];

static void debug_draw(const GXDrawState* st, const GXVertex* v, int n) {
    int i;
    fprintf(stderr, "draw %u: %d verts, vp %.0f,%.0f %.0fx%.0f z %.3f-%.3f, sc %d,%d-%d,%d, stages %d, blend %d %d/%d sub %d lop %d/%d, z %d/%d/%d, cu %d au %d\n",
            sDrawCount, n, st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3], st->viewport[4],
            st->viewport[5], st->scissor[0], st->scissor[1], st->scissor[2], st->scissor[3], st->num_stages,
            st->blend_enable, st->blend_src, st->blend_dst, st->blend_subtract, st->logic_enable, st->logic_op, st->z_enable,
            st->z_func, st->z_update, st->color_update, st->alpha_update);
    fprintf(stderr, "   alpha test %d %d / %d %d logic %d; chan0 ctrl %X/%X mat %08X amb %08X; konst0 %.2f %.2f %.2f %.2f\n",
            st->alpha_func[0], st->alpha_ref[0], st->alpha_func[1], st->alpha_ref[1], st->alpha_logic, xf[0x100E],
            xf[0x1010], xf[0x100C], xf[0x100A], st->konst[0][0], st->konst[0][1], st->konst[0][2], st->konst[0][3]);
    fprintf(stderr, "   tev regs: prev %.2f %.2f %.2f %.2f, c0 %.2f %.2f %.2f %.2f, c1 %.2f %.2f %.2f %.2f, c2 %.2f %.2f %.2f %.2f\n",
            st->tev_reg[0][0], st->tev_reg[0][1], st->tev_reg[0][2], st->tev_reg[0][3], st->tev_reg[1][0],
            st->tev_reg[1][1], st->tev_reg[1][2], st->tev_reg[1][3], st->tev_reg[2][0], st->tev_reg[2][1],
            st->tev_reg[2][2], st->tev_reg[2][3], st->tev_reg[3][0], st->tev_reg[3][1], st->tev_reg[3][2],
            st->tev_reg[3][3]);
    if (xf[0x100E] & 2) {
        /* A lit channel: its lights' color, cos and distance attenuation,
         * position and direction. */
        uint32_t mask = ((xf[0x100E] >> 2) & 15) | (((xf[0x100E] >> 11) & 15) << 4), j;
        for (j = 0; j < 8; j++) {
            uint32_t b = 0x600 + j * 16;
            if (!(mask & (1u << j))) continue;
            fprintf(stderr, "   light %u: color %08X cos %g %g %g dist %g %g %g pos %g %g %g dir %g %g %g\n", j, xf[b + 3],
                    xf_float(b + 4), xf_float(b + 5), xf_float(b + 6), xf_float(b + 7), xf_float(b + 8), xf_float(b + 9),
                    xf_float(b + 10), xf_float(b + 11), xf_float(b + 12), xf_float(b + 13), xf_float(b + 14),
                    xf_float(b + 15));
        }
    }
    for (i = 0; i < st->num_stages && i < 3; i++) {
        const GXTevStage* t = &st->stage[i];
        fprintf(stderr, "   stage %d: c %d %d %d %d a %d %d %d %d tex %d(map %d coord %d) ch %d k %d/%d -> %d/%d\n", i,
                t->color_in[0], t->color_in[1], t->color_in[2], t->color_in[3], t->alpha_in[0], t->alpha_in[1],
                t->alpha_in[2], t->alpha_in[3], t->tex_enable, t->texmap, t->texcoord, t->channel, t->kcolor_sel,
                t->kalpha_sel, t->color_dest, t->alpha_dest);
    }
    if (sDrawCount == 1) {
        int k, nz = 0;
        fprintf(stderr, "   xf rows with data:");
        for (k = 0; k < 0x100; k += 4) {
            if (xf[k] | xf[k + 1] | xf[k + 2] | xf[k + 3]) { fprintf(stderr, " %d", k / 4); nz++; }
        }
        fprintf(stderr, " (%d); MATINDEX_A %08X, CP 0xAC %08X 0xBC %08X\n", nz, xf[0x1018], cp[0xAC], cp[0xBC]);
    }
    for (i = 0; i < n && i < 3; i++) {
        /* The vertex's view-space z and its depth as GX computes it (the
         * shader does the same on the GPU). */
        uint32_t m = (uint32_t)v[i].mtx[0] * 4 + 8;
        float vz = xf_float(m) * v[i].pos[0] + xf_float(m + 1) * v[i].pos[1] + xf_float(m + 2) * v[i].pos[2] + xf_float(m + 3);
        float cz = xf[0x1026] ? xf_float(0x1024) * vz + xf_float(0x1025) : xf_float(0x1024) * vz + xf_float(0x1025);
        float cw = xf[0x1026] ? 1.0f : -vz;
        uint32_t mr = (uint32_t)v[i].mtx[0] * 4;
        float vx = xf_float(mr) * v[i].pos[0] + xf_float(mr + 1) * v[i].pos[1] + xf_float(mr + 2) * v[i].pos[2] + xf_float(mr + 3);
        float vy = xf_float(mr + 4) * v[i].pos[0] + xf_float(mr + 5) * v[i].pos[1] + xf_float(mr + 6) * v[i].pos[2] + xf_float(mr + 7);
        float cx = xf[0x1026] ? xf_float(0x1020) * vx + xf_float(0x1021) : xf_float(0x1020) * vx + xf_float(0x1021) * vz;
        float cy = xf[0x1026] ? xf_float(0x1022) * vy + xf_float(0x1023) : xf_float(0x1022) * vy + xf_float(0x1023) * vz;
        /* Screen position in the 640x456 EFB (y down). */
        float sx = cw != 0 ? (cx / cw * 0.5f + 0.5f) * st->viewport[2] + st->viewport[0] : 0;
        float sy = cw != 0 ? (0.5f - cy / cw * 0.5f) * st->viewport[3] + st->viewport[1] : 0;
        fprintf(stderr, "   v%d mtx %.0f pos %.3f %.3f %.3f col %.2f %.2f %.2f %.2f tex %.3f %.3f  view z %.6f depth %.8f screen %.0f,%.0f\n", i,
                (float)v[i].mtx[0], v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].color[0][0] / 255.0f,
                v[i].color[0][1] / 255.0f, v[i].color[0][2] / 255.0f, v[i].color[0][3] / 255.0f, v[i].tex[0][0], v[i].tex[0][1], vz, cw != 0 ? 1.0f + cz / cw : 0.0f, sx, sy);
    }
}

/* Batching: consecutive draws with the same state go to the backend as
 * one. The game sends many small primitives (22,000 a frame in the park),
 * and each backend draw costs far more than its vertices. A batch is
 * drawn before anything that changes what it would look like: different
 * state, an XF write (xf_write), a texture's contents changing
 * (texture_for_map), or an EFB copy, clear or present (efb_copy).
 *
 * Vertices are decoded straight into the batch, each once, and the
 * primitives become indices into them. */
static GXDrawState sState, sBatchState;
static GXVertex* sBatch;
static uint32_t* sIndex;
static int sBatchVerts, sBatchCap, sIndexCount, sIndexCap;
/* Where the pending batch starts. While the backend records frames
 * (gxb_recording), batches follow each other through the whole frame;
 * otherwise each starts at 0. */
static int sBatchFirst, sIndexFirst, sKeepFrame;
/* The display list being run (0 outside one) and the draws made in it
 * so far: a draw's identity from frame to frame. */
static uint32_t sDisplayList, sListDraws;
/* Vertices go to the device's arena (gxb_arena), display lists' kept
 * there from frame to frame (list_begin). */
static int sArena;

/* The frame's vertices so far, for drawing what's been recorded of it
 * (gxb_copy_sync, while frames are recorded). */
void gx_frame_data_so_far(void) {
    flush_batch();
    if (sKeepFrame) gxb_frame_data(sArena ? NULL : sBatch, sBatchVerts, sIndex, sIndexCount);
}

static void flush_batch(void) {
    if (sIndexCount == sIndexFirst) return;
    if (sArena) gxb_draw_triangles(&sBatchState, NULL, 0, 0, sIndex, sIndexFirst, sIndexCount - sIndexFirst);
    else
        gxb_draw_triangles(&sBatchState, sBatch, sBatchFirst, sBatchVerts - sBatchFirst, sIndex, sIndexFirst,
                           sIndexCount - sIndexFirst);
    if (sKeepFrame) {
        sBatchFirst = sBatchVerts;
        sIndexFirst = sIndexCount;
    } else {
        sBatchVerts = sIndexCount = 0;
    }
}

/* Whether the position matrix (XF's, not the vertices') puts its origin
 * in the minimap's corner of the screen. */
static int origin_in_minimap(const GXDrawState* st) {
    uint32_t m = (xf[0x1018] & 63) * 4;
    float tx = xf_float(m + 3), ty = xf_float(m + 7), tz = xf_float(m + 11), x, y;
    if (xf[0x1026]) { /* orthographic */
        x = xf_float(0x1020) * tx + xf_float(0x1021);
        y = xf_float(0x1022) * ty + xf_float(0x1023);
    } else {
        if (tz >= 0) return 0;
        x = (xf_float(0x1020) * tx + xf_float(0x1021) * tz) / -tz;
        y = (xf_float(0x1022) * ty + xf_float(0x1023) * tz) / -tz;
    }
    x = st->viewport[0] + (x + 1) * 0.5f * st->viewport[2];
    y = st->viewport[1] + (1 - y) * 0.5f * st->viewport[3];
    return x >= kGxMinimapRect[0] && x < kGxMinimapRect[2] && y >= kGxMinimapRect[1] && y < kGxMinimapRect[3];
}

/* The canvas has margins (gx_canvas), as the frame started. */
static int sCanvasOn;
void gx_canvas(int* mx, int* my);                       /* gx_frames.c */
int gx_canvas_whole(float x, float y, float w, float h); /* gx_frames.c */

/* GXDrawState's canvas for a draw of count vertices at p, with the state
 * st: 1 if it is the world in 3D (a perspective projection with a far
 * plane past 2000) over the whole picture; 2 if it covers the whole
 * picture otherwise (2D, or the 2D screens' 3D), in up to 64 vertices;
 * else 0. */
static int canvas_kind(const GXDrawState* st, const uint8_t* p, const VtxFormat* f, int count) {
    static GXVertex v[64];
    GXVertex tpl;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    int i, persp = !xf[0x1026];
    if (!sCanvasOn || !gx_canvas_whole(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3])) return 0;
    /* The park's cameras see to 9999; the 2D screens' 3D (the menus'
     * boards, close-ups) only to 1000, and stays in the picture, as the
     * rest of the screen does. */
    if (persp && xf_float(0x1024) != 0 && xf_float(0x1025) / xf_float(0x1024) > 2000) return 1;
    if (count > 64 || count <= 0) return 0;
    vertex_template(&tpl);
    for (i = 0; i < count; i++) v[i] = tpl;
    read_vertices(p, f, v, count);
    for (i = 0; i < count; i++) {
        uint32_t m = (v[i].mtx[0] & 63) * 4;
        const float* q = v[i].pos;
        float wx = xf_float(m) * q[0] + xf_float(m + 1) * q[1] + xf_float(m + 2) * q[2] + xf_float(m + 3);
        float wy = xf_float(m + 4) * q[0] + xf_float(m + 5) * q[1] + xf_float(m + 6) * q[2] + xf_float(m + 7);
        float wz = xf_float(m + 8) * q[0] + xf_float(m + 9) * q[1] + xf_float(m + 10) * q[2] + xf_float(m + 11);
        float cx, cy, x, y;
        if (persp) {
            if (wz >= 0) return 0; /* behind the camera */
            cx = (xf_float(0x1020) * wx + xf_float(0x1021) * wz) / -wz;
            cy = (xf_float(0x1022) * wy + xf_float(0x1023) * wz) / -wz;
        } else {
            cx = xf_float(0x1020) * wx + xf_float(0x1021);
            cy = xf_float(0x1022) * wy + xf_float(0x1023);
        }
        x = st->viewport[0] + (cx + 1) * 0.5f * st->viewport[2];
        y = st->viewport[1] + (1 - cy) * 0.5f * st->viewport[3];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    return gx_canvas_whole(x0, y0, x1 - x0, y1 - y0) ? 2 : 0;
}

/* Whether a guest physical address has n bytes of memory from it. */
static int phys_ok(uint32_t a, uint32_t n) {
    return a + n <= 0x01800000u || (a >= 0x10000000u && a + n <= 0x14000000u);
}

/* Whether texture map 0's image is a minimap's map: a texture named
 * Fdm<area>, its TEX0 header just before it. */
static int sMapArea, sMapZone; /* from the map's name: FdmAr02Zn01 */

/* Where the minimap last drawn was of: area (0 if none yet) and zone. */
int gx_minimap_place(int* zone) {
    *zone = sMapZone;
    return sMapArea;
}

static int texture_is_map(void) {
    static uint32_t last = ~0u;
    static int was;
    uint32_t a = (bp[0x94] & 0xFFFFFFu) << 5, h = a - 0x40, name;
    if (a == last) return was;
    last = a;
    was = 0;
    if (!phys_ok(h, 0x40) || memcmp(g_mem + h, "TEX0", 4) != 0) return 0;
    name = h + rd32(g_mem + h + 0x14);
    was = phys_ok(name, 12) && memcmp(g_mem + name, "Fdm", 3) == 0;
    if (was && memcmp(g_mem + name + 3, "Ar", 2) == 0 && memcmp(g_mem + name + 7, "Zn", 2) == 0) {
        const uint8_t* n = g_mem + name;
        sMapArea = (n[5] - '0') * 10 + (n[6] - '0');
        sMapZone = (n[9] - '0') * 10 + (n[10] - '0');
    }
    return was;
}

/* Makes the batch one drawn with the current state, drawing the pending
 * one first if its state differs. */
static void use_current_state(int mtx_per_vertex, const uint8_t* p, const VtxFormat* f, int count) {
    int rebuilt = sStateDirty, screen, canvas;
    if (rebuilt) {
        build_state(&sState); /* may draw the pending batch, if a texture changes */
        sStateDirty = 0;
    }
    screen = 0;
    /* The map's own pieces wherever their origin is (a zone's map can be
     * several, the Beach's), and what's drawn on it once it has been. */
    if (sScreenMarked && !(mtx_per_vertex & 1)) {
        if (texture_is_map()) sMapDrawn = screen = 1;
        else if (origin_in_minimap(&sState)) screen = sMapDrawn;
    }
    canvas = canvas_kind(&sState, p, f, count);
    if (sState.mtx_per_vertex != mtx_per_vertex || sState.screen != screen || sState.canvas != canvas) {
        sState.mtx_per_vertex = mtx_per_vertex;
        sState.screen = screen;
        sState.canvas = canvas;
        rebuilt = 1;
    }
    /* Everything but the XF fields, which build_state leaves zero. A
     * pending batch already has the current state unless it was rebuilt. */
    if (sIndexCount == sIndexFirst || (rebuilt && memcmp(&sState, &sBatchState, offsetof(GXDrawState, xf)) != 0)) {
        flush_batch();
        sBatchState = sState;
        sBatchState.xf = sXfPacked;
        sBatchState.xf_dirty_lo = sXfDirtyLo;
        sBatchState.xf_dirty_hi = sXfDirtyHi;
        sBatchState.key = 0;
        sXfDirtyLo = GX_XF_WORDS;
        sXfDirtyHi = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Display lists, decoded once
 *
 * Where the device keeps vertices in video memory (gxb_arena), a display
 * list's vertices are decoded the first time it is called and kept there;
 * later calls draw them from there, while the list's bytes, the vertex
 * formats and array registers it starts with, and the parts of the arrays
 * its vertices index are as they were. The park's display lists are
 * nearly all its vertices (221,000 a frame), and decoding them and copying
 * them to the GPU every frame was a third of the game thread's time.
 * Draws outside display lists go to spans of the arena used for a frame. */

/* Arena spans let go of once no recorded frame draws from them: a frame
 * is drawn again until the next two are made. */
typedef struct LateFree {
    uint64_t frame;
    int first, n;
} LateFree;
static LateFree* sLate;
static int sLateCount, sLateCap;

static void arena_free_later(int first, int n) {
    if (n <= 0) return;
    if (sLateCount == sLateCap) {
        sLateCap = sLateCap ? sLateCap * 2 : 256;
        sLate = (LateFree*)realloc(sLate, (size_t)sLateCap * sizeof(LateFree));
    }
    sLate[sLateCount].frame = sFrame;
    sLate[sLateCount].first = first;
    sLate[sLateCount].n = n;
    sLateCount++;
}

/* A part of an array the list's vertices index, and its bytes' hash. */
typedef struct ListRange {
    uint32_t addr, len;
    uint64_t hash;
} ListRange;

typedef struct ListEntry {
    uint32_t addr, size; /* 0 size: unused */
    uint64_t state;      /* list_state as it was called */
    uint64_t hash;       /* of the list's bytes */
    int first, verts;    /* its vertices in the arena; first -1 for none */
    int valid;           /* decoded, and its vertices kept */
    ListRange* ranges;
    int nranges, rangecap;
    uint64_t used;       /* the last frame it was called in */
    int changed;         /* calls in a row it was different */
} ListEntry;

#define LIST_SETS 4096
#define LIST_WAYS 4
/* Calls in a row a list was different, after which it's only checked
 * every 64 frames; and the count for one that can't be kept. */
#define LIST_CHANGING 4
#define LIST_UNCACHED 1000000
static ListEntry sLists[LIST_SETS * LIST_WAYS];

/* The list running: drawn from its entry's vertices (sListBuild 0), or
 * decoded into sListVerts for it; sListAt vertices so far, of sListMax
 * made room for. */
static ListEntry* sList;
static int sListBuild, sListAt, sListMax;
static GXVertex* sListVerts;
static int sListVertsCap;
static ListRange* sRanges;
static int sRangeCount, sRangeCap;
/* Spans for draws outside display lists: the rest of this frame's. */
static int sTransNext, sTransLeft;

/* What a list's vertices depend on besides its bytes and the arrays: the
 * vertex descriptors and formats, the array registers, and the default
 * matrix indices. */
static uint64_t list_state(void) {
    uint64_t h = 0x243F6A8885A308D3ull;
    int i;
    h = mix64(h ^ cp[0x50] ^ ((uint64_t)cp[0x60] << 32));
    for (i = 0; i < 8; i++) h = mix64(h ^ cp[0x70 + i] ^ ((uint64_t)cp[0x80 + i] << 32) ^ ((uint64_t)cp[0x90 + i] << 16));
    for (i = 0; i < 12; i++) h = mix64(h ^ cp[0xA0 + i] ^ ((uint64_t)cp[0xB0 + i] << 32));
    return mix64(h ^ xf[0x1018] ^ ((uint64_t)xf[0x1019] << 32));
}

/* The vertices of the draws in list p[0..len), as run would draw them;
 * -1 if implausibly many. */
static int list_vertex_count(const uint8_t* p, uint32_t len) {
    uint32_t saved[0x98 - 0x50];
    uint32_t pos = 0;
    int64_t total = 0;
    memcpy(saved, cp + 0x50, sizeof(saved));
    while (pos < len) {
        uint32_t left = len - pos;
        uint8_t op = p[pos];
        if (op == 0x08) {
            if (left < 6) break;
            if (p[pos + 1] >= 0x50 && p[pos + 1] < 0x98) cp[p[pos + 1]] = rd32(p + pos + 2);
            pos += 6;
        } else if (op == 0x10) {
            if (left < 5) break;
            pos += 5 + 4 * (((rd32(p + pos + 1) >> 16) & 15) + 1);
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 || op == 0x61) {
            pos += 5;
        } else if (op == 0x40) {
            pos += 9;
        } else if (op >= 0x80 && op < 0xC0) {
            const VtxFormat* f;
            int count;
            if (left < 3) break;
            f = format(op & 7);
            count = rd16(p + pos + 1);
            if (left < 3 + (uint32_t)count * (uint32_t)f->size) break;
            total += count;
            pos += 3 + (uint32_t)count * (uint32_t)f->size;
        } else {
            pos++;
        }
    }
    memcpy(cp + 0x50, saved, sizeof(saved));
    return total > (1 << 22) ? -1 : (int)total;
}

/* Notes the parts of the arrays the draw's count vertices at p index. */
static void list_note_arrays(const uint8_t* p, const VtxFormat* f, int count) {
    int a, i;
    for (a = 0; a < 12; a++) {
        int type, off, elem;
        uint32_t lo = 0xFFFF, hi = 0, base, stride;
        if (a == 0) type = f->pos, off = f->off_pos, elem = f->pos_cs * f->pos_n;
        else if (a == 1) type = f->nrm, off = f->off_nrm, elem = f->nrm_cs * 3;
        else if (a < 4) type = f->col[a - 2], off = f->off_col[a - 2], elem = color_size(f->col_fmt[a - 2]);
        else type = f->tex[a - 4], off = f->off_tex[a - 4], elem = f->tex_cs[a - 4] * f->tex_n[a - 4];
        if (type < ATTR_INDEX8) continue;
        for (i = 0; i < count; i++) {
            const uint8_t* q = p + (size_t)i * f->size + off;
            uint32_t k = type == ATTR_INDEX8 ? q[0] : rd16(q);
            if (k < lo) lo = k;
            if (k > hi) hi = k;
        }
        base = cp[0xA0 + a] & 0x3FFFFFFF;
        stride = cp[0xB0 + a] & 0xFF;
        if (sRangeCount == sRangeCap) {
            sRangeCap = sRangeCap ? sRangeCap * 2 : 64;
            sRanges = (ListRange*)realloc(sRanges, (size_t)sRangeCap * sizeof(ListRange));
        }
        sRanges[sRangeCount].addr = base + lo * stride;
        sRanges[sRangeCount].len = (hi - lo) * stride + (uint32_t)elem;
        sRangeCount++;
    }
}

static int range_order(const void* a, const void* b) {
    uint32_t x = ((const ListRange*)a)->addr, y = ((const ListRange*)b)->addr;
    return x < y ? -1 : x > y;
}

static int list_ranges_same(const ListEntry* e) {
    int i;
    for (i = 0; i < e->nranges; i++) {
        if (hash_bytes64(MA(e->ranges[i].addr), e->ranges[i].len) != e->ranges[i].hash) return 0;
    }
    return 1;
}

static void list_drop_vertices(ListEntry* e) {
    if (e->first >= 0) arena_free_later(e->first, e->verts);
    e->first = -1;
    e->verts = 0;
    e->valid = 0;
}

/* Before list addr (size bytes) runs: its entry drawn from, or decoded
 * into; neither (sList NULL) for a list that changes from call to call,
 * whose draws go to this frame's spans. */
static void list_begin(uint32_t addr, uint32_t size) {
    uint64_t state, key, hash = 0;
    ListEntry *set, *e = NULL;
    int i, count;
    sList = NULL;
    if (!sArena || (long)sFrame == sDebugFrame) return;
    state = list_state();
    key = mix64(state ^ ((uint64_t)addr << 32) ^ size);
    set = &sLists[(key % LIST_SETS) * LIST_WAYS];
    for (i = 0; i < LIST_WAYS && !e; i++) {
        if (set[i].size == size && set[i].addr == addr && set[i].state == state) e = &set[i];
    }
    if (e) {
        e->used = sFrame;
        /* One that has kept changing is only checked now and then. */
        if (e->changed >= LIST_CHANGING && (sFrame & 63) != 0) return;
        hash = hash_bytes64(MA(addr), size);
        if (e->valid && hash == e->hash && list_ranges_same(e)) {
            e->changed = 0;
            sList = e;
            sListBuild = 0;
            sListAt = 0;
            return;
        }
        e->changed++;
        list_drop_vertices(e);
    } else {
        e = &set[0];
        for (i = 1; i < LIST_WAYS; i++) {
            if (!set[i].size) {
                e = &set[i];
                break;
            }
            if (set[i].used < e->used) e = &set[i];
        }
        if (e->size) list_drop_vertices(e);
        e->addr = addr;
        e->size = size;
        e->state = state;
        e->first = -1;
        e->verts = 0;
        e->valid = 0;
        e->changed = 0;
        e->used = sFrame;
        hash = hash_bytes64(MA(addr), size);
    }
    count = list_vertex_count(MA(addr), size);
    if (count < 0) {
        e->changed = LIST_UNCACHED;
        return;
    }
    e->hash = hash;
    e->first = count ? gxb_arena_alloc(count) : -1;
    e->verts = count;
    if (count > sListVertsCap) {
        sListVertsCap = count * 2;
        sListVerts = (GXVertex*)realloc(sListVerts, (size_t)sListVertsCap * sizeof(GXVertex));
    }
    sList = e;
    sListBuild = 1;
    sListAt = 0;
    sListMax = count;
    sRangeCount = 0;
}

/* After the list runs: a decoded one's vertices written to the arena,
 * and the parts of arrays it read noted. */
static void list_end(void) {
    ListEntry* e = sList;
    int i, n = 0, bad = 0;
    sList = NULL;
    if (!e || !sListBuild) return;
    if (sListAt) gxb_arena_write(e->first, sListVerts, sListAt);
    if (sListAt < sListMax) {
        /* Never drawn from: free at once. */
        gxb_arena_free(e->first + sListAt, sListMax - sListAt);
        e->verts = sListAt;
        if (!sListAt) e->first = -1;
    }
    if (sRangeCount > 1) qsort(sRanges, (size_t)sRangeCount, sizeof(ListRange), range_order);
    for (i = 0; i < sRangeCount; i++) {
        if (n && sRanges[i].addr <= sRanges[n - 1].addr + sRanges[n - 1].len + 64) {
            uint32_t end = sRanges[i].addr + sRanges[i].len;
            if (end > sRanges[n - 1].addr + sRanges[n - 1].len) sRanges[n - 1].len = end - sRanges[n - 1].addr;
        } else {
            sRanges[n++] = sRanges[i];
        }
    }
    if (n > e->rangecap) {
        e->rangecap = n;
        e->ranges = (ListRange*)realloc(e->ranges, (size_t)n * sizeof(ListRange));
    }
    e->nranges = n;
    for (i = 0; i < n; i++) {
        e->ranges[i] = sRanges[i];
        /* A range that big is likely an index of 0xFFFF, not an array. */
        if (sRanges[i].len > (4u << 20)) bad = 1;
        else e->ranges[i].hash = hash_bytes64(MA(sRanges[i].addr), sRanges[i].len);
    }
    if (bad) {
        e->changed = LIST_UNCACHED;
        list_drop_vertices(e);
    } else {
        e->valid = 1;
    }
}

/* An arena span of n vertices for this frame alone. */
static int transient(int n) {
    if (n > sTransLeft) {
        int size = n > 16384 ? n : 16384;
        sTransNext = gxb_arena_alloc(size);
        sTransLeft = size;
        arena_free_later(sTransNext, size);
    }
    sTransNext += n;
    sTransLeft -= n;
    return sTransNext - n;
}

/* At the end of a frame: spans no recorded frame draws from any more let
 * go of, and lists not called for a while forgotten. */
static void list_frame_end(void) {
    static int sweep;
    int i;
    for (i = 0; i < sLateCount;) {
        if (sLate[i].frame + 3 <= sFrame) {
            gxb_arena_free(sLate[i].first, sLate[i].n);
            sLate[i] = sLate[--sLateCount];
        } else i++;
    }
    for (i = 0; i < 64; i++) {
        ListEntry* e = &sLists[sweep];
        sweep = (sweep + 1) % (LIST_SETS * LIST_WAYS);
        if (e->size && e->used + 600 < sFrame) {
            list_drop_vertices(e);
            e->size = 0;
        }
    }
    sTransLeft = 0;
}

/* Draws count vertices of format f from p as primitive prim. */
static void draw(int prim, const uint8_t* p, const VtxFormat* f, int count, int vat) {
    static int skipLo = -2, skipHi = -1;
    GXVertex tpl, *v;
    uint32_t* ix;
    uint32_t base;
    int tris, i, own = 0;
    switch (prim) {
    case 0x80: /* quads */
    case 0x88: /* quads (alternate encoding) */
        tris = count / 4 * 2;
        break;
    case 0x90: tris = count / 3; break; /* triangles */
    case 0x98: /* triangle strip */
    case 0xA0: /* triangle fan */
        tris = count - 2;
        break;
    default: return; /* lines and points are not drawn yet */
    }
    if (tris <= 0) return;
    if (skipLo == -2) {
        /* RECOMP_GX_SKIP=<first>-<last>: drop those draws (numbered from 0
         * in each frame), to find which draws make up something on
         * screen. */
        const char* e = getenv("RECOMP_GX_SKIP");
        skipLo = -1;
        if (e && sscanf(e, "%d-%d", &skipLo, &skipHi) != 2) skipLo = skipHi = -1;
    }
    if ((int)sDrawCount >= skipLo && (int)sDrawCount <= skipHi) {
        sDrawCount++;
        return;
    }
    use_current_state(f->pnmtx | (f->texmtx_mask ? 2 : 0), p, f, count);
    if ((sArena ? 0 : sBatchVerts) + count > sBatchCap) {
        sBatchCap = ((sArena ? 0 : sBatchVerts) + count) * 2;
        sBatch = (GXVertex*)realloc(sBatch, (size_t)sBatchCap * sizeof(GXVertex));
    }
    if (sIndexCount + 3 * tris > sIndexCap) {
        sIndexCap = (sIndexCount + 3 * tris) * 2;
        sIndex = (uint32_t*)realloc(sIndex, (size_t)sIndexCap * sizeof(uint32_t));
    }
    {
        /* In a display list, which one and which draw in it; otherwise
         * (particles, 2D) what is drawn: format, size and texture. */
        uint64_t k = sDisplayList ? ((uint64_t)sDisplayList << 24) ^ sListDraws++
                                  : 0x5A5A000000000000ull ^ ((uint64_t)bp[0x94] << 24) ^ ((uint64_t)prim << 16) ^
                                        ((uint64_t)vat << 20) ^ (uint64_t)count;
        sBatchState.key = mix64(sBatchState.key ^ k);
    }
    if (!sArena) {
        v = sBatch + sBatchVerts;
        base = (uint32_t)(sBatchVerts - sBatchFirst);
    } else if (sList && sListAt + count <= sList->verts) {
        /* The list's, in the arena: decoded now (sListBuild), or before. */
        v = sListBuild ? sListVerts + sListAt : NULL;
        base = (uint32_t)(sList->first + sListAt);
        sListAt += count;
        if (sListBuild) list_note_arrays(p, f, count);
    } else {
        v = sBatch;
        base = (uint32_t)transient(count);
        own = 1;
    }
    if (v) {
        vertex_template(&tpl);
        for (i = 0; i < count; i++) v[i] = tpl;
        read_vertices(p, f, v, count);
        if ((long)sFrame == sDebugFrame) debug_draw(&sBatchState, v, count);
        if (own) gxb_arena_write((int)base, v, count);
    }
    ix = sIndex + sIndexCount;
    switch (prim) {
    case 0x80:
    case 0x88:
        for (i = 0; i + 3 < count; i += 4) {
            *ix++ = base + i; *ix++ = base + i + 1; *ix++ = base + i + 2;
            *ix++ = base + i; *ix++ = base + i + 2; *ix++ = base + i + 3;
        }
        break;
    case 0x90:
        for (i = 0; i + 2 < count; i += 3) {
            *ix++ = base + i; *ix++ = base + i + 1; *ix++ = base + i + 2;
        }
        break;
    case 0x98: /* every other triangle turned round, to keep the winding */
        for (i = 0; i + 2 < count; i++) {
            if (i & 1) { *ix++ = base + i + 1; *ix++ = base + i; *ix++ = base + i + 2; }
            else { *ix++ = base + i; *ix++ = base + i + 1; *ix++ = base + i + 2; }
        }
        break;
    default:
        for (i = 1; i + 1 < count; i++) {
            *ix++ = base; *ix++ = base + i; *ix++ = base + i + 1;
        }
        break;
    }
    if (!sArena) sBatchVerts += count;
    sIndexCount = (int)(ix - sIndex);
    sDrawCount++;
}

/* ------------------------------------------------------------------------ */
/* Register writes */

static void load_xf(uint32_t addr, const uint8_t* data, int count) {
    int i;
    for (i = 0; i < count; i++) {
        xf_write(addr + (uint32_t)i, rd32(data + 4 * i));
    }
}

static void load_indexed_xf(int n, uint32_t value) {
    uint32_t index = value >> 16, len = ((value >> 12) & 15) + 1, addr = value & 0xFFF, i;
    const uint8_t* src = array_ptr(12 + n, index);
    for (i = 0; i < len; i++) {
        xf_write(addr + i, rd32(src + 4 * i));
    }
}

static void load_tlut(void) {
    uint32_t src = (bp[0x64] & 0xFFFFFF) << 5;
    uint32_t dst = (bp[0x65] & 0x3FF) << 9;
    uint32_t bytes = ((bp[0x65] >> 10) & 0x7FF) * 32;
    if (dst + bytes > sizeof(tmem)) bytes = sizeof(tmem) - dst;
    memcpy(&tmem[dst], MA(src), bytes);
}

static void set_tev_color(int reg, uint32_t v) {
    int i = (reg - 0xE0) / 2, high = (reg - 0xE0) & 1, konst = (v >> 23) & 1;
    int16_t (*dst)[4] = konst ? tev_konst : tev_color;
    int lo = (int)(v & 0x7FF), hi = (int)((v >> 12) & 0x7FF);
    if (lo & 0x400) lo -= 0x800;
    if (hi & 0x400) hi -= 0x800;
    if (!high) {
        dst[i][0] = (int16_t)lo; /* R */
        dst[i][3] = (int16_t)hi; /* A */
    } else {
        dst[i][2] = (int16_t)lo; /* B */
        dst[i][1] = (int16_t)hi; /* G */
    }
}

static void efb_copy(uint32_t exec) {
    uint32_t tl = bp[0x49], wh = bp[0x4A];
    int x = (int)(tl & 0x3FF), y = (int)((tl >> 10) & 0x3FF);
    int w = (int)(wh & 0x3FF) + 1, h = (int)((wh >> 10) & 0x3FF) + 1;
    int clear = (exec >> 11) & 1, to_xfb = (exec >> 14) & 1;
    uint32_t dst = (bp[0x4B] & 0xFFFFFF) << 5;
    if ((long)sFrame == sDebugFrame) {
        fprintf(stderr, "efb copy %dx%d at (%d,%d) to %s %08X, exec %08X, clear %d\n", w, h, x, y, to_xfb ? "XFB" : "texture",
                dst, exec, clear);
    }
    if (to_xfb && (long)sFrame == sDebugFrame) {
        int k;
        fprintf(stderr, "commands in frame:");
        for (k = 0; k < 256; k++) if (sOpCount[k]) fprintf(stderr, " %02X:%u", k, sOpCount[k]);
        fputc(10, stderr);
    }
    flush_batch();
    if (to_xfb) {
        LOG("frame %llu: copy to XFB %dx%d at (%d,%d), %u draws, %u vertices", (unsigned long long)sFrame, w, h, x, y,
            sDrawCount, sVertexCount);
        if (sKeepFrame) gxb_frame_data(sArena ? NULL : sBatch, sBatchVerts, sIndex, sIndexCount);
        if (sArena) list_frame_end();
        gxb_present(x, y, w, h);
        sBatchVerts = sIndexCount = sBatchFirst = sIndexFirst = 0;
        sKeepFrame = gxb_recording();
        sFrame++;
        note_writes((uint32_t)sFrame);
        {
            /* The filtering setting changed: every texture made again. */
            static int generation;
            int t;
            if (gxb_texture_generation() != generation) {
                generation = gxb_texture_generation();
                for (t = 0; t < TEX_CACHE; t++) {
                    if (sTexCache[t].handle) gxb_texture_free(sTexCache[t].handle);
                    sTexCache[t].handle = 0;
                }
            }
        }
        sDrawCount = sVertexCount = 0;
        sStateDirty = 1; /* textures are checked again each frame */
        {
            int mx, my;
            gx_canvas(&mx, &my);
            sCanvasOn = mx || my;
        }
    } else {
        int fmt = (int)(((exec >> 4) & 7) | (((exec >> 3) & 1) << 3));
        gxb_copy_to_texture(dst, x, y, w, h, fmt, (exec >> 15) & 1, (bp[0x43] & 7) == 1, (int)(bp[0x4D] & 0x3FF) * 32);
    }
    if (clear) {
        uint32_t rgba = ((bp[0x4F] & 0xFF) << 24) | (((bp[0x50] >> 8) & 0xFF) << 16) | ((bp[0x50] & 0xFF) << 8) |
                        ((bp[0x4F] >> 8) & 0xFF);
        uint32_t blend = bp[0x41], zmode = bp[0x40];
        gxb_clear((blend >> 3) & 1, (blend >> 4) & 1, (zmode >> 4) & 1, rgba, bp[0x51] & 0xFFFFFF);
    }
}

static void bp_write(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) {
        bp_mask = val;
        return;
    }
    val = (bp[reg] & ~bp_mask) | (val & bp_mask);
    bp_mask = 0xFFFFFF;
    if (bp[reg] != val) sStateDirty = 1;
    bp[reg] = val;
    if (reg >= 0xE0 && reg <= 0xE7) set_tev_color((int)reg, val);
    else if (reg == 0x65) load_tlut();
    else if (reg == 0x52) efb_copy(val);
    else if (reg == 0x45) hle_raise_interrupt(INTERRUPT_PE_FINISH); /* draw done */
    else if (reg == 0x47) ST16(PE_TOKEN_REG, val & 0xFFFF);
    else if (reg == 0x48) hle_raise_interrupt_ex(INTERRUPT_PE_TOKEN, set_token, val & 0xFFFF);
}

/* ------------------------------------------------------------------------ */
/* Command parsing */

static unsigned sErrors;

/* Decodes complete commands from p[0..len) and returns the bytes used.
 * *need is set to the size of the incomplete command left at the end. */
static size_t run(const uint8_t* p, size_t len, int depth, size_t* need) {
    size_t pos = 0;
    *need = 1;
    while (pos < len) {
        const uint8_t* c = p + pos;
        size_t left = len - pos;
        uint8_t op = c[0];
        if ((long)sFrame == sDebugFrame && depth >= 0) sOpCount[op]++;
        if (op == 0x00 || op == 0x48 || op == 0x44) {
            pos++;
        } else if (op == 0x08) {
            if (left < 6) { *need = 6; break; }
            cp[c[1]] = rd32(c + 2);
            if (c[1] == 0xF0) { /* not the GPU's: hle_gx.c's mark */
                sScreenMarked = (int)cp[0xF0];
                sMapDrawn = 0;
            }
            if (c[1] == 0x30) xf_write(0x1018, cp[0x30]);
            if (c[1] == 0x40) xf_write(0x1019, cp[0x40]);
            pos += 6;
        } else if (op == 0x10) {
            uint32_t h, n;
            if (left < 5) { *need = 5; break; }
            h = rd32(c + 1);
            n = ((h >> 16) & 15) + 1;
            if (left < 5 + 4 * n) { *need = 5 + 4 * n; break; }
            load_xf(h & 0xFFFF, c + 5, (int)n);
            pos += 5 + 4 * n;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
            if (left < 5) { *need = 5; break; }
            load_indexed_xf((op - 0x20) >> 3, rd32(c + 1));
            pos += 5;
        } else if (op == 0x40) {
            uint32_t addr, size;
            if (left < 9) { *need = 9; break; }
            addr = rd32(c + 1) & 0x3FFFFFFF;
            size = rd32(c + 5);
            if (depth == 0 && size < (16u << 20)) {
                size_t dl_need;
                uint32_t list = sDisplayList, draws = sListDraws;
                sDisplayList = addr;
                sListDraws = 0;
                list_begin(addr, size);
                run(MA(addr), size, 1, &dl_need);
                list_end();
                sDisplayList = list;
                sListDraws = draws;
            }
            pos += 9;
        } else if (op == 0x61) {
            if (left < 5) { *need = 5; break; }
            bp_write(rd32(c + 1));
            pos += 5;
        } else if (op >= 0x80 && op < 0xC0) {
            const VtxFormat* f;
            int count;
            if (left < 3) { *need = 3; break; }
            f = format(op & 7);
            count = rd16(c + 1);
            if (left < 3 + (size_t)count * f->size) { *need = 3 + (size_t)count * f->size; break; }
            sVertexCount += (unsigned)count;
            draw(op & 0xF8, c + 3, f, count, op & 7);
            pos += 3 + (size_t)count * f->size;
        } else {
            if (sErrors++ < 20) {
                fprintf(stderr, "gx: unknown command %02X at offset %zu (%s); skipping a byte\n", op, pos,
                        depth ? "display list" : "FIFO");
            }
            if (sErrors == 1) {
                size_t from = pos > 96 ? pos - 96 : 0, k;
                for (k = from; k < pos + 32 && k < len; k++) {
                    fprintf(stderr, "%s%02X", (k - from) % 16 == 0 ? "\n  " : " ", p[k]);
                }
                fputc('\n', stderr);
            }
            pos++;
        }
    }
    return pos;
}

void gx_flush(void) {
    size_t used;
    if (!sReady || !sFifoLen) return;
    used = run(sFifo, sFifoLen, 0, &sNeed);
    memmove(sFifo, sFifo + used, sFifoLen - used);
    sFifoLen -= used;
}

void gx_init(void) {
    const char* v = getenv("RECOMP_GX_LOG");
    sLog = v && *v && *v != '0';
    if ((v = getenv("RECOMP_GX_DEBUG_FRAME"))) sDebugFrame = atol(v);
    bp[0x41] = 0x18; /* color and alpha updates on */
    gxb_init();
    /* RECOMP_GX_LISTS=0: display lists decoded every time they're called,
     * and every draw's vertices sent with it. */
    v = getenv("RECOMP_GX_LISTS");
    sArena = gxb_arena() && !(v && *v == '0') && !getenv("RECOMP_GX_SKIP");
    g_wgpipe_write = fifo_append;
    g_gx_flush = gx_flush;
    g_gx_copy_sync = gxb_copy_sync;
    sReady = 1;
}
