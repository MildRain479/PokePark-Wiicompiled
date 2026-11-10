/* The NAND file system (ISFS), stored in a host directory: the Wii path
 * /title/00010000/52384145/data/x is <root>/title/00010000/52384145/data/x.
 * hle_ios.c routes /dev/fs requests and file descriptors here. The root
 * defaults to "nand" next to the disc directory (orig/R8AE01/nand). */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hle.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0755)
#endif

/* ISFS result codes. */
#define FS_OK 0
#define FS_EINVAL (-101)
#define FS_EACCESS (-102)
#define FS_EEXIST (-105)
#define FS_ENOENT (-106)
#define FS_ENOTEMPTY (-115)

static char sRoot[512] = "nand";

static void ensure_sysconf(void);
static void ensure_setting_txt(void);

/* Also fills in the system settings a console's NAND would have. */
void hle_nand_set_root(const char* root) {
    strncpy(sRoot, root, sizeof(sRoot) - 1);
    ensure_sysconf();
    ensure_setting_txt();
}

/* Host path for a NAND path; rejects anything that could escape the root. */
static int host_path(const char* nand, char* out, size_t n) {
    if (nand[0] != '/' || strstr(nand, "..")) return 0;
    snprintf(out, n, "%s%s", sRoot, nand);
    return 1;
}

static void mkdirs(const char* path) {
    char tmp[600];
    size_t i;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir_one(tmp);
            tmp[i] = '/';
        }
    }
    mkdir_one(tmp);
}

static int is_dir(const char* host) {
    struct stat st;
    return stat(host, &st) == 0 && (st.st_mode & S_IFDIR);
}

static int exists(const char* host) {
    struct stat st;
    return stat(host, &st) == 0;
}

/* Parent directories are created on demand: on a Wii the system menu has
 * made the title's data directory before the game runs. */
static void ensure_parent(const char* host) {
    char tmp[600];
    char* slash;
    strncpy(tmp, host, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    slash = strrchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        mkdirs(tmp);
    }
}

/* ------------------------------------------------------------------------ */
/* Files */

FILE* hle_nand_open(const char* nand, int mode, int32_t* err) {
    char host[600];
    FILE* f;
    if (!host_path(nand, host, sizeof(host))) {
        *err = FS_EINVAL;
        return NULL;
    }
    if (strncmp(nand, "/title/00010000/", 16) == 0) ensure_parent(host);
    if (!exists(host) || is_dir(host)) {
        *err = FS_ENOENT;
        return NULL;
    }
    f = fopen(host, mode & 2 ? "r+b" : "rb");
    if (!f) {
        *err = FS_EACCESS;
        return NULL;
    }
    HLE_LOG("NAND open %s (mode %d)", nand, mode);
    return f;
}

int32_t hle_nand_read(FILE* f, uint32_t dst, uint32_t len) {
    return (int32_t)fread(MA(dst), 1, len, f);
}

int32_t hle_nand_write(FILE* f, uint32_t src, uint32_t len) {
    int32_t n = (int32_t)fwrite(MA(src), 1, len, f);
    fflush(f);
    return n;
}

int32_t hle_nand_seek(FILE* f, int32_t offset, int32_t whence) {
    if (fseek(f, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END) != 0) return FS_EINVAL;
    return (int32_t)ftell(f);
}

/* ISFS_GetFileStats: size and position. */
int32_t hle_nand_file_stats(FILE* f, uint32_t out) {
    long pos = ftell(f), size;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, pos, SEEK_SET);
    ST32(out, (uint32_t)size);
    ST32(out + 4, (uint32_t)pos);
    return FS_OK;
}

/* ------------------------------------------------------------------------ */
/* /dev/fs */

/* Attribute block used by CreateDir/CreateFile/SetAttr/GetAttr: owner id
 * (u32), group id (u16), path[64], three permission bytes and an
 * attribute byte. */
#define ATTR_PATH 6
#define ATTR_SIZE 0x4C

static int32_t create(uint32_t in, int dir) {
    char nand[65], host[600];
    hle_read_str(in + ATTR_PATH, nand, sizeof(nand));
    if (!host_path(nand, host, sizeof(host))) return FS_EINVAL;
    ensure_parent(host);
    if (exists(host)) return FS_EEXIST;
    HLE_LOG("NAND create %s %s", dir ? "dir" : "file", nand);
    if (dir) {
        mkdir_one(host);
    } else {
        FILE* f = fopen(host, "wb");
        if (!f) return FS_EACCESS;
        fclose(f);
    }
    return FS_OK;
}

