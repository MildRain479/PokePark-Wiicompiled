/* The disc checks (disc_verify.h). The reasons given and reading dtk's
 * output follow SparkPark (Dvrkie13/SparkPark, CC0). */
#include "disc_verify.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The text after the first "key" in out, its leading spaces skipped. */
static const char* after(const char* out, const char* key) {
    const char* p = out ? strstr(out, key) : NULL;
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Whether the run of hex digits at p is want. */
static int hex_is(const char* p, const char* want) {
    size_t i, n = strlen(want);
    if (!p) return 0;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)p[i]) != want[i]) return 0;
    }
    return !isxdigit((unsigned char)p[n]);
}

static const char* region_name(char c) {
    switch (c) {
    case 'P': return "the European release";
    case 'J': return "the Japanese release";
    case 'K': return "the Korean release";
    case 'W': return "the Taiwanese release";
    default: return "another region's release";
    }
}

disc_result disc_check_id(const char* id, int revision, char* msg, size_t n) {
    if (memcmp(id, "R8A", 3) != 0 || memcmp(id + 4, "01", 2) != 0) {
        snprintf(msg, n, "That disc image is another game (%.6s), not PokePark Wii: Pikachu's Adventure (USA, R8AE01).",
                 id);
        return DISC_WRONG_GAME;
    }
    if (id[3] != 'E') {
        snprintf(msg, n, "That's %s of PokePark Wii (%.6s). This needs the USA release, R8AE01.", region_name(id[3]),
                 id);
        return DISC_WRONG_REGION;
    }
    if (revision != 0) {
        snprintf(msg, n, "That's revision %d of PokePark Wii (USA). This needs the first release, revision 0.",
                 revision);
        return DISC_WRONG_REVISION;
    }
    return DISC_OK;
}

disc_result disc_check_info(const char* info, char* msg, size_t n) {
    const char* id = after(info, "Game ID:");
    const char* rev = after(info, "Revision");
    if (!id || strlen(id) < 6) {
        snprintf(msg, n, "That file couldn't be read as a disc image. Choose a disc image of PokePark Wii (USA): an "
                         "ISO, RVZ, WBFS or the like.");
        return DISC_UNREADABLE;
    }
    return disc_check_id(id, rev ? atoi(rev) : -1, msg, n);
}

disc_result disc_check_dol(const uint8_t* dol, size_t len, char* msg, size_t n) {
    char sha1[41];
    disc_sha1(dol, len, sha1);
    if (!strcmp(sha1, DISC_MAIN_DOL_SHA1)) return DISC_OK;
    snprintf(msg, n, "That disc image's game code isn't the original's: it has been patched or modded, and this can't "
                     "run it. Use an unmodified copy of your disc.");
    return DISC_WRONG_CODE;
}

static uint32_t be32(disc_read_fn read, void* user, uint64_t offset) {
    uint8_t b[4];
    if (!read(user, offset, b, 4)) return 0;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

#define WII_MAGIC 0x5D1C9EA3u /* at 0x18 of a Wii disc's header */
#define NKIT_MAGIC 0x4E4B4954u /* "NKIT", at 0x200 of an NKit ISO */

/* What a plain Wii ISO's data partition needs: from the partition table
 * (0x40000) and the partition's header (its data's offset and size,
 * 0x2B8), or 0 if there's none. */
static uint64_t iso_needs(disc_read_fn read, void* user) {
    uint32_t g, i;
    for (g = 0; g < 4; g++) {
        uint32_t count = be32(read, user, 0x40000 + g * 8);
        uint64_t table = (uint64_t)be32(read, user, 0x40004 + g * 8) << 2;
        for (i = 0; i < count && i < 64; i++) {
            uint64_t at = (uint64_t)be32(read, user, table + i * 8) << 2;
            if (be32(read, user, table + i * 8 + 4) != 0) continue; /* not the game's data */
            return at + ((uint64_t)be32(read, user, at + 0x2B8) << 2) + ((uint64_t)be32(read, user, at + 0x2BC) << 2);
        }
    }
    return 0;
}

disc_result disc_check_file(disc_read_fn read, void* user, uint64_t size, char* msg, size_t n) {
    uint8_t head[0x34];
    uint64_t want = 0;
    if (!read(user, 0, head, sizeof(head))) return DISC_OK; /* too short to say: let the reader say */
    if (!memcmp(head, "RVZ\1", 4) || !memcmp(head, "WIA\1", 4)) {
        int i;
        for (i = 0; i < 8; i++) want = want << 8 | head[0x2C + i]; /* the file's size */
    } else if (be32(read, user, 0x18) == WII_MAGIC) {
        if (be32(read, user, 0x200) == NKIT_MAGIC) {
            snprintf(msg, n, "That disc image is an NKit ISO (.nkit.iso), which this can't read. Turn it back into a "
                             "whole disc image first (NKit's Recover to ISO), or use an .rvz, .iso or .wbfs file.");
            return DISC_NKIT;
        }
        want = iso_needs(read, user);
    }
    if (size >= want) return DISC_OK;
    snprintf(msg, n, "That disc image is incomplete: it has %llu of its %llu MB, so it was only partly copied or "
                     "downloaded. Copy the whole file again.",
             (unsigned long long)(size >> 20), (unsigned long long)(want >> 20));
    return DISC_INCOMPLETE;
}

int disc_is_original(const char* verify) {
    const char* crc = after(verify, "CRC32 :");
    return hex_is(after(verify, "SHA-1 :"), DISC_REDUMP_SHA1) && (!crc || hex_is(crc, DISC_REDUMP_CRC32));
}

/* SHA-1 (FIPS 180). */
#define ROL(v, s) (((v) << (s)) | ((v) >> (32 - (s))))

static void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        uint32_t f = i < 20 ? ((b & c) | (~b & d)) + 0x5A827999u
                   : i < 40 ? (b ^ c ^ d) + 0x6ED9EBA1u
                   : i < 60 ? ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDCu
                            : (b ^ c ^ d) + 0xCA62C1D6u;
        uint32_t t = ROL(a, 5) + f + e + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void disc_sha1(const uint8_t* data, size_t len, char out[41]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t last[128];
    uint64_t bits = (uint64_t)len * 8;
    size_t left = len % 64, i, size = left < 56 ? 64 : 128;
    for (i = 0; i + 64 <= len; i += 64) sha1_block(h, data + i);
    memset(last, 0, sizeof(last));
    if (left) memcpy(last, data + i, left);
    last[left] = 0x80;
    for (i = 0; i < 8; i++) last[size - 1 - i] = (uint8_t)(bits >> (i * 8));
    sha1_block(h, last);
    if (size == 128) sha1_block(h, last + 64);
    for (i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
}
