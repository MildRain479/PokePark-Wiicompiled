/* Making a language pack (langpack.h) in the game, as
 * tools/recomp/langpack.py does on a PC: the same steps, in C.
 *
 *   text/files: the USA archives (.dac, .dan, .arc: LZ11 around U8) that
 *   carry text, rebuilt with each message file (Message/<name>.bmg)
 *   swapped for the other disc's (Message/<language>/<name>.bmg), the
 *   font for the other disc's, and the menus' pictures with words on them
 *   (Menu/.../*.brres textures of the same name and shape) for the other
 *   disc's. Everything else stays the USA game's: the other disc is a
 *   different build, whose data doesn't fit the USA program. A message
 *   file with fewer messages keeps the USA's extra ones; the staff roll,
 *   a different list, is matched message by message (STAFF_ROLL).
 *
 *   cries/files/Sound/Delhi.brsar: the other disc's sound archive, if its
 *   tables are the USA one's (the same sounds in the same order), whose
 *   Pokemon voices are that language's.
 *
 * Written to <pack>.new, then put in the pack's place: at once if the game
 * isn't using the pack, or else as the game next starts
 * (langpack_promote). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_settings.h"
#include "langpack.h"

/* ------------------------------------------------------------------------ */
/* Bytes */

typedef struct {
    uint8_t* p;
    size_t n, cap;
} Buf;