static int32_t remove_path(const char* host) {
    if (is_dir(host)) {
#ifdef _WIN32
        return _rmdir(host) == 0 ? FS_OK : FS_ENOTEMPTY;
#else
        return rmdir(host) == 0 ? FS_OK : FS_ENOTEMPTY;
#endif
    }
    return remove(host) == 0 ? FS_OK : FS_EACCESS;
}

int32_t hle_nand_ioctl(uint32_t type, uint32_t in, uint32_t out, uint32_t outsize) {
    char nand[65], nand2[65], host[600], host2[600];
    switch (type) {
    case 2: /* GetStats: report a mostly empty NAND */
        if (outsize >= 0x1C) {
            memset(MA(out), 0, 0x1C);
            ST32(out + 0x00, 0x4000);  /* cluster size */
            ST32(out + 0x04, 0x5000);  /* free clusters */
            ST32(out + 0x08, 0x1000);  /* used clusters */
            ST32(out + 0x14, 0x6000);  /* free inodes */
        }
        return FS_OK;
    case 3: return create(in, 1);
    case 9: return create(in, 0);
    case 5: /* SetAttr: permissions are not modelled */
        return FS_OK;
    case 6: /* GetAttr */
        hle_read_str(in, nand, sizeof(nand));
        if (!host_path(nand, host, sizeof(host))) return FS_EINVAL;
        if (!exists(host)) return FS_ENOENT;
        if (outsize >= ATTR_SIZE) {
            memset(MA(out), 0, ATTR_SIZE);
            ST32(out, 0x1000);         /* owner */
            ST16(out + 4, 0x3031);     /* group: maker code "01" */
            memcpy(MA(out + ATTR_PATH), nand, strlen(nand) + 1);
            ST8(out + 0x46, 3);        /* owner read/write */
            ST8(out + 0x47, 3);
            ST8(out + 0x48, 3);
        }
        return FS_OK;
    case 7: /* Delete */
        hle_read_str(in, nand, sizeof(nand));
        if (!host_path(nand, host, sizeof(host))) return FS_EINVAL;
        if (!exists(host)) return FS_ENOENT;
        HLE_LOG("NAND delete %s", nand);
        return remove_path(host);
    case 8: /* Rename */
        hle_read_str(in, nand, sizeof(nand));
        hle_read_str(in + 64, nand2, sizeof(nand2));
        if (!host_path(nand, host, sizeof(host)) || !host_path(nand2, host2, sizeof(host2))) return FS_EINVAL;
        if (!exists(host)) return FS_ENOENT;
        HLE_LOG("NAND rename %s -> %s", nand, nand2);
        ensure_parent(host2);
        remove(host2);
        return rename(host, host2) == 0 ? FS_OK : FS_EACCESS;
    case 13: /* Shutdown */
        return FS_OK;
    }
    HLE_WARN("/dev/fs ioctl 0x%X: unhandled", type);
    return FS_EINVAL;
}

/* ReadDir: in[0] = path; with a second in/out pair, out[0] receives the
 * NUL-separated names and out[1] the count, else out[0] the count. */
static int32_t read_dir(uint32_t vec, uint32_t nin, uint32_t nout) {
    char nand[65], host[600];
    char names[64][32];
    int count = 0, i;
    hle_read_str(LD32(vec), nand, sizeof(nand));
    if (!host_path(nand, host, sizeof(host))) return FS_EINVAL;
    if (!is_dir(host)) return exists(host) ? FS_EINVAL : FS_ENOENT;
#ifdef _WIN32
    {
        char pattern[620];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s/*", host);
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") && count < 64) {
                    strncpy(names[count], fd.cFileName, 31);
                    names[count++][31] = '\0';
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR* d = opendir(host);
        struct dirent* e;
        while (d && (e = readdir(d)) && count < 64) {
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                strncpy(names[count], e->d_name, 31);
                names[count++][31] = '\0';
            }
        }
        if (d) closedir(d);
    }
#endif
    if (nin >= 2 && nout >= 2) {
        uint32_t max = LD32(LD32(vec + 8)), dst = LD32(vec + 8 * nin), pos = 0;
        for (i = 0; i < count && (uint32_t)i < max; i++) {
            size_t len = strlen(names[i]) + 1;
            memcpy(MA(dst + pos), names[i], len);
            pos += (uint32_t)len;
        }
        ST32(LD32(vec + 8 * (nin + 1)), (uint32_t)(count < (int)max ? count : (int)max));
    } else if (nout >= 1) {
        ST32(LD32(vec + 8 * nin), (uint32_t)count);
    }
    return FS_OK;
}

int32_t hle_nand_ioctlv(uint32_t type, uint32_t vec, uint32_t nin, uint32_t nout) {
    switch (type) {
    case 4: return read_dir(vec, nin, nout);
    case 12: /* GetUsage: blocks and inodes used under a path */
        if (nout >= 2) {
            ST32(LD32(vec + 8 * nin), 0);
            ST32(LD32(vec + 8 * (nin + 1)), 0);
        }
        return FS_OK;
    }
    HLE_WARN("/dev/fs ioctlv 0x%X: unhandled", type);
    return FS_EINVAL;
}

