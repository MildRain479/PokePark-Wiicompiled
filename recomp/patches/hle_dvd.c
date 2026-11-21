/* The disc, served from files extracted from the user's own copy (see
 * recomp/README.md). The game's DVD library is left as is: it reads the
 * data partition through /dev/di (hle_ios.c), and those reads are mapped
 * back to files through the disc's file table (FST). On Android the disc
 * can instead be read straight from the user's disc image (disc_image.c),
 * whose data partition is read at the same offsets. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#ifndef _WIN32
#define _strdup strdup
#define _fseeki64 fseeko
#endif

typedef struct Region {
    uint64_t offset, size; /* in the data partition */
    char* path;            /* host file */
} Region;

static char sRoot[512];
static Region* sRegions;
static int sRegionCount, sRegionCap;
static uint8_t sBootBin[0x440];
static uint8_t* sFst;
static uint32_t sFstSize;
/* A disc image's partition, read at an offset, and its TMD. */
static int (*sImageRead)(uint64_t offset, void* out, uint32_t len);
static uint8_t* sTmd;
static uint32_t sTmdSize;

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void add_region(uint64_t offset, uint64_t size, const char* path) {
    if (sRegionCount == sRegionCap) {
        sRegionCap = sRegionCap ? sRegionCap * 2 : 512;
        sRegions = (Region*)realloc(sRegions, sRegionCap * sizeof(Region));
    }
    sRegions[sRegionCount].offset = offset;
    sRegions[sRegionCount].size = size;
    sRegions[sRegionCount].path = _strdup(path);
    sRegionCount++;
}

