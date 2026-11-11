/* Bringing over a save from a Wii or Dolphin (wii_save.h).
 *
 * A data.bin, as a Wii's Data Management copies a save to an SD card, is
 *   0x0000  a header, 0xF0C0 bytes, AES-128-CBC encrypted with the SD key
 *           and IV: the title's ID (8 bytes), the banner's size (4), and
 *           from 0x20 the banner (banner.bin);
 *   0xF0C0  the "Bk" header, 0x80 bytes, plain: its size (0x70), magic
 *           0x426B0001, the console's ID, the number of files...;
 *   0xF140  each file: a 0x80-byte header, plain (magic 0x03ADF17E, size,
 *           permissions, attributes, type (1 a file, 2 a folder), name at
 *           0x0B, the IV its data is encrypted with at 0x50), then its
 *           data, encrypted with the SD key and that IV, padded to 0x40;
 *   then the console's certificates, which aren't needed here. */
#include "wii_save.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#define mkdir_one(p) _mkdir(p)
#define rmdir_one(p) _rmdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0770)
#define rmdir_one(p) rmdir(p)
#endif

const uint8_t kWiiSdKey[16] = {0xAB, 0x01, 0xB9, 0xD8, 0xE1, 0x62, 0x2B, 0x08,
                               0xAF, 0xBA, 0xD8, 0x4D, 0xBF, 0xC2, 0xA5, 0x5D};
const uint8_t kWiiSdIv[16] = {0x21, 0x67, 0x12, 0xE6, 0xAA, 0x1F, 0x68, 0x9F,
                              0x95, 0xC5, 0xA2, 0x23, 0x24, 0xDC, 0x6A, 0x98};

#define TITLE_HI 0x00010000u
#define TITLE_LO 0x52384145u /* R8AE */
#define HEADER_SIZE 0xF0C0u
#define BANNER_MAX (HEADER_SIZE - 0x20)
#define BK_SIZE 0x80u
#define FILE_HEADER 0x80u

const char* wiisave_message(int result) {
    switch (result) {
    case WIISAVE_OK: return "Imported";
    case WIISAVE_NOT_A_SAVE: return "Not a Wii save";
    case WIISAVE_OTHER_GAME: return "Another game's save";
    case WIISAVE_UNREADABLE: return "Couldn't read it";
    default: return "Couldn't save it";
    }
}

/* ------------------------------------------------------------------------ */
/* AES-128 */

static uint8_t sSbox[256], sInv[256];

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1B)); }

static uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) r ^= a;
    return r;
}

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

/* The S-box, from its definition: the inverse in GF(2^8), transformed. */
static void aes_init(void) {
    uint8_t p = 1, q = 1;
    int i;
    if (sSbox[0]) return;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0)); /* p * 3 */
        q ^= (uint8_t)(q << 1);                              /* q / 3 */
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        sSbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sSbox[0] = 0x63;
    for (i = 0; i < 256; i++) sInv[sSbox[i]] = (uint8_t)i;
}

static void expand(const uint8_t key[16], uint8_t rk[176]) {
    uint8_t rcon = 1;
    int i;
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(sSbox[t[1]] ^ rcon);
            t[1] = sSbox[t[2]];
            t[2] = sSbox[t[3]];
            t[3] = sSbox[u];
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 16] ^ t[0];
        rk[i + 1] = rk[i - 15] ^ t[1];
        rk[i + 2] = rk[i - 14] ^ t[2];
        rk[i + 3] = rk[i - 13] ^ t[3];
    }
}

static void add_key(uint8_t s[16], const uint8_t* k) {
    int i;
    for (i = 0; i < 16; i++) s[i] ^= k[i];
}

