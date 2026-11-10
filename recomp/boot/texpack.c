/* Texture packs: Dolphin's, which replace the game's textures with
 * pictures of one's own (higher resolution, as a rule).
 *
 * A pack is a folder of PNGs named as Dolphin names a texture, anywhere
 * under the textures folder (beside the program; on Android in the app's
 * shared folder, beside R8AE01; RECOMP_TEXTURES=<dir> over either), so a
 * pack made for Dolphin is copied there as it is, its R8AE01 folder and
 * all:
 *
 *   tex1_<width>x<height>[_m]_<texture hash>[_<palette hash>]_<format>.png
 *
 * width and height are the texture's own, _m says it has a mipmap filter,
 * the hashes are XXH64 (seed 0) of the texture's bytes (its first level,
 * whole blocks) and, for a colour-indexed one, of the palette's entries
 * from the least index the texture uses to the greatest, and the format
 * is GX's number for it. As in Dolphin, "$" in place of either hash
 * matches any, and an "_arb" ending (mipmaps of their own) is ignored:
 * the levels below are made from the picture. DDS files aren't read.
 *
 * The folder is listed the first time a texture is made; a picture is
 * read when its texture is first used, and kept a while after.
 * RECOMP_TEXTURES_LOG=1 prints each texture's name as it's looked for. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <android/imagedecoder.h>
#endif

#include "gx.h"
#include "host_settings.h"

/* ------------------------------------------------------------------------ */
/* XXH64 */

#define P1 11400714785074694791ull
#define P2 14029467366897019727ull
#define P3 1609587929392839161ull
#define P4 9650029242287828579ull
#define P5 2870177450012600261ull