static uint8_t* read_file(const char* path, uint32_t* size) {
    FILE* f = fopen(path, "rb");
    uint8_t* data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(n ? n : 1);
    if (fread(data, 1, n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return data;
}

/* Walks the FST, adding a region for every file. Wii FSTs store file
 * offsets divided by 4. */
static void add_fst_files(void) {
    uint32_t count = be32(sFst + 8);
    const char* names = (const char*)sFst + count * 12;
    uint32_t dir_end[64];
    char dir_path[64][256];
    int depth = 0;
    uint32_t i;
    strcpy(dir_path[0], "files");
    dir_end[0] = count;
    for (i = 1; i < count; i++) {
        const uint8_t* e = sFst + i * 12;
        const char* name = names + (be32(e) & 0xFFFFFF);
        char path[512];
        while (depth > 0 && i >= dir_end[depth]) depth--;
        if (e[0]) {
            if (depth + 1 < 64) {
                depth++;
                snprintf(dir_path[depth], sizeof(dir_path[depth]), "%s/%s", dir_path[depth - 1], name);
                dir_end[depth] = be32(e + 8);
            }
        } else {
            snprintf(path, sizeof(path), "%s/%s/%s", sRoot, dir_path[depth], name);
            add_region((uint64_t)be32(e + 4) << 2, be32(e + 8), path);
        }
    }
}

/* The extracted disc's folder (root/sys, root/files), or "" when it's
 * read from a disc image. */
const char* hle_dvd_root(void) { return sImageRead ? "" : sRoot; }

int hle_dvd_init(const char* root) {
    uint8_t* boot;
    char path[600];
    uint32_t size;
    strncpy(sRoot, root, sizeof(sRoot) - 1);
    snprintf(path, sizeof(path), "%s/sys/boot.bin", sRoot);
    boot = read_file(path, &size);
    if (!boot || size < sizeof(sBootBin)) {
        HLE_WARN("no disc at %s (extract it with dtk; see recomp/README.md)", sRoot);
        return -1;
    }
    memcpy(sBootBin, boot, sizeof(sBootBin));
    free(boot);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", sRoot);
    sFst = read_file(path, &sFstSize);
    if (!sFst) return -1;
    /* The system area, then every file. */
    snprintf(path, sizeof(path), "%s/sys/boot.bin", sRoot);
    add_region(0, 0x440, path);
    snprintf(path, sizeof(path), "%s/sys/bi2.bin", sRoot);
    add_region(0x440, 0x2000, path);
    snprintf(path, sizeof(path), "%s/sys/apploader.img", sRoot);
    add_region(0x2440, 0x3B000, path);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", sRoot);
    add_region((uint64_t)be32(sBootBin + 0x424) << 2, sFstSize, path);
    add_fst_files();
    HLE_LOG("disc: %d files from %s", sRegionCount, sRoot);
    return 0;
}

int hle_dvd_init_image(const uint8_t* boot, const uint8_t* fst, uint32_t fstSize, const uint8_t* tmd,
                       uint32_t tmdSize, int (*read)(uint64_t offset, void* out, uint32_t len)) {
    memcpy(sBootBin, boot, sizeof(sBootBin));
    sFst = (uint8_t*)malloc(fstSize);
    memcpy(sFst, fst, fstSize);
    sFstSize = fstSize;
    if (tmd && tmdSize) {
        sTmd = (uint8_t*)malloc(tmdSize);
        memcpy(sTmd, tmd, tmdSize);
        sTmdSize = tmdSize;
    }
    sImageRead = read;
    HLE_LOG("disc: from a disc image, %u files", be32(sFst + 8));
    return 0;
}

/* Files that replace the disc's own (a language pack, hle_dvd_overlay):
 * their FST entries point past the end of the disc, here. */
#define OVERLAY_BASE 0x200000000ull /* 8 GB */
static Region* sOverlays;
static int sOverlayCount;
static uint64_t sOverlayNext = OVERLAY_BASE;

static uint64_t file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    int64_t n;
    if (!f) return 0;
    _fseeki64(f, 0, SEEK_END);
#ifdef _WIN32
    n = _ftelli64(f);
#else
    n = ftello(f);
#endif
    fclose(f);
    return n > 0 ? (uint64_t)n : 0;
}

/* Every file under <dir>/files at a path the disc has (spelled as the
 * disc spells it) is read in place of the disc's. The FST in guest memory
 * is changed, so this runs before the game starts; returns how many files
 * were replaced. */
int hle_dvd_overlay(const char* dir) {
    uint32_t fst = LD32(0x80000038), count, i;
    const char* names;
    uint32_t dir_end[64];
    char dir_path[64][256];
    int depth = 0, made = 0;
    uint64_t next = sOverlayNext;
    if (!sFst || !fst || !dir || !*dir) return 0;
    count = be32(sFst + 8);
    names = (const char*)sFst + count * 12;
    dir_path[0][0] = '\0';
    dir_end[0] = count;
    for (i = 1; i < count; i++) {
        const uint8_t* e = sFst + i * 12;
        const char* name = names + (be32(e) & 0xFFFFFF);
        char path[1024];
        uint64_t size;
        while (depth > 0 && i >= dir_end[depth]) depth--;
        if (e[0]) {
            if (depth + 1 < 64) {
                depth++;
                snprintf(dir_path[depth], sizeof(dir_path[depth]), "%s/%s", dir_path[depth - 1], name);
                dir_end[depth] = be32(e + 8);
            }
            continue;
        }
        snprintf(path, sizeof(path), "%s/files%s/%s", dir, dir_path[depth], name);
        size = file_size(path);
        if (!size || size > 0xFFFFFFFFu) continue;
        sOverlays = (Region*)realloc(sOverlays, (sOverlayCount + 1) * sizeof(Region));
        sOverlays[sOverlayCount].offset = next;
        sOverlays[sOverlayCount].size = size;
        sOverlays[sOverlayCount].path = _strdup(path);
        sOverlayCount++;
        ST32(fst + i * 12 + 4, (uint32_t)(next >> 2));
        ST32(fst + i * 12 + 8, (uint32_t)size);
        next = (next + size + 0x7FFF) & ~0x7FFFull;
        made++;
    }
    sOverlayNext = next;
    fprintf(stderr, "disc: %d files from %s\n", made, dir);
    return made;
}

static int read_overlay(uint64_t offset, uint32_t len, uint32_t dst) {
    uint64_t end = offset + len;
    int i;
    memset(MA(dst), 0, len);
    for (i = 0; i < sOverlayCount; i++) {
        const Region* r = &sOverlays[i];
        uint64_t hi = end < r->offset + r->size ? end : r->offset + r->size;
        FILE* f;
        if (offset < r->offset || offset >= r->offset + r->size) continue;
        if (!(f = fopen(r->path, "rb"))) break;
        _fseeki64(f, (int64_t)(offset - r->offset), SEEK_SET);
        i = fread(MA(dst), 1, (size_t)(hi - offset), f) == hi - offset;
        fclose(f);
        HLE_LOG("disc read %08llX+%X: %s", (unsigned long long)offset, (unsigned)(hi - offset), r->path);
        return i;
    }
    HLE_WARN("disc read %08llX+%X: no replacement file there", (unsigned long long)offset, (unsigned)len);
    return 0;
}

/* What the apploader leaves in memory: the disc header at 0x80000000 and
 * the FST at the top of MEM1, just below the arena. */
void hle_dvd_boot_setup(void) {
    uint32_t arena_hi = LD32(0x80000034);
    uint32_t fst_addr = (arena_hi - sFstSize) & ~31u;
    if (!sFst) return;
    memcpy(MA(0x80000000), sBootBin, 0x20);
    memcpy(MA(fst_addr), sFst, sFstSize);
    ST32(0x80000034, fst_addr); /* arena hi */
    ST32(0x80000038, fst_addr); /* FST */
    ST32(0x8000003C, sFstSize); /* max FST size */
    ST32(0x80003110, fst_addr); /* MEM1 arena end */
}

/* Copies len bytes at a data-partition offset to guest memory. */
int hle_dvd_read(uint64_t offset, uint32_t len, uint32_t dst) {
    uint64_t end = offset + len;
    int i, ok = 1;
    if (g_mem_will_write) g_mem_will_write(dst, len);
    if (offset >= OVERLAY_BASE) return read_overlay(offset, len, dst);
    if (sImageRead) {
        HLE_LOG("disc read %08llX+%X", (unsigned long long)offset, (unsigned)len);
        if (sImageRead(offset, MA(dst), len)) return 1;
        memset(MA(dst), 0, len);
        HLE_WARN("disc read %08llX+%X failed", (unsigned long long)offset, (unsigned)len);
        return 0;
    }
    memset(MA(dst), 0, len);
    for (i = 0; i < sRegionCount; i++) {
        const Region* r = &sRegions[i];
        uint64_t lo = offset > r->offset ? offset : r->offset;
        uint64_t hi = end < r->offset + r->size ? end : r->offset + r->size;
        FILE* f;
        if (lo >= hi) continue;
        f = fopen(r->path, "rb");
        if (!f) {
            HLE_WARN("disc read needs %s, which is missing", r->path);
            ok = 0;
            continue;
        }
        _fseeki64(f, (int64_t)(lo - r->offset), SEEK_SET);
        if (fread(MA(dst + (uint32_t)(lo - offset)), 1, (size_t)(hi - lo), f) != hi - lo) ok = 0;
        fclose(f);
        HLE_LOG("disc read %08llX+%X: %s", (unsigned long long)lo, (unsigned)(hi - lo), r->path + strlen(sRoot) + 1);
    }
    return ok;
}

/* The partition's TMD, which DVDLowOpenPartition returns. */
uint32_t hle_dvd_tmd(uint32_t dst, uint32_t max) {
    char path[600];
    uint32_t size;
    uint8_t* tmd;
    if (sTmd) {
        size = sTmdSize < max ? sTmdSize : max;
        memcpy(MA(dst), sTmd, size);
        return size;
    }
    snprintf(path, sizeof(path), "%s/tmd.bin", sRoot);
    tmd = read_file(path, &size);
    if (!tmd) return 0;
    if (size > max) size = max;
    memcpy(MA(dst), tmd, size);
    free(tmd);
    return size;
}

const uint8_t* hle_dvd_disc_id(void) { return sBootBin; }

/* BOOL DVDReadAsyncPrio(DVDFileInfo* file, void* addr, s32 length, s32 offset,
 *                       DVDCallback callback, s32 prio): logged only. */
void f_802262E0(ppc_t* c) {
    if (hle_log_enabled()) {
        HLE_LOG("DVDReadAsyncPrio(file %08X, addr %08X, len %X, offset %X)", c->r[3], c->r[4], c->r[5], c->r[6]);
        if (hle_log_enabled() > 1) ppc_backtrace(c);
    }
    f_802262E0_orig(c);
}