static int buf_reserve(Buf* b, size_t more) {
    if (b->n + more <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + more) cap *= 2;
    uint8_t* p = (uint8_t*)realloc(b->p, cap);
    if (!p) return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int buf_add(Buf* b, const void* data, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    if (data) memcpy(b->p + b->n, data, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
    return 1;
}

static void buf_pad(Buf* b, size_t to) {
    while (b->n % to) buf_add(b, NULL, 1);
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* ------------------------------------------------------------------------ */
/* LZ11 */

static int lz11_decompress(const uint8_t* src, size_t n, Buf* out) {
    size_t size, i = 4;
    if (n < 4 || src[0] != 0x11) return 0;
    size = src[1] | src[2] << 8 | (size_t)src[3] << 16;
    if (size == 0) {
        if (n < 8) return 0;
        size = src[4] | src[5] << 8 | (size_t)src[6] << 16 | (size_t)src[7] << 24;
        i = 8;
    }
    out->n = 0;
    if (!buf_reserve(out, size)) return 0;
    while (out->n < size) {
        int bit, flags;
        if (i >= n) return 0;
        flags = src[i++];
        for (bit = 0; bit < 8 && out->n < size; bit++) {
            size_t len, disp, k;
            if (!(flags & (0x80 >> bit))) {
                if (i >= n) return 0;
                out->p[out->n++] = src[i++];
                continue;
            }
            if (i + 1 >= n) return 0;
            switch (src[i] >> 4) {
            case 0:
                if (i + 2 >= n) return 0;
                len = ((src[i] & 15) << 4 | src[i + 1] >> 4) + 0x11;
                disp = ((src[i + 1] & 15) << 8 | src[i + 2]) + 1;
                i += 3;
                break;
            case 1:
                if (i + 3 >= n) return 0;
                len = ((size_t)(src[i] & 15) << 12 | src[i + 1] << 4 | src[i + 2] >> 4) + 0x111;
                disp = ((src[i + 2] & 15) << 8 | src[i + 3]) + 1;
                i += 4;
                break;
            default:
                len = (src[i] >> 4) + 1;
                disp = ((src[i] & 15) << 8 | src[i + 1]) + 1;
                i += 2;
            }
            if (disp > out->n || out->n + len > size) return 0;
            for (k = 0; k < len; k++, out->n++) out->p[out->n] = out->p[out->n - disp];
        }
    }
    return 1;
}

#define HASH_BITS 16
static uint32_t hash3(const uint8_t* p) { return ((uint32_t)(p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u) >> (32 - HASH_BITS); }

/* Greedy LZ11 with hash chains over the 4 KB window. */
static int lz11_compress(const uint8_t* data, size_t n, Buf* out) {
    int32_t* head = (int32_t*)malloc(sizeof(int32_t) << HASH_BITS);
    int32_t* prev = (int32_t*)malloc(sizeof(int32_t) * (n ? n : 1));
    size_t i = 0, flag_pos = 0, p;
    int bit = 8;
    uint8_t hdr[8] = {0x11, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16)};
    if (!head || !prev) {
        free(head);
        free(prev);
        return 0;
    }
    memset(head, 0xFF, sizeof(int32_t) << HASH_BITS);
    out->n = 0;
    if (n >= 1u << 24) {
        hdr[1] = hdr[2] = hdr[3] = 0;
        hdr[4] = (uint8_t)n, hdr[5] = (uint8_t)(n >> 8), hdr[6] = (uint8_t)(n >> 16), hdr[7] = (uint8_t)(n >> 24);
        buf_add(out, hdr, 8);
    } else {
        buf_add(out, hdr, 4);
    }
#define INSERT(q)                                \
    do {                                         \
        if ((q) + 3 <= n) {                      \
            uint32_t h_ = hash3(data + (q));     \
            prev[q] = head[h_];                  \
            head[h_] = (int32_t)(q);             \
        }                                        \
    } while (0)
    while (i < n) {
        size_t best_len = 0, best_disp = 0;
        if (bit == 8) {
            if (!buf_reserve(out, 1 + 8 * 4)) break;
            flag_pos = out->n;
            out->p[out->n++] = 0;
            bit = 0;
        }
        if (i + 3 <= n) {
            int32_t cand = head[hash3(data + i)];
            size_t limit = n - i < 0x10110 ? n - i : 0x10110;
            int tries = 48;
            while (cand >= 0 && i - (size_t)cand <= 0x1000 && tries--) {
                size_t len = 0;
                while (len < limit && data[cand + len] == data[i + len]) len++;
                if (len > best_len) {
                    best_len = len;
                    best_disp = i - (size_t)cand;
                    if (len == limit) break;
                }
                cand = prev[cand];
            }
        }
        if (best_len >= 3) {
            size_t d = best_disp - 1;
            uint8_t c[4];
            out->p[flag_pos] |= (uint8_t)(0x80 >> bit);
            if (best_len <= 0x10) {
                c[0] = (uint8_t)((best_len - 1) << 4 | d >> 8), c[1] = (uint8_t)d;
                buf_add(out, c, 2);
            } else if (best_len <= 0x110) {
                size_t m = best_len - 0x11;
                c[0] = (uint8_t)(m >> 4), c[1] = (uint8_t)((m & 15) << 4 | d >> 8), c[2] = (uint8_t)d;
                buf_add(out, c, 3);
            } else {
                size_t m = best_len - 0x111;
                c[0] = (uint8_t)(0x10 | m >> 12), c[1] = (uint8_t)(m >> 4), c[2] = (uint8_t)((m & 15) << 4 | d >> 8),
                c[3] = (uint8_t)d;
                buf_add(out, c, 4);
            }
            for (p = i; p < i + best_len; p++) INSERT(p);
            i += best_len;
        } else {
            buf_add(out, data + i, 1);
            INSERT(i);
            i++;
        }
        bit++;
    }
#undef INSERT
    free(head);
    free(prev);
    buf_pad(out, 4);
    return i >= n;
}

/* ------------------------------------------------------------------------ */
/* U8 */

static const uint8_t kU8[4] = {0x55, 0xAA, 0x38, 0x2D};

typedef struct {
    char path[256];
    uint32_t node, off, size;
} U8File;

static int is_u8(const uint8_t* p, size_t n) { return n >= 16 && !memcmp(p, kU8, 4); }

/* Every file of an archive, or NULL (count in *count). */
static U8File* u8_files(const uint8_t* d, size_t len, int* count) {
    uint32_t root = be32(d + 4), n, strings, i;
    uint32_t ends[32];
    char names[32][128];
    int depth = 0, cap = 64;
    U8File* files;
    *count = 0;
    if (root + 12 > len) return NULL;
    n = be32(d + root + 8);
    strings = root + n * 12;
    if (strings > len) return NULL;
    files = (U8File*)malloc(sizeof(U8File) * cap);
    for (i = 1; files && i < n; i++) {
        const uint8_t* e = d + root + i * 12;
        uint32_t t = be32(e), name = strings + (t & 0xFFFFFF);
        char nm[128];
        while (depth && i >= ends[depth - 1]) depth--;
        snprintf(nm, sizeof(nm), "%s", name < len ? (const char*)d + name : "");
        if (t >> 24) {
            if (depth < 32) {
                ends[depth] = be32(e + 8);
                snprintf(names[depth], sizeof(names[depth]), "%s", nm);
                depth++;
            }
        } else {
            U8File* f;
            int k;
            size_t at = 0;
            if (*count == cap) {
                U8File* more = (U8File*)realloc(files, sizeof(U8File) * (cap *= 2));
                if (!more) break;
                files = more;
            }
            f = &files[(*count)++];
            f->path[0] = '\0';
            for (k = 0; k < depth; k++) {
                if (!names[k][0] || !strcmp(names[k], ".")) continue;
                at += snprintf(f->path + at, sizeof(f->path) - at, "%s/", names[k]);
                if (at >= sizeof(f->path)) at = sizeof(f->path) - 1;
            }
            snprintf(f->path + at, sizeof(f->path) - at, "%s", nm);
            f->node = i;
            f->off = be32(e + 4);
            f->size = be32(e + 8);
            if ((uint64_t)f->off + f->size > len) (*count)--;
        }
    }
    return files;
}

static int by_offset(const void* a, const void* b) {
    uint32_t x = ((const U8File*)a)->off, y = ((const U8File*)b)->off;
    return x < y ? -1 : x > y;
}

/* The archive with replace[node] (if its data is set) in place of its
 * files; the node and name tables stay, the data is laid out again in its
 * old order, each file on 32 bytes. */
static int u8_replace(const uint8_t* d, size_t len, U8File* files, int count, Buf* replace, Buf* out) {
    uint32_t root = be32(d + 4), doff = be32(d + 12);
    int i;
    (void)len;
    out->n = 0;
    if (!buf_add(out, d, doff)) return 0;
    qsort(files, count, sizeof(U8File), by_offset);
    for (i = 0; i < count; i++) {
        const uint8_t* body = d + files[i].off;
        size_t size = files[i].size;
        if (replace[i].p) body = replace[i].p, size = replace[i].n;
        buf_pad(out, 32);
        put32(out->p + root + files[i].node * 12 + 4, (uint32_t)out->n);
        put32(out->p + root + files[i].node * 12 + 8, (uint32_t)size);
        if (!buf_add(out, body, size)) return 0;
    }
    buf_pad(out, 32);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* BMG */

typedef struct {
    int count;
    Buf sec[8];
} Bmg;

static void bmg_free(Bmg* m) {
    int i;
    for (i = 0; i < m->count; i++) free(m->sec[i].p);
    m->count = 0;
}

static int bmg_parse(const uint8_t* b, size_t n, Bmg* m) {
    uint32_t count, off = 0x20, i;
    memset(m, 0, sizeof(*m));
    if (n < 0x20 || memcmp(b, "MESGbmg1", 8)) return 0;
    count = be32(b + 0xC);
    for (i = 0; i < count && i < 8; i++) {
        uint32_t size;
        if (off + 8 > n) break;
        size = be32(b + off + 4);
        if (size < 8 || off + size > n) break;
        buf_add(&m->sec[m->count++], b + off, size);
        off += size;
    }
    return m->count > 0;
}

static Buf* bmg_sec(Bmg* m, const char* magic) {
    int i;
    for (i = 0; i < m->count; i++)
        if (!memcmp(m->sec[i].p, magic, 4)) return &m->sec[i];
    return NULL;
}

static void bmg_build(const uint8_t* header, Bmg* m, Buf* out) {
    int i;
    out->n = 0;
    buf_add(out, header, 0x20);
    for (i = 0; i < m->count; i++) {
        buf_pad(&m->sec[i], 32);
        put32(m->sec[i].p + 4, (uint32_t)m->sec[i].n);
        buf_add(out, m->sec[i].p, m->sec[i].n);
    }
    put32(out->p + 8, (uint32_t)out->n);
    put32(out->p + 0xC, (uint32_t)m->count);
}

static int bmg_count(const uint8_t* b, size_t n) {
    Bmg m;
    Buf* inf;
    int c = 0;
    if (!bmg_parse(b, n, &m)) return 0;
    if ((inf = bmg_sec(&m, "INF1")) != NULL && inf->n >= 16) c = be16(inf->p + 8);
    bmg_free(&m);
    return c;
}

/* The end (past its terminator) of the text at p in a DAT1 section: tags
 * (0x1A, then a length byte) may hold zero bytes, so they're walked. */
static size_t text_end(const Buf* dat, size_t p) {
    while (p + 2 <= dat->n) {
        unsigned ch = dat->p[p] << 8 | dat->p[p + 1];
        if (ch == 0) return p + 2;
        if (ch == 0x1A) {
            if (p + 2 >= dat->n || dat->p[p + 2] < 2) break;
            p += dat->p[p + 2];
        } else {
            p += 2;
        }
    }
    return dat->n;
}

/* Message i's text, as stored with its terminator, from a parsed file. */
static int bmg_message(Bmg* m, int i, const uint8_t** text, size_t* n) {
    Buf *inf = bmg_sec(m, "INF1"), *dat = bmg_sec(m, "DAT1");
    unsigned es, p;
    if (!inf || !dat || i >= be16(inf->p + 8)) return 0;
    es = be16(inf->p + 10);
    if (16 + (i + 1) * es > inf->n) return 0;
    p = 8 + be32(inf->p + 16 + i * es);
    if (p >= dat->n) return 0;
    *text = dat->p + p;
    *n = text_end(dat, p) - p;
    return 1;
}

/* other with usa's messages past its last appended (their text added to
 * the end of other's DAT1). */
static int bmg_fill(const uint8_t* other, size_t on, const uint8_t* usa, size_t un, Buf* out) {
    Bmg o, u;
    Buf *inf, *dat, *mid, *uinf, *udat, *umid;
    Buf text = {0}, entries = {0};
    unsigned have, es, want, i;
    int ok = 0;
    if (!bmg_parse(other, on, &o)) return 0;
    if (!bmg_parse(usa, un, &u)) {
        bmg_free(&o);
        return 0;
    }
    inf = bmg_sec(&o, "INF1"), dat = bmg_sec(&o, "DAT1"), mid = bmg_sec(&o, "MID1");
    uinf = bmg_sec(&u, "INF1"), udat = bmg_sec(&u, "DAT1"), umid = bmg_sec(&u, "MID1");
    if (inf && dat && uinf && udat && (es = be16(inf->p + 10)) == be16(uinf->p + 10) && es >= 4) {
        have = be16(inf->p + 8);
        want = be16(uinf->p + 8);
        /* DAT1's text ends after the last terminator: it's padded with zeros. */
        buf_add(&text, dat->p + 8, dat->n - 8);
        while (text.n >= 2 && !text.p[text.n - 1] && !text.p[text.n - 2]) text.n -= 2;
        buf_add(&text, NULL, 2);
        buf_add(&entries, inf->p + 16, have * es);
        for (i = have; i < want; i++) {
            uint8_t e[64];
            size_t p = 8 + be32(uinf->p + 16 + i * es), end = text_end(udat, p);
            memcpy(e, uinf->p + 16 + i * es, es < 64 ? es : 64);
            put32(e, (uint32_t)text.n);
            buf_add(&text, udat->p + p, end - p);
            buf_add(&entries, e, es);
        }
        put16(inf->p + 8, (uint16_t)want);
        inf->n = 16;
        buf_add(inf, entries.p, entries.n);
        dat->n = 8;
        buf_add(dat, text.p, text.n);
        if (mid && umid && mid->n >= 16 + have * 4 && umid->n >= 16 + want * 4) {
            put16(mid->p + 8, (uint16_t)want);
            mid->n = 16 + have * 4;
            buf_add(mid, umid->p + 16 + have * 4, (want - have) * 4);
        }
        bmg_build(other, &o, out);
        ok = 1;
    }
    free(text.p);
    free(entries.p);
    bmg_free(&o);
    bmg_free(&u);
    return ok;
}

/* The Japanese staff roll against the USA one: the USA list has ten more
 * headings (37 to 46, the English version's) before the names, and fifty
 * more names at the end (the English version's staff), which stay. Its
 * empty messages are the Japanese voice cast and studios, filled. */
static int staff_rule(int usa, int other, int i) {
    if (usa == 246 && other == 186) return i <= 36 ? i : i >= 47 && i <= 195 ? i - 10 : -1;
    return -2;
}

/* The USA staff roll with each message in the other language where it
 * has one; 0 if the two lists aren't a pair this knows. */
static int staff_roll(const uint8_t* usa, size_t un, const uint8_t* other, size_t on, Buf* out) {
    Bmg u, o;
    Buf *inf, *dat, text = {0};
    int a = bmg_count(usa, un), b = bmg_count(other, on), i, ok = 0;
    unsigned es;
    if (staff_rule(a, b, 0) == -2) return 0;
    if (!bmg_parse(usa, un, &u)) return 0;
    if (!bmg_parse(other, on, &o)) {
        bmg_free(&u);
        return 0;
    }
    inf = bmg_sec(&u, "INF1"), dat = bmg_sec(&u, "DAT1");
    if (inf && dat) {
        Buf old = {0};
        es = be16(inf->p + 10);
        buf_add(&old, dat->p, dat->n);
        buf_add(&text, NULL, 2);
        for (i = 0; i < a; i++) {
            const uint8_t* t = NULL;
            size_t n = 0;
            int j = staff_rule(a, b, i);
            if (!(j >= 0 && bmg_message(&o, j, &t, &n) && n > 2)) {
                size_t p = 8 + be32(inf->p + 16 + i * es);
                t = old.p + p;
                n = text_end(&old, p) - p;
            }
            put32(inf->p + 16 + i * es, (uint32_t)text.n);
            buf_add(&text, t, n);
        }
        dat->n = 8;
        buf_add(dat, text.p, text.n);
        bmg_build(usa, &u, out);
        free(old.p);
        ok = 1;
    }
    free(text.p);
    bmg_free(&u);
    bmg_free(&o);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* BRRES: menu pictures with words on them */

typedef struct {
    char name[64];
    uint32_t at, size, data;
    uint8_t shape[16];
    int shape_len;
} BrresSub;

/* A brres folder's subfiles (count in *count): a texture's shape is its
 * size, format and mipmaps, a palette's its format and count. */
static BrresSub* brres_folder(const uint8_t* b, size_t len, const char* folder, int* count) {
    uint32_t root, g, n, i, fn;
    BrresSub* out = NULL;
    *count = 0;
    if (len < 0x10 || memcmp(b, "bres", 4)) return NULL;
    root = be16(b + 0xC);
    g = root + 8;
    if (g + 8 > len) return NULL;
    fn = be32(b + g + 4);
    for (i = 1; i <= fn; i++) {
        const uint8_t* e = b + g + 8 + i * 16;
        uint32_t nm, sub;
        if (g + 8 + i * 16 + 16 > len) break;
        nm = g + be32(e + 8);
        if (nm >= len || strcmp((const char*)b + nm, folder)) continue;
        sub = g + be32(e + 12);
        if (sub + 8 > len) break;
        n = be32(b + sub + 4);
        out = (BrresSub*)calloc(n ? n : 1, sizeof(BrresSub));
        for (uint32_t k = 1; out && k <= n; k++) {
            const uint8_t* f = b + sub + 8 + k * 16;
            BrresSub* s;
            uint32_t t, fnm;
            if (sub + 8 + k * 16 + 16 > len) break;
            fnm = sub + be32(f + 8);
            t = sub + be32(f + 12);
            if (fnm >= len || t + 0x28 > len) continue;
            s = &out[(*count)++];
            snprintf(s->name, sizeof(s->name), "%s", (const char*)b + fnm);
            s->at = t;
            s->size = be32(b + t + 4);
            s->data = be32(b + t + 0x10);
            s->shape_len = !memcmp(b + t, "TEX0", 4) ? 16 : 6;
            memcpy(s->shape, b + t + 0x18, s->shape_len);
            if ((uint64_t)t + s->size > len || s->data > s->size) (*count)--;
        }
        break;
    }
    return out;
}

static const BrresSub* find_sub(const BrresSub* s, int n, const char* name) {
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(s[i].name, name)) return &s[i];
    return NULL;
}

/* The lobbies' pictures of the attractions (MnAt01Cut01...): photos of
 * play, which differ between the builds but have no words. */
static int is_screenshot(const char* name) { return !strncmp(name, "MnAt", 4) && strstr(name, "Cut"); }

/* A palette entry as RGBA (formats 0 IA8, 1 RGB565, 2 RGB5A3). */
static void color(uint16_t v, uint32_t fmt, int c[4]) {
    if (fmt == 0) {
        c[0] = c[1] = c[2] = v & 255, c[3] = v >> 8;
    } else if (fmt == 1) {
        c[0] = (v >> 11) * 255 / 31, c[1] = (v >> 5 & 63) * 255 / 63, c[2] = (v & 31) * 255 / 31, c[3] = 255;
    } else if (v & 0x8000) {
        c[0] = (v >> 10 & 31) * 255 / 31, c[1] = (v >> 5 & 31) * 255 / 31, c[2] = (v & 31) * 255 / 31, c[3] = 255;
    } else {
        c[0] = (v >> 8 & 15) * 17, c[1] = (v >> 4 & 15) * 17, c[2] = (v & 15) * 17, c[3] = (v >> 12 & 7) * 255 / 7;
    }
}

/* The other disc's paletted texture (C4, C8: tex, in place) and palette
 * (pal, n entries) made to fit a palette of cap entries: the closest two
 * colors merged, again and again, and the texture's indices moved to
 * match. The palette's first entries are rewritten; how many are used is
 * returned, 0 if it can't be done. */
static int repaint(uint8_t* tex, uint32_t tex_len, uint32_t tex_fmt, uint8_t* pal, int n, uint32_t pal_fmt, int cap) {
    int rgba[256][4], alive[256], where[256], remap[256], count = n, i, k, x, y;
    uint16_t kept[256];
    if (n <= cap) return n;
    if ((tex_fmt != 8 && tex_fmt != 9) || n > 256 || cap < 1) return 0;
    for (i = 0; i < n; i++) {
        color(be16(pal + i * 2), pal_fmt, rgba[i]);
        alive[i] = where[i] = i;
    }
    while (count > cap) {
        long best = -1;
        int bx = 0, by = 1, gone;
        for (x = 0; x < count; x++) {
            for (y = x + 1; y < count; y++) {
                long d = 0;
                for (k = 0; k < 4; k++) {
                    long e = rgba[alive[x]][k] - rgba[alive[y]][k];
                    d += e * e;
                }
                if (best < 0 || d < best) best = d, bx = x, by = y;
            }
        }
        gone = alive[by];
        for (i = 0; i < n; i++)
            if (where[i] == gone) where[i] = alive[bx];
        memmove(alive + by, alive + by + 1, (count - by - 1) * sizeof(int));
        count--;
    }
    for (i = 0; i < count; i++) kept[i] = be16(pal + alive[i] * 2);
    for (i = 0; i < n; i++) {
        for (k = 0; k < count && alive[k] != where[i]; k++) {}
        remap[i] = k < count ? k : 0;
    }
    for (i = 0; i < count; i++) put16(pal + i * 2, kept[i]);
    for (uint32_t p = 0; p < tex_len; p++) {
        if (tex_fmt == 9) {
            tex[p] = (uint8_t)(tex[p] < n ? remap[tex[p]] : 0);
        } else {
            int hi = tex[p] >> 4, lo = tex[p] & 15;
            tex[p] = (uint8_t)((hi < n ? remap[hi] : 0) << 4 | (lo < n ? remap[lo] : 0));
        }
    }
    return count;
}

/* usa with the other disc's textures (and their palettes) wherever the two
 * have the same name and shape and differ; how many, 0 for none. Only the
 * pixel data is copied: the headers stay the USA's. A palette of another
 * size is fitted to the USA one's (repaint). */
static int brres_swap(const uint8_t* usa, size_t un, const uint8_t* other, size_t on, Buf* out) {
    int nu, no, npu, npo, i, swapped = 0;
    BrresSub* tu = brres_folder(usa, un, "Textures(NW4R)", &nu);
    BrresSub* to = brres_folder(other, on, "Textures(NW4R)", &no);
    BrresSub* pu = brres_folder(usa, un, "Palettes(NW4R)", &npu);
    BrresSub* po = brres_folder(other, on, "Palettes(NW4R)", &npo);
    uint8_t* tex = NULL;
    out->n = 0;
    if (tu && to) buf_add(out, usa, un);
    for (i = 0; tu && to && i < nu; i++) {
        const BrresSub *t = &tu[i], *o = find_sub(to, no, t->name), *p, *q = NULL;
        uint32_t len = t->size - t->data, plen = 0;
        uint8_t pal[512];
        if (!o || is_screenshot(t->name) || o->shape_len != t->shape_len ||
            memcmp(o->shape, t->shape, t->shape_len) || o->size - o->data != len)
            continue;
        free(tex);
        if (!(tex = (uint8_t*)malloc(len))) break;
        memcpy(tex, other + o->at + o->data, len);
        if ((p = find_sub(pu, npu, t->name)) != NULL) {
            uint32_t qlen;
            q = find_sub(po, npo, t->name);
            plen = p->size - p->data;
            if (!q || memcmp(q->shape, p->shape, 4) || plen > sizeof(pal)) continue;
            qlen = q->size - q->data;
            if (qlen > sizeof(pal)) continue;
            /* The other's entries over the USA's: those past them stay, and
             * more than fit are merged by repaint. */
            memcpy(pal, usa + p->at + p->data, plen);
            memcpy(pal, other + q->at + q->data, qlen);
            if (!repaint(tex, len, be32(t->shape + 8), pal, (int)(qlen / 2), be32(p->shape), (int)(plen / 2)))
                continue;
        }
        if (!memcmp(out->p + t->at + t->data, tex, len) && (!p || !memcmp(out->p + p->at + p->data, pal, plen)))
            continue;
        memcpy(out->p + t->at + t->data, tex, len);
        if (p) memcpy(out->p + p->at + p->data, pal, plen);
        swapped++;
    }
    free(tex);
    free(tu);
    free(to);
    free(pu);
    free(po);
    return swapped;
}

/* ------------------------------------------------------------------------ */
/* An archive */

typedef struct {
    int messages, pictures, fonts;
} Counts;

/* usa (an uncompressed archive) with its text and pictures from other,
 * into out; 0 if nothing in it changed. */
static int swap(const uint8_t* usa, size_t un, const uint8_t* other, size_t on, Counts* c, Buf* out, int depth) {
    int nu = 0, no = 0, i, k, changed = 0;
    U8File* fu = u8_files(usa, un, &nu);
    U8File* fo = other ? u8_files(other, on, &no) : NULL;
    Buf* rep = (Buf*)calloc(nu ? nu : 1, sizeof(Buf));
    if (!fu || !rep || depth > 4) goto done;
    for (i = 0; i < nu; i++) {
        const uint8_t* body = usa + fu[i].off;
        size_t bn = fu[i].size;
        const uint8_t* theirs = NULL;
        size_t tn = 0;
        const char* p = fu[i].path;
        size_t pl = strlen(p);
        for (k = 0; fo && k < no; k++) {
            if (!strcmp(fo[k].path, p)) {
                theirs = other + fo[k].off;
                tn = fo[k].size;
                break;
            }
        }
        if (is_u8(body, bn) && theirs && is_u8(theirs, tn)) {
            if (swap(body, bn, theirs, tn, c, &rep[i], depth + 1)) changed = 1;
        } else if (bn && body[0] == 0x11 && theirs && tn && theirs[0] == 0x11 && pl > 4 &&
                   (!strcmp(p + pl - 4, ".dan") || !strcmp(p + pl - 4, ".dac") || !strcmp(p + pl - 4, ".arc"))) {
            Buf a = {0}, b = {0}, got = {0};
            if (lz11_decompress(body, bn, &a) && is_u8(a.p, a.n) && lz11_decompress(theirs, tn, &b) &&
                swap(a.p, a.n, b.p, b.n, c, &got, depth + 1)) {
                if (lz11_compress(got.p, got.n, &rep[i])) changed = 1;
                else free(rep[i].p), memset(&rep[i], 0, sizeof(Buf));
            }
            free(a.p);
            free(b.p);
            free(got.p);
        } else if (!strncmp(p, "Message/", 8) && !strchr(p + 8, '/') && pl > 4 && !strcmp(p + pl - 4, ".bmg")) {
            const char* name = p + 8;
            const uint8_t* mine = NULL;
            size_t mn = 0;
            /* Message/<language>/<name>.bmg, any letter case. */
            for (k = 0; fo && k < no; k++) {
                const char* q = fo[k].path;
                const char* s1 = strchr(q, '/');
                const char* s2 = s1 ? strchr(s1 + 1, '/') : NULL;
                if (!strncmp(q, "Message/", 8) && s2 && !strchr(s2 + 1, '/') &&
#ifdef _WIN32
                    !_stricmp(s2 + 1, name)
#else
                    !strcasecmp(s2 + 1, name)
#endif
                ) {
                    mine = other + fo[k].off;
                    mn = fo[k].size;
                    break;
                }
            }
            if (!mine) continue;
            if (!strcmp(name, "StaffRoll.bmg")) {
                if (staff_roll(body, bn, mine, mn, &rep[i])) changed = 1, c->messages++;
                continue;
            }
            if (bmg_count(mine, mn) < bmg_count(body, bn)) {
                if (!bmg_fill(mine, mn, body, bn, &rep[i])) continue;
            } else {
                buf_add(&rep[i], mine, mn);
            }
            changed = 1;
            c->messages++;
        } else if (!strncmp(p, "Menu/", 5) && pl > 6 && !strcmp(p + pl - 6, ".brres") && theirs) {
            int n = brres_swap(body, bn, theirs, tn, &rep[i]);
            if (n) changed = 1, c->pictures += n;
            else free(rep[i].p), memset(&rep[i], 0, sizeof(Buf));
        } else if (!strcmp(p, "Font/Standerd_US_EU.brfnt")) {
            for (k = 0; fo && k < no; k++) {
                if (!strcmp(fo[k].path, "Font/Standerd_JP.brfnt")) {
                    buf_add(&rep[i], other + fo[k].off, fo[k].size);
                    changed = 1;
                    c->fonts++;
                    break;
                }
            }
        }
    }
    if (changed) {
        /* u8_replace sorts the files: the replacements go with them. */
        for (i = 0; i < nu; i++) fu[i].node |= (uint32_t)i << 16;
        qsort(fu, nu, sizeof(U8File), by_offset);
        {
            Buf* sorted = (Buf*)calloc(nu, sizeof(Buf));
            for (i = 0; sorted && i < nu; i++) {
                sorted[i] = rep[fu[i].node >> 16];
                fu[i].node &= 0xFFFF;
            }
            if (sorted) {
                memcpy(rep, sorted, nu * sizeof(Buf));
                free(sorted);
                if (!u8_replace(usa, un, fu, nu, rep, out)) changed = 0;
            } else {
                changed = 0;
            }
        }
    }
done:
    for (i = 0; rep && i < nu; i++) free(rep[i].p);
    free(rep);
    free(fu);
    free(fo);
    return changed;
}

/* ------------------------------------------------------------------------ */
/* Discs */

typedef struct {
    char path[256];
    uint64_t offset;
    uint32_t size;
} FstFile;

/* Every file of a disc's FST (count in *count). */
static FstFile* fst_files(const LangDisc* d, int* count) {
    const uint8_t* f = d->fst;
    uint32_t n, strings, i, ends[32];
    char names[32][128];
    int depth = 0, cap = 512;
    FstFile* out;
    *count = 0;
    if (!f || d->fst_size < 12) return NULL;
    n = be32(f + 8);
    strings = n * 12;
    if (strings > d->fst_size) return NULL;
    out = (FstFile*)malloc(sizeof(FstFile) * cap);
    for (i = 1; out && i < n; i++) {
        const uint8_t* e = f + i * 12;
        uint32_t name = strings + (be32(e) & 0xFFFFFF);
        const char* nm = name < d->fst_size ? (const char*)f + name : "";
        while (depth && i >= ends[depth - 1]) depth--;
        if (f[i * 12]) {
            if (depth < 32) {
                ends[depth] = be32(e + 8);
                snprintf(names[depth], sizeof(names[depth]), "%s", nm);
                depth++;
            }
        } else {
            FstFile* x;
            size_t at = 0;
            int k;
            if (*count == cap) {
                FstFile* more = (FstFile*)realloc(out, sizeof(FstFile) * (cap *= 2));
                if (!more) break;
                out = more;
            }
            x = &out[(*count)++];
            for (k = 0; k < depth; k++) {
                at += snprintf(x->path + at, sizeof(x->path) - at, "%s/", names[k]);
                if (at >= sizeof(x->path)) at = sizeof(x->path) - 1;
            }
            snprintf(x->path + at, sizeof(x->path) - at, "%s", nm);
            x->offset = (uint64_t)be32(e + 4) << 2;
            x->size = be32(e + 8);
        }
    }
    return out;
}

static const FstFile* find_file(const FstFile* f, int n, const char* path) {
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(f[i].path, path)) return &f[i];
    return NULL;
}

static int read_whole(const LangDisc* d, const FstFile* f, Buf* out) {
    out->n = 0;
    if (!buf_reserve(out, f->size ? f->size : 1)) return 0;
    if (!d->read(d->user, f->path, f->offset, 0, out->p, f->size)) return 0;
    out->n = f->size;
    return 1;
}

typedef struct {
    uint8_t* fst;
    char root[1024];
} Folder;

static int folder_read(void* user, const char* path, uint64_t offset, uint32_t pos, void* out, uint32_t size) {
    Folder* f = (Folder*)user;
    char full[1400];
    FILE* fp;
    int ok;
    (void)offset;
    snprintf(full, sizeof(full), "%s/files/%s", f->root, path);
    if (!(fp = fopen(full, "rb"))) return 0;
    ok = fseek(fp, (long)pos, SEEK_SET) == 0 && fread(out, 1, size, fp) == size;
    fclose(fp);
    return ok;
}

int langdisc_folder(LangDisc* d, const char* root) {
    Folder* f = (Folder*)calloc(1, sizeof(Folder));
    char path[1100];
    FILE* fp;
    long n;
    memset(d, 0, sizeof(*d));
    if (!f) return 0;
    snprintf(f->root, sizeof(f->root), "%s", root);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", root);
    if (!(fp = fopen(path, "rb"))) {
        free(f);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->fst = (uint8_t*)malloc(n > 0 ? n : 1);
    if (n <= 0 || !f->fst || fread(f->fst, 1, n, fp) != (size_t)n) {
        fclose(fp);
        free(f->fst);
        free(f);
        return 0;
    }
    fclose(fp);
    d->fst = f->fst;
    d->fst_size = (uint32_t)n;
    d->read = folder_read;
    d->user = f;
    return 1;
}

void langdisc_free(LangDisc* d) {
    if (d->read == folder_read && d->user) {
        free(((Folder*)d->user)->fst);
        free(d->user);
    }
    memset(d, 0, sizeof(*d));
}

int langpack_language_of(const char* id) {
    if (!strncmp(id, "R8AJ01", 6)) return LANGUAGE_JP;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Files */

#ifdef _WIN32
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

static void make_dirs(const char* file) {
    char p[1400];
    char* s;
    snprintf(p, sizeof(p), "%s", file);
    for (s = p + 1; *s; s++) {
        if (*s == '/' || *s == '\\') {
            char c = *s;
            *s = '\0';
            MKDIR(p);
            *s = c;
        }
    }
}

static void remove_tree(const char* dir) {
#ifdef _WIN32
    char pattern[1400], full[1400];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(full);
            else DeleteFileA(full);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    char full[1400];
    if (d) {
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(full);
            else unlink(full);
        }
        closedir(d);
    }
    rmdir(dir);
#endif
}

static int exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int write_file(const char* path, const void* data, size_t n) {
    FILE* f;
    int ok;
    make_dirs(path);
    if (!(f = fopen(path, "wb"))) return 0;
    ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* ------------------------------------------------------------------------ */
/* The sound archive */

static volatile int sPercent;

/* A disc's file copied to path, in pieces (the sound archive is 160 MB),
 * the last tenth of the progress. */
static int copy_file(const LangDisc* d, const FstFile* f, const char* path) {
    enum { PIECE = 4 << 20 };
    uint8_t* buf = (uint8_t*)malloc(PIECE);
    uint32_t pos = 0;
    FILE* fp;
    int ok = buf != NULL;
    make_dirs(path);
    if (!ok || !(fp = fopen(path, "wb"))) {
        free(buf);
        return 0;
    }
    while (ok && pos < f->size) {
        uint32_t n = f->size - pos < PIECE ? f->size - pos : PIECE;
        ok = d->read(d->user, f->path, f->offset, pos, buf, n) && fwrite(buf, 1, n, fp) == n;
        pos += n;
        sPercent = 90 + (int)(10.0 * pos / f->size);
    }
    free(buf);
    return fclose(fp) == 0 && ok;
}

/* The counts of its sounds, files, groups and the rest: what the USA
 * program needs to match. */
static int brsar_tables(const LangDisc* d, const FstFile* f, uint32_t out[5]) {
    uint8_t h[0x40];
    uint8_t* info;
    uint32_t off, size, i;
    if (f->size < 0x40 || !d->read(d->user, f->path, f->offset, 0, h, 0x40) || memcmp(h, "RSAR", 4)) return 0;
    off = be32(h + 0x18);
    size = be32(h + 0x1C);
    if (size < 0x50 || (uint64_t)off + size > f->size || !(info = (uint8_t*)malloc(size))) return 0;
    if (!d->read(d->user, f->path, f->offset, off, info, size)) {
        free(info);
        return 0;
    }
    for (i = 0; i < 5; i++) {
        uint32_t ref = be32(info + 8 + i * 8 + 4);
        out[i] = 8 + ref + 4 <= size ? be32(info + 8 + ref) : 0;
    }
    free(info);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* The job */

static volatile int sState = LANGPACK_IDLE;
static char sMessage[256];

int langpack_state(int* percent, const char** message) {
    if (percent) *percent = sPercent;
    if (message) *message = sMessage;
    return sState;
}

void langpack_fail(const char* message) {
    snprintf(sMessage, sizeof(sMessage), "%s", message);
    sState = LANGPACK_FAILED;
    fprintf(stderr, "language pack: %s\n", message);
}

void langpack_set_running(const char* message) {
    snprintf(sMessage, sizeof(sMessage), "%s", message);
    sPercent = 0;
    sState = LANGPACK_RUNNING;
}

static int ends_with(const char* s, const char* e) {
    size_t a = strlen(s), b = strlen(e);
    return a >= b && !strcmp(s + a - b, e);
}

void langpack_run(int lang, const LangDisc* usa, const LangDisc* other) {
    char out[1100], path[1500];
    int nu = 0, no = 0, i, total = 0, at = 0, archives = 0, cries = 0;
    FstFile* fu = fst_files(usa, &nu);
    FstFile* fo = fst_files(other, &no);
    Counts c = {0, 0, 0};
    const FstFile *bu, *bo;
    snprintf(sMessage, sizeof(sMessage), "Making the %s pack", settings_language_name(lang));
    sPercent = 0;
    sState = LANGPACK_RUNNING;
    if (!fu || !fo) {
        langpack_fail("The discs' file tables couldn't be read");
        goto end;
    }
    snprintf(out, sizeof(out), "%s.new", settings_language_dir(lang));
    remove_tree(out);
    for (i = 0; i < nu; i++)
        if (ends_with(fu[i].path, ".dac") || ends_with(fu[i].path, ".dan") || ends_with(fu[i].path, ".arc")) total++;
    for (i = 0; i < nu; i++) {
        const FstFile* f = &fu[i];
        const FstFile* o;
        Buf raw = {0}, oraw = {0}, a = {0}, b = {0}, got = {0}, packed = {0};
        int compressed;
        if (!(ends_with(f->path, ".dac") || ends_with(f->path, ".dan") || ends_with(f->path, ".arc"))) continue;
        sPercent = 90 * ++at / (total ? total : 1);
        if (!(o = find_file(fo, no, f->path))) continue;
        if (!read_whole(usa, f, &raw) || !read_whole(other, o, &oraw)) {
            free(raw.p);
            free(oraw.p);
            langpack_fail("A file couldn't be read from a disc");
            goto end;
        }
        compressed = raw.n && raw.p[0] == 0x11;
        if (compressed ? !lz11_decompress(raw.p, raw.n, &a) : !buf_add(&a, raw.p, raw.n)) goto next;
        if (oraw.n && oraw.p[0] == 0x11 ? !lz11_decompress(oraw.p, oraw.n, &b) : !buf_add(&b, oraw.p, oraw.n))
            goto next;
        if (!is_u8(a.p, a.n) || !is_u8(b.p, b.n)) goto next;
        if (!swap(a.p, a.n, b.p, b.n, &c, &got, 0)) goto next;
        if (compressed && !lz11_compress(got.p, got.n, &packed)) goto next;
        snprintf(path, sizeof(path), "%s/text/files/%s", out, f->path);
        if (!write_file(path, compressed ? packed.p : got.p, compressed ? packed.n : got.n)) {
            langpack_fail("The pack couldn't be written: is the storage full?");
            goto end;
        }
        archives++;
    next:
        free(raw.p);
        free(oraw.p);
        free(a.p);
        free(b.p);
        free(got.p);
        free(packed.p);
    }
    /* The cries: the whole sound archive, if its tables match. */
    bu = find_file(fu, nu, "Sound/Delhi.brsar");
    bo = find_file(fo, no, "Sound/Delhi.brsar");
    if (bu && bo) {
        uint32_t tu[5], to[5];
        if (brsar_tables(usa, bu, tu) && brsar_tables(other, bo, to) && !memcmp(tu, to, sizeof(tu))) {
            snprintf(path, sizeof(path), "%s/cries/files/Sound/Delhi.brsar", out);
            if (!copy_file(other, bo, path)) {
                langpack_fail("The cries couldn't be written: is the storage full?");
                goto end;
            }
            cries = 1;
        }
    }
    if (!archives) {
        langpack_fail("Nothing on that disc fits this game's text");
        goto end;
    }
    snprintf(path, sizeof(path), "%s/complete", out);
    write_file(path, "1", 1);
    fprintf(stderr, "language pack: %d archives (%d messages, %d pictures, %d fonts)%s to %s\n", archives, c.messages,
            c.pictures, c.fonts, cries ? ", cries" : "", out);
    /* In use, the pack the game started with stays until it starts again. */
    if (g_language_started != lang && g_cries_started != lang) langpack_promote(lang);
    snprintf(sMessage, sizeof(sMessage), "%s ready%s", settings_language_name(lang),
             g_language_started == lang || g_cries_started == lang ? ": restart" : "");
    sPercent = 100;
    sState = LANGPACK_DONE;
end:
    free(fu);
    free(fo);
}

void langpack_promote(int lang) {
    char dir[1100], fresh[1100], mark[1200];
    snprintf(dir, sizeof(dir), "%s", settings_language_dir(lang));
    snprintf(fresh, sizeof(fresh), "%s.new", dir);
    snprintf(mark, sizeof(mark), "%s/complete", fresh);
    if (!exists(mark)) return;
    remove_tree(dir);
    if (rename(fresh, dir) != 0) fprintf(stderr, "language pack: %s couldn't be put in place\n", fresh);
}

typedef struct {
    void (*fn)(void*);
    void* arg;
} Thread;

#ifdef _WIN32
static DWORD WINAPI thread_main(void* p)
#else
static void* thread_main(void* p)
#endif
{
    Thread t = *(Thread*)p;
    free(p);
    t.fn(t.arg);
    return 0;
}

int langpack_thread(void (*fn)(void*), void* arg) {
    Thread* j = (Thread*)malloc(sizeof(Thread));
    if (!j) return -1;
    j->fn = fn;
    j->arg = arg;
#ifdef _WIN32
    {
        HANDLE t = CreateThread(NULL, 0, thread_main, j, 0, NULL);
        if (!t) {
            free(j);
            return -1;
        }
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, thread_main, j)) {
            free(j);
            return -1;
        }
        pthread_detach(t);
    }
#endif
    return 0;
}