/* ------------------------------------------------------------------------ */
/* System settings */

/* What a US console's system menu leaves on the NAND: SYSCONF (language,
 * display, sound, Wii Remote settings) and setting.txt (region). Without
 * them the SDK falls back to defaults, among them the Japanese language:
 * the game then shows its Japanese placeholders (the default name
 * "Pikachu" in katakana, a dummy name graphic in the picture confirmation dialog). */

#define SYSCONF_SIZE 0x4000
enum { SC_BIGARRAY = 1, SC_SMALLARRAY, SC_BYTE, SC_SHORT, SC_LONG, SC_LONGLONG, SC_BOOL };

typedef struct {
    const char* name;
    int type;
    uint32_t value;
} ScDefault;

static const ScDefault kScDefaults[] = {
    {"IPL.LNG", SC_BYTE, 1},  /* English */
    {"IPL.AR", SC_BYTE, 0},   /* 4:3 (reported as hle_widescreen() says) */
    {"IPL.SND", SC_BYTE, 1},  /* stereo */
    {"IPL.PGS", SC_BYTE, 0},  /* no progressive scan */
    {"IPL.E60", SC_BYTE, 1},  /* EuRGB60 (PAL only) */
    {"IPL.SSV", SC_BYTE, 0},  /* screen saver off */
    {"IPL.CB", SC_LONG, 0},   /* counter bias */
    {"BT.MOT", SC_BYTE, 1},   /* Wii Remote rumble on */
    {"BT.BAR", SC_BYTE, 0},   /* sensor bar below the screen */
    {"BT.SENS", SC_LONG, 3},  /* sensor bar sensitivity */
    {"BT.SPKV", SC_BYTE, 0x58}, /* Wii Remote speaker volume */
};

/* One parsed SYSCONF entry: its bytes, from the type/name byte on. */
typedef struct {
    char name[32];
    const uint8_t* data;
    int size;
} ScEntry;

static int sc_entry_size(const uint8_t* p, const uint8_t* end) {
    int type = p[0] >> 5, nlen = (p[0] & 31) + 1, n = 1 + nlen;
    const uint8_t* v = p + n;
    if (v >= end) return -1;
    switch (type) {
    case SC_BIGARRAY: return n + 2 + ((v[0] << 8) | v[1]) + 1;
    case SC_SMALLARRAY: return n + 1 + v[0] + 1;
    case SC_BYTE: case SC_BOOL: return n + 1;
    case SC_SHORT: return n + 2;
    case SC_LONG: return n + 4;
    case SC_LONGLONG: return n + 8;
    }
    return -1;
}

/* Builds the file from the entries, in order. Returns 0 if they don't fit. */
static int sc_build(uint8_t* out, const ScEntry* e, int n) {
    int i, pos = 6 + 2 * (n + 1);
    memset(out, 0, SYSCONF_SIZE);
    memcpy(out, "SCv0", 4);
    out[4] = (uint8_t)(n >> 8);
    out[5] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        if (pos + e[i].size > SYSCONF_SIZE - 4) return 0;
        out[6 + 2 * i] = (uint8_t)(pos >> 8);
        out[7 + 2 * i] = (uint8_t)pos;
        memcpy(out + pos, e[i].data, (size_t)e[i].size);
        pos += e[i].size;
    }
    out[6 + 2 * n] = (uint8_t)(pos >> 8);
    out[7 + 2 * n] = (uint8_t)pos;
    memcpy(out + SYSCONF_SIZE - 4, "SCed", 4);
    return 1;
}