static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t le64(const uint8_t* p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t round64(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static uint64_t merge64(uint64_t acc, uint64_t v) { return (acc ^ round64(0, v)) * P1 + P4; }

uint64_t texpack_xxh64(const uint8_t* p, size_t n) {
    const uint8_t* end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
        for (; p + 32 <= end; p += 32) {
            v1 = round64(v1, le64(p));
            v2 = round64(v2, le64(p + 8));
            v3 = round64(v3, le64(p + 16));
            v4 = round64(v4, le64(p + 24));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge64(merge64(merge64(merge64(h, v1), v2), v3), v4);
    } else {
        h = P5;
    }
    h += n;
    for (; p + 8 <= end; p += 8) h = rotl(h ^ round64(0, le64(p)), 27) * P1 + P4;
    if (p + 4 <= end) {
        h = rotl(h ^ (uint64_t)le32(p) * P1, 23) * P2 + P3;
        p += 4;
    }
    for (; p < end; p++) h = rotl(h ^ *p * P5, 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    return h ^ (h >> 32);
}

/* ------------------------------------------------------------------------ */
/* The pictures in the folder */

typedef struct {
    char* name; /* the file's name without .png (and _arb) */
    char* path;
} Picture;

static Picture* sPics;
static int sPicCount, sPicCap, sListed;
static char sRoot[1024];

static void add_picture(const char* dir, const char* file) {
    size_t n = strlen(file);
    char* name;
    if (n < 9 || strncmp(file, "tex1_", 5) != 0 || (strcmp(file + n - 4, ".png") && strcmp(file + n - 4, ".PNG"))) return;
    if (sPicCount == sPicCap) {
        sPicCap = sPicCap ? sPicCap * 2 : 256;
        sPics = (Picture*)realloc(sPics, (size_t)sPicCap * sizeof(Picture));
    }
    name = (char*)malloc(n - 3);
    memcpy(name, file, n - 4);
    name[n - 4] = '\0';
    if (n - 4 > 4 && !strcmp(name + n - 8, "_arb")) name[n - 8] = '\0';
    sPics[sPicCount].name = name;
    sPics[sPicCount].path = (char*)malloc(strlen(dir) + n + 2);
    sprintf(sPics[sPicCount].path, "%s/%s", dir, file);
    sPicCount++;
}

/* Every PNG under dir, depth levels of folders down at most. */
static void list_folder(const char* dir, int depth) {
#ifdef _WIN32
    char pattern[1100];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if ((h = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[1100];
            snprintf(sub, sizeof(sub), "%s\\%s", dir, fd.cFileName);
            if (depth > 0) list_folder(sub, depth - 1);
        } else {
            add_picture(dir, fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char sub[1100];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) list_folder(sub, depth - 1);
        } else {
            add_picture(dir, e->d_name);
        }
    }
    closedir(d);
#endif
}

static int by_name(const void* a, const void* b) { return strcmp(((const Picture*)a)->name, ((const Picture*)b)->name); }

static void list_pictures(void) {
    const char* e = getenv("RECOMP_TEXTURES");
    int i, kept = 0;
    sListed = 1;
    if (e && *e) {
        snprintf(sRoot, sizeof(sRoot), "%s", e);
    } else {
#ifdef _WIN32
        char* slash;
        GetModuleFileNameA(NULL, sRoot, sizeof(sRoot) - 16);
        slash = strrchr(sRoot, '\\');
        strcpy(slash ? slash + 1 : sRoot, "textures");
#else
        const char* shared = getenv("RECOMP_SHARED"); /* Android: the app's shared folder */
        snprintf(sRoot, sizeof(sRoot), "%s%stextures", shared ? shared : "", shared ? "/" : "");
#endif
    }
    list_folder(sRoot, 8);
    qsort(sPics, (size_t)sPicCount, sizeof(Picture), by_name);
    /* Of two with the same name, the first (as Dolphin keeps one). */
    for (i = 0; i < sPicCount; i++) {
        if (kept && !strcmp(sPics[kept - 1].name, sPics[i].name)) {
            free(sPics[i].name);
            free(sPics[i].path);
        } else {
            sPics[kept++] = sPics[i];
        }
    }
    sPicCount = kept;
    fprintf(stderr, "textures: %d in %s\n", sPicCount, sRoot);
}

static const Picture* find_picture(const char* name) {
    int lo = 0, hi = sPicCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, sPics[mid].name);
        if (!c) return &sPics[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

int texpack_count(void) {
    if (!sListed) list_pictures();
    return sPicCount;
}

int texpack_on(void) { return g_settings.texture_pack && texpack_count() > 0; }

/* ------------------------------------------------------------------------ */
/* Reading a picture: RGBA8, top row first (malloc'd), or NULL */

#define MAX_SIDE 8192

#ifdef _WIN32
static uint8_t* read_png(const char* path, int* w, int* h) {
    static IWICImagingFactory* factory;
    IWICBitmapDecoder* dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* conv = NULL;
    WCHAR wpath[1100];
    UINT cw = 0, ch = 0;
    uint8_t* px = NULL;
    if (!factory) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                    (void**)&factory)))
            return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1100)) MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 1100);
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(factory, wpath, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(factory, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppRGBA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &cw, &ch)) && cw && ch && cw <= MAX_SIDE && ch <= MAX_SIDE) {
        px = (uint8_t*)malloc((size_t)cw * ch * 4);
        if (px && FAILED(IWICFormatConverter_CopyPixels(conv, NULL, cw * 4, cw * ch * 4, px))) {
            free(px);
            px = NULL;
        }
        *w = (int)cw;
        *h = (int)ch;
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    return px;
}
#elif defined(__ANDROID__)
static uint8_t* read_png(const char* path, int* w, int* h) {
    AImageDecoder* dec = NULL;
    uint8_t* px = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (AImageDecoder_createFromFd(fd, &dec) == ANDROID_IMAGE_DECODER_SUCCESS &&
        AImageDecoder_setAndroidBitmapFormat(dec, ANDROID_BITMAP_FORMAT_RGBA_8888) == ANDROID_IMAGE_DECODER_SUCCESS &&
        AImageDecoder_setUnpremultipliedRequired(dec, 1) == ANDROID_IMAGE_DECODER_SUCCESS) {
        const AImageDecoderHeaderInfo* info = AImageDecoder_getHeaderInfo(dec);
        int cw = AImageDecoderHeaderInfo_getWidth(info), ch = AImageDecoderHeaderInfo_getHeight(info);
        if (cw > 0 && ch > 0 && cw <= MAX_SIDE && ch <= MAX_SIDE && (px = (uint8_t*)malloc((size_t)cw * ch * 4)) &&
            AImageDecoder_decodeImage(dec, px, (size_t)cw * 4, (size_t)cw * ch * 4) != ANDROID_IMAGE_DECODER_SUCCESS) {
            free(px);
            px = NULL;
        }
        *w = cw;
        *h = ch;
    }
    if (dec) AImageDecoder_delete(dec);
    close(fd);
    return px;
}
#else
static uint8_t* read_png(const char* path, int* w, int* h) {
    (void)path;
    (void)w;
    (void)h;
    return NULL;
}
#endif

/* Pictures read lately, kept while they take up less than KEEP_BYTES:
 * a texture made again (evicted from gx_fifo.c's cache, or its memory
 * written with the same bytes) needn't be read again. */
#define KEEP 32
#define KEEP_BYTES (256u << 20)
typedef struct {
    const Picture* pic;
    uint8_t* px;
    int w, h;
    unsigned used;
} Kept;
static Kept sKept[KEEP];
static unsigned sUse;