/* s[c * 4 + r] is row r of column c. */
static void encrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    uint8_t t[16];
    int round, c, r;
    add_key(s, rk);
    for (round = 1; round <= 10; round++) {
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++) t[c * 4 + r] = sSbox[s[((c + r) % 4) * 4 + r]];
        if (round < 10) {
            for (c = 0; c < 4; c++) {
                uint8_t* a = t + c * 4;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                s[c * 4] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
                s[c * 4 + 1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
                s[c * 4 + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
                s[c * 4 + 3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
            }
        } else {
            memcpy(s, t, 16);
        }
        add_key(s, rk + round * 16);
    }
}

static void decrypt_block(const uint8_t rk[176], uint8_t s[16]) {
    uint8_t t[16];
    int round, c, r;
    add_key(s, rk + 160);
    for (round = 9; round >= 0; round--) {
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++) t[((c + r) % 4) * 4 + r] = sInv[s[c * 4 + r]];
        memcpy(s, t, 16);
        add_key(s, rk + round * 16);
        if (round > 0) {
            for (c = 0; c < 4; c++) {
                uint8_t* a = s + c * 4;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                a[1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                a[2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                a[3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
        }
    }
}

void wiisave_aes_cbc(const uint8_t key[16], uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n, int decrypt) {
    uint8_t rk[176], b[16];
    size_t i;
    int k;
    aes_init();
    expand(key, rk);
    for (i = 0; i + 16 <= n; i += 16) {
        memcpy(b, in + i, 16);
        if (decrypt) {
            uint8_t next[16];
            memcpy(next, b, 16);
            decrypt_block(rk, b);
            for (k = 0; k < 16; k++) out[i + k] = b[k] ^ iv[k];
            memcpy(iv, next, 16);
        } else {
            for (k = 0; k < 16; k++) b[k] ^= iv[k];
            encrypt_block(rk, b);
            memcpy(out + i, b, 16);
            memcpy(iv, b, 16);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Reading a save */

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A plain file name: no folders, nothing hidden or odd. */
static int plain_name(const char* s) {
    size_t i, n = strlen(s);
    if (n == 0 || n >= 64 || s[0] == '.') return 0;
    for (i = 0; i < n; i++)
        if (s[i] == '/' || s[i] == '\\' || s[i] == ':' || (unsigned char)s[i] < 0x20) return 0;
    return 1;
}

int wiisave_check_files(const WiiSaveFile* files, int count) {
    int i, game = 0;
    for (i = 0; i < count; i++) {
        const char* s = files[i].name;
        if (!plain_name(s)) return 0;
        if (!strcmp(s, "GWD") || (s[0] == 'G' && s[1] == 'D' && s[2] >= '0' && s[2] <= '3' && !s[3])) game = 1;
    }
    return game;
}

int wiisave_read_bin(uint8_t* bin, size_t size, WiiSaveFile* files, int max, int* count) {
    uint8_t iv[16];
    uint32_t banner, nfiles, i;
    size_t at;
    *count = 0;
    if (size < HEADER_SIZE + BK_SIZE || max < 1) return WIISAVE_NOT_A_SAVE;
    memcpy(iv, kWiiSdIv, 16);
    wiisave_aes_cbc(kWiiSdKey, iv, bin, bin, HEADER_SIZE, 1);
    if (be32(bin) != TITLE_HI || be32(bin + HEADER_SIZE) != 0x70 || be32(bin + HEADER_SIZE + 4) != 0x426B0001u)
        return WIISAVE_NOT_A_SAVE;
    if (be32(bin + 4) != TITLE_LO) return WIISAVE_OTHER_GAME;
    banner = be32(bin + 8);
    if (banner == 0 || banner > BANNER_MAX) return WIISAVE_NOT_A_SAVE;
    strcpy(files[0].name, "banner.bin");
    files[0].data = bin + 0x20;
    files[0].size = banner;
    *count = 1;
    nfiles = be32(bin + HEADER_SIZE + 0x0C);
    at = HEADER_SIZE + BK_SIZE;
    for (i = 0; i < nfiles; i++) {
        uint8_t* h = bin + at;
        uint32_t fsize, padded;
        if (at + FILE_HEADER > size || be32(h) != 0x03ADF17Eu) return WIISAVE_NOT_A_SAVE;
        fsize = be32(h + 4);
        padded = (fsize + 0x3F) & ~0x3Fu;
        if (padded < fsize || at + FILE_HEADER + padded > size) return WIISAVE_NOT_A_SAVE;
        at += FILE_HEADER;
        if (h[0x0A] == 1) {
            if (*count >= max) return WIISAVE_NOT_A_SAVE;
            memcpy(iv, h + 0x50, 16);
            wiisave_aes_cbc(kWiiSdKey, iv, bin + at, bin + at, padded, 1);
            memcpy(files[*count].name, h + 0x0B, 0x40);
            files[*count].name[63] = 0;
            files[*count].data = bin + at;
            files[*count].size = fsize;
            ++*count;
            at += padded;
        } else if (h[0x0A] != 2) {
            return WIISAVE_NOT_A_SAVE;
        }
    }
    return wiisave_check_files(files, *count) ? WIISAVE_OK : WIISAVE_NOT_A_SAVE;
}

/* ------------------------------------------------------------------------ */
/* Putting it in place */

/* A folder of plain files, and the folder, deleted; 1 if it's gone. */
static int remove_dir(const char* dir) {
    char path[1024];
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(path, sizeof(path), "%s/*", dir);
    if ((h = FindFirstFileA(path, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, fd.cFileName);
            remove(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            remove(path);
        }
        closedir(d);
    }
#endif
    return rmdir_one(dir) == 0 || errno == ENOENT;
}

static void make_dirs(const char* path) {
    char tmp[1024];
    size_t i;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            mkdir_one(tmp);
            tmp[i] = c;
        }
    }
    mkdir_one(tmp);
}

static int exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int wiisave_install(const char* nand_root, const WiiSaveFile* files, int count) {
    char title[1024], dir[1100], kept[1100], fresh[1100], path[1200];
    int i;
    if (!wiisave_check_files(files, count)) return WIISAVE_NOT_A_SAVE;
    snprintf(title, sizeof(title), "%s/title/00010000/52384145", nand_root);
    snprintf(dir, sizeof(dir), "%s/data", title);
    snprintf(kept, sizeof(kept), "%s/data before import", title);
    snprintf(fresh, sizeof(fresh), "%s/data importing", title);
    /* Written beside it first, so a failure leaves the save as it was. */
    remove_dir(fresh);
    make_dirs(fresh);
    for (i = 0; i < count; i++) {
        FILE* f;
        int ok;
        snprintf(path, sizeof(path), "%s/%s", fresh, files[i].name);
        if (!(f = fopen(path, "wb"))) break;
        ok = fwrite(files[i].data, 1, files[i].size, f) == files[i].size;
        if (fclose(f) != 0 || !ok) break;
    }
    if (i < count || !remove_dir(kept)) {
        remove_dir(fresh);
        return WIISAVE_UNWRITABLE;
    }
    if (exists(dir) && rename(dir, kept) != 0) {
        remove_dir(fresh);
        return WIISAVE_UNWRITABLE;
    }
    if (rename(fresh, dir) != 0) {
        rename(kept, dir);
        remove_dir(fresh);
        return WIISAVE_UNWRITABLE;
    }
    return WIISAVE_OK;
}

int wiisave_import_files(const char* nand_root, WiiSaveFile* files, int count) {
    WiiSaveFile in_bin[40];
    int n, r;
    if (count == 1 && !wiisave_own_name(files[0].name)) { /* a data.bin */
        r = wiisave_read_bin((uint8_t*)files[0].data, files[0].size, in_bin, 40, &n);
        return r != WIISAVE_OK ? r : wiisave_install(nand_root, in_bin, n);
    }
    return wiisave_install(nand_root, files, count);
}

int wiisave_own_name(const char* s) {
    size_t n = strlen(s);
    if (!strcmp(s, "GWD") || !strcmp(s, "banner.bin")) return 1;
    if (n < 3 || s[n - 1] < '0' || s[n - 1] > '9') return 0;
    return (n == 3 && s[0] == 'G' && s[1] == 'D') || (n == 4 && !strncmp(s, "Pic", 3));
}