/* Adds the settings above to SYSCONF where missing, keeping the rest. */
static void ensure_sysconf(void) {
    static uint8_t old[SYSCONF_SIZE], out[SYSCONF_SIZE], extra[sizeof(kScDefaults) / sizeof(kScDefaults[0])][16];
    ScEntry e[256];
    char host[600];
    int n = 0, i, k, changed = 0;
    FILE* f;
    snprintf(host, sizeof(host), "%s/shared2/sys/SYSCONF", sRoot);
    f = fopen(host, "rb");
    if (f) {
        if (fread(old, 1, SYSCONF_SIZE, f) != SYSCONF_SIZE || memcmp(old, "SCv0", 4)) memset(old, 0, sizeof(old));
        fclose(f);
    }
    if (!memcmp(old, "SCv0", 4)) {
        int count = (old[4] << 8) | old[5];
        for (i = 0; i < count && n < 256; i++) {
            int off = (old[6 + 2 * i] << 8) | old[7 + 2 * i], size;
            if (off <= 0 || off >= SYSCONF_SIZE) break;
            size = sc_entry_size(old + off, old + SYSCONF_SIZE);
            if (size <= 0 || off + size > SYSCONF_SIZE) break;
            e[n].data = old + off;
            e[n].size = size;
            snprintf(e[n].name, sizeof(e[n].name), "%.*s", (old[off] & 31) + 1, (const char*)old + off + 1);
            n++;
        }
    }
    for (k = 0; k < (int)(sizeof(kScDefaults) / sizeof(kScDefaults[0])); k++) {
        const ScDefault* d = &kScDefaults[k];
        int nlen = (int)strlen(d->name), vlen = d->type == SC_LONG ? 4 : 1, j;
        for (i = 0; i < n && strcmp(e[i].name, d->name); i++) {}
        if (i < n || n == 256) continue;
        extra[k][0] = (uint8_t)((d->type << 5) | (nlen - 1));
        memcpy(&extra[k][1], d->name, (size_t)nlen);
        for (j = 0; j < vlen; j++) extra[k][1 + nlen + j] = (uint8_t)(d->value >> (8 * (vlen - 1 - j)));
        e[n].data = extra[k];
        e[n].size = 1 + nlen + vlen;
        strcpy(e[n].name, d->name);
        n++;
        changed = 1;
    }
    if (!changed || !sc_build(out, e, n)) return;
    ensure_parent(host);
    f = fopen(host, "wb");
    if (!f) return;
    fwrite(out, 1, SYSCONF_SIZE, f);
    fclose(f);
    HLE_LOG("NAND: wrote default system settings to SYSCONF");
}

/* 16:9 unless the aspect ratio setting is 4:3 (hle_set_widescreen, as
 * settings are loaded) or RECOMP_WIDESCREEN=0; fixed once it is first
 * asked, as the game asks once. */
static int sWidescreenSet = 1;
void hle_set_widescreen(int on) { sWidescreenSet = on; }
int hle_widescreen(void) {
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("RECOMP_WIDESCREEN");
        on = e && *e ? *e != '0' : sWidescreenSet;
    }
    return on;
}

/* u8 SCGetAspectRatio(void): the TV's shape from SYSCONF (IPL.AR), as the
 * game reads it once at start-up. Answered from hle_widescreen() instead,
 * so the setting doesn't depend on a SYSCONF already on the NAND. */
void f_8023E860(ppc_t* c) { c->r[3] = hle_widescreen() ? 1 : 0; }

/* The system menu's setting.txt: text lines, XOR-scrambled with a rotating
 * key, padded to 0x100 bytes. */
static void ensure_setting_txt(void) {
    static const char kText[] = "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\n"
                                "SERNO=000000000\r\nVIDEO=NTSC\r\nGAME=US\r\n";
    uint8_t buf[0x100];
    uint32_t key = 0x73B5DBFA;
    char host[600];
    FILE* f;
    size_t i;
    snprintf(host, sizeof(host), "%s/title/00000001/00000002/data/setting.txt", sRoot);
    if (exists(host)) return;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, kText, sizeof(kText) - 1);
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] ^= (uint8_t)key;
        key = (key << 1) | (key >> 31);
    }
    ensure_parent(host);
    f = fopen(host, "wb");
    if (!f) return;
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
    HLE_LOG("NAND: wrote setting.txt (USA)");
}