static const Kept* picture_pixels(const Picture* pic) {
    size_t bytes = 0;
    int i, oldest = 0;
    Kept* k;
    for (i = 0; i < KEEP; i++) {
        if (sKept[i].pic == pic) {
            sKept[i].used = ++sUse;
            return sKept[i].px ? &sKept[i] : NULL;
        }
        if (sKept[i].used < sKept[oldest].used) oldest = i;
    }
    k = &sKept[oldest];
    free(k->px);
    memset(k, 0, sizeof(*k));
    k->pic = pic;
    k->used = ++sUse;
    k->px = read_png(pic->path, &k->w, &k->h);
    if (!k->px) fprintf(stderr, "textures: can't read %s\n", pic->path);
    /* Over the budget: the least lately used let go, but this one. */
    for (i = 0; i < KEEP; i++) bytes += sKept[i].px ? (size_t)sKept[i].w * sKept[i].h * 4 : 0;
    while (bytes > KEEP_BYTES) {
        Kept* o = NULL;
        for (i = 0; i < KEEP; i++) {
            if (sKept[i].px && &sKept[i] != k && (!o || sKept[i].used < o->used)) o = &sKept[i];
        }
        if (!o) break;
        bytes -= (size_t)o->w * o->h * 4;
        free(o->px);
        memset(o, 0, sizeof(*o));
    }
    return k->px ? k : NULL;
}

/* ------------------------------------------------------------------------ */
/* Finding a texture's replacement */

uint8_t* texpack_find(const uint8_t* data, int size, int fmt, int width, int height, int mipmaps, const uint8_t* tlut,
                      int tlut_size, int levels, int* out_w, int* out_h, int* out_levels) {
    char base[48], tex[24], pal[24], name[128];
    const Picture* pic;
    const Kept* k;
    uint8_t* out;
    size_t at;
    int i, lw, lh, n;
    if (!texpack_on() || size <= 0) return NULL;
    if (tlut && tlut_size > 0) {
        /* The palette's entries the texture uses, least to greatest, as
         * Dolphin hashes them (its CI14 reading kept as it is: a byte,
         * moved up 8 bits). */
        unsigned lo = 0xFFFF, hi = 0;
        for (i = 0; i < size; i += tlut_size == 16384 * 2 ? 2 : 1) {
            unsigned a = data[i], b = a;
            if (tlut_size == 16 * 2) {
                a &= 15;
                b >>= 4;
            } else if (tlut_size == 16384 * 2) {
                a = b = (a << 8) & 0x3FFF;
            }
            if (a < lo) lo = a;
            if (b < lo) lo = b;
            if (a > hi) hi = a;
            if (b > hi) hi = b;
        }
        snprintf(pal, sizeof(pal), "_%016llx",
                 (unsigned long long)texpack_xxh64(tlut + 2 * lo, (size_t)2 * (hi + 1 - lo)));
    } else {
        pal[0] = '\0';
    }
    snprintf(base, sizeof(base), "tex1_%dx%d%s", width, height, mipmaps ? "_m" : "");
    snprintf(tex, sizeof(tex), "%016llx", (unsigned long long)texpack_xxh64(data, (size_t)size));
    snprintf(name, sizeof(name), "%s_%s%s_%d", base, tex, pal, fmt);
    pic = find_picture(name);
    {
        static int log = -1;
        if (log < 0) log = getenv("RECOMP_TEXTURES_LOG") && *getenv("RECOMP_TEXTURES_LOG") != '0';
        if (log) fprintf(stderr, "textures: %s%s\n", name, pic ? " (found)" : "");
    }
    if (!pic) {
        snprintf(name, sizeof(name), "%s_%s_$_%d", base, tex, fmt);
        pic = find_picture(name);
    }
    if (!pic && pal[0]) {
        snprintf(name, sizeof(name), "%s_$%s_%d", base, pal, fmt);
        pic = find_picture(name);
    }
    if (!pic || !(k = picture_pixels(pic))) return NULL;
    /* Its levels: as many as the texture has, each half the last's size
     * (averaged), down to 1 x 1. */
    for (n = 1, lw = k->w, lh = k->h, at = 0; ; n++) {
        at += (size_t)lw * lh * 4;
        if (n >= levels || (lw == 1 && lh == 1)) break;
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    if (!(out = (uint8_t*)malloc(at))) return NULL;
    memcpy(out, k->px, (size_t)k->w * k->h * 4);
    for (i = 1, lw = k->w, lh = k->h, at = 0; i < n; i++) {
        const uint8_t* up = out + at;
        int uw = lw, uh = lh, x, y, c;
        uint8_t* to;
        at += (size_t)uw * uh * 4;
        to = out + at;
        lw = uw > 1 ? uw / 2 : 1;
        lh = uh > 1 ? uh / 2 : 1;
        for (y = 0; y < lh; y++) {
            int y0 = uh > 1 ? 2 * y : 0, y1 = uh > 1 ? 2 * y + 1 : 0;
            for (x = 0; x < lw; x++) {
                int x0 = uw > 1 ? 2 * x : 0, x1 = uw > 1 ? 2 * x + 1 : 0;
                for (c = 0; c < 4; c++) {
                    to[((size_t)y * lw + x) * 4 + c] =
                        (uint8_t)((up[((size_t)y0 * uw + x0) * 4 + c] + up[((size_t)y0 * uw + x1) * 4 + c] +
                                   up[((size_t)y1 * uw + x0) * 4 + c] + up[((size_t)y1 * uw + x1) * 4 + c] + 2) / 4);
                }
            }
        }
    }
    *out_w = k->w;
    *out_h = k->h;
    *out_levels = n;
    return out;
}
