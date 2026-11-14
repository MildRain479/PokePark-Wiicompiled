// Checks recompiled game functions against two references:
//  - the decompiled source in src/, compiled natively for the host, and
//  - the host C library, for the game's copies of the MSL C library.
//
// Usage: test_recomp <path to main.dol>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "recomp_funcs.h"
extern "C" {
#include "host_sdcard.h"
}

// The decompiled source, built for the host.
#include "flib/CodepointConvert.h"
void Demangle_FName(const u32* in, char* out);

// Guest functions under test.
static const uint32_t kUnicodeToSJIS = 0x802EDC10;
static const uint32_t kEncodeCodepoint = 0x802EDA20;
static const uint32_t kDemangleFName = 0x802ED920;
static const uint32_t kStrlen = 0x802F3464;
static const uint32_t kStrcmp = 0x802FA7B8;
static const uint32_t kSprintf = 0x802FA410;
static const uint32_t kAtoi = 0x802FAF30;
static const uint32_t kSin = 0x803012AC;
static const uint32_t kCos = 0x80300DA4;
static const uint32_t kTan = 0x80301378;
static const uint32_t kAtan = 0x803009FC;
static const uint32_t kAtan2 = 0x803013F8;
static const uint32_t kPow = 0x80301404;
static const uint32_t kSqrt = 0x80301650;
static const uint32_t kFmod = 0x803013FC;
static const uint32_t kInitRegisters = 0x800041C0;

// Scratch guest memory for arguments and results, well above the DOL's bss.
static const uint32_t kBuf = 0x81000000;
static const uint32_t kBuf2 = 0x81001000;

static ppc_t gCpu;
static ppc_t gReset;
static int gFailures;
static int gChecks;

static void call(uint32_t addr) {
    ppc_func fn = ppc_lookup(addr);
    if (!fn) {
        fprintf(stderr, "no function at %08X\n", addr);
        exit(1);
    }
    // Restore the stack and small-data pointers between calls so one
    // test can't leak state into the next.
    gCpu.r[1] = gReset.r[1];
    gCpu.r[2] = gReset.r[2];
    gCpu.r[13] = gReset.r[13];
    fn(&gCpu);
}

static void check(bool ok, const char* test, const char* fmt, ...) {
    gChecks++;
    if (ok) return;
    if (++gFailures <= 20) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FAIL %s: ", test);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}

static void put_str(uint32_t addr, const char* s) { memcpy(MA(addr), s, strlen(s) + 1); }

static double ulps(double a, double b) {
    if (a == b || (std::isnan(a) && std::isnan(b))) return 0;
    int64_t ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    if (ia < 0) ia = INT64_MIN - ia;
    if (ib < 0) ib = INT64_MIN - ib;
    return std::fabs((double)(ia - ib));
}

static void test_unicode_to_sjis() {
    int before = gFailures;
    for (int64_t cp = -0x100; cp < 0x110100; cp++) {
        gCpu.r[3] = (uint32_t)cp;
        call(kUnicodeToSJIS);
        int32_t want = UnicodeToSJIS((s32)cp);
        check((int32_t)gCpu.r[3] == want, "UnicodeToSJIS", "cp %X: got %X want %X", (unsigned)cp, gCpu.r[3], want);
    }
    printf("UnicodeToSJIS: %s over 1.1M code points\n", gFailures == before ? "match" : "MISMATCH");
}

static void test_encode_codepoint() {
    int before = gFailures;
    std::mt19937 rng(1);
    for (int enc = 0; enc <= 4; enc++) {
        for (int n = 0; n < 300000; n++) {
            int32_t cp = n < 200000 ? n : (int32_t)rng();
            char host[8] = {0};
            int want = EncodeCodepoint(host, cp, enc);
            if (enc == ENCODING_UTF16) {
                // The host wrote native-endian u16s; the guest writes big-endian.
                for (int i = 0; i + 1 < 8; i += 2) std::swap(host[i], host[i + 1]);
            }
            memset(MA(kBuf), 0, 8);
            gCpu.r[3] = kBuf;
            gCpu.r[4] = (uint32_t)cp;
            gCpu.r[5] = (uint32_t)enc;
            call(kEncodeCodepoint);
            int got = (int32_t)gCpu.r[3];
            bool ok = got == want && (want <= 0 || memcmp(MA(kBuf), host, want) == 0);
            check(ok, "EncodeCodepoint", "enc %d cp %X: got %d want %d", enc, cp, got, want);
        }
    }
    printf("EncodeCodepoint: %s over 1.5M inputs\n", gFailures == before ? "match" : "MISMATCH");
}

static void test_demangle() {
    int before = gFailures;
    std::mt19937 rng(2);
    for (int n = 0; n < 200000; n++) {
        u32 in[2] = {(u32)rng(), (u32)rng()};
        if (n & 1) in[0] = in[0] % (40u * 40 * 40 * 40 * 40 * 40);  // keep some words "full"
        char host[16];
        memset(host, 0x55, sizeof(host));
        Demangle_FName(in, host);
        ST32(kBuf, in[0]);
        ST32(kBuf + 4, in[1]);
        memset(MA(kBuf2), 0x55, 16);
        gCpu.r[3] = kBuf;
        gCpu.r[4] = kBuf2;
        call(kDemangleFName);
        check(memcmp(MA(kBuf2), host, 14) == 0, "Demangle_FName", "%08X %08X", (unsigned)in[0], (unsigned)in[1]);
    }
    printf("Demangle_FName: %s over 200K inputs\n", gFailures == before ? "match" : "MISMATCH");
}

static void test_strings() {
    int before = gFailures;
    const char* words[] = {"", "a", "PokePark", "Pikachu", "Pikachu!", "pikachu", "Wii", "WiiWare", "Treehouse"};
    for (const char* a : words) {
        put_str(kBuf, a);
        gCpu.r[3] = kBuf;
        call(kStrlen);
        check(gCpu.r[3] == strlen(a), "strlen", "\"%s\"", a);
        for (const char* b : words) {
            put_str(kBuf, a);
            put_str(kBuf2, b);
            gCpu.r[3] = kBuf;
            gCpu.r[4] = kBuf2;
            call(kStrcmp);
            int got = (int32_t)gCpu.r[3], want = strcmp(a, b);
            check((got > 0) == (want > 0) && (got < 0) == (want < 0), "strcmp", "\"%s\" \"%s\"", a, b);
        }
    }
    const char* nums[] = {"0", "42", "-17", "  123abc", "2147483647", "-2147483648"};
    for (const char* s : nums) {
        put_str(kBuf, s);
        gCpu.r[3] = kBuf;
        call(kAtoi);
        check((int32_t)gCpu.r[3] == atoi(s), "atoi", "\"%s\" got %d", s, (int32_t)gCpu.r[3]);
    }
    printf("strlen/strcmp/atoi: %s\n", gFailures == before ? "match" : "MISMATCH");
}

static void test_sprintf() {
    int before = gFailures;
    struct Case {
        const char* fmt;
        int a, b;
        const char* s;
    } cases[] = {
        {"%d", 12345, 0, ""},
        {"%d %d", -7, 2147483647, ""},
        {"%08X|%x", 0xBEEF, 0x80001234, ""},
        {"[%5d] [%-5d]", 42, 42, ""},
        {"%s and %d", 3, 0, ""},
        {"%c%c", 'P', 'K', ""},
        {"%u %o", 4000000000u, 8, ""},
    };
    for (const Case& k : cases) {
        char want[256];
        put_str(kBuf2, k.fmt);
        put_str(kBuf2 + 0x200, "Pikachu");
        gCpu.r[3] = kBuf;
        gCpu.r[4] = kBuf2;
        if (strstr(k.fmt, "%s")) {
            snprintf(want, sizeof(want), k.fmt, "Pikachu", k.a);
            gCpu.r[5] = kBuf2 + 0x200;
            gCpu.r[6] = (uint32_t)k.a;
        } else {
            snprintf(want, sizeof(want), k.fmt, k.a, k.b);
            gCpu.r[5] = (uint32_t)k.a;
            gCpu.r[6] = (uint32_t)k.b;
        }
        cr_setbit(&gCpu, 6, 0);  // no FP arguments in registers
        call(kSprintf);
        check(strcmp((char*)MA(kBuf), want) == 0, "sprintf", "\"%s\": got \"%s\" want \"%s\"", k.fmt,
              (char*)MA(kBuf), want);
    }
    // Floating point goes through f1 with CR bit 6 set, as MWCC passes it.
    double vals[] = {0.0, 1.5, -2.25, 3.14159265358979, 1234567.875, 1e-3};
    for (double v : vals) {
        char want[64];
        snprintf(want, sizeof(want), "%.3f", v);
        put_str(kBuf2, "%.3f");
        gCpu.r[3] = kBuf;
        gCpu.r[4] = kBuf2;
        gCpu.f[1] = v;
        cr_setbit(&gCpu, 6, 1);
        call(kSprintf);
        check(strcmp((char*)MA(kBuf), want) == 0, "sprintf %f", "%g: got \"%s\" want \"%s\"", v,
              (char*)MA(kBuf), want);
    }
    printf("sprintf: %s\n", gFailures == before ? "match" : "MISMATCH");
}

// recomp/patches/example_hook.c wraps the game's strlen. sprintf's "%s"
// calls it directly, so a working hook sees that call.
#include "../boot/disc_verify.h"

extern "C" unsigned g_strlen_calls;

static void test_disc() {
    char msg[512], sha1[41];
    const char* good = "Format: RVZ\nLossless: true\n\nTitle: Pokepark Wii\nGame ID: R8AE01\nDisc 1, Revision 0\n\n"
                       "Partition 0\n\tGame ID: RELSAB (0001000000555045)\n\tDisc 1, Revision 3\n";
    const char* verified = "Redump: PokePark Wii (USA) \xe2\x9c\x85\nCRC32 : 26e533d5 \xe2\x9c\x85\n"
                           "SHA-1 : c9a6f7601fd54b5e21a0909084839d30874a9a37 \xe2\x9c\x85\n";
    const char* other = "CRC32 : 11111111\nSHA-1 : 0123456789abcdef0123456789abcdef01234567\n";
    const char* sameSha = "CRC32 : 11111111\nSHA-1 : C9A6F7601FD54B5E21A0909084839D30874A9A37\n";
    const char* dol = "abc";
    static uint8_t zeros[120];
    check(disc_check_info(good, msg, sizeof(msg)) == DISC_OK, "disc", "the good disc refused: %s", msg);
    check(disc_check_info("nonsense", msg, sizeof(msg)) == DISC_UNREADABLE, "disc", "not a disc");
    check(disc_check_info(NULL, msg, sizeof(msg)) == DISC_UNREADABLE, "disc", "no output");
    check(disc_check_id("S2LE01", 0, msg, sizeof(msg)) == DISC_WRONG_GAME, "disc", "PokePark 2");
    check(disc_check_id("R8AP01", 0, msg, sizeof(msg)) == DISC_WRONG_REGION, "disc", "Europe");
    check(disc_check_id("R8AJ01", 0, msg, sizeof(msg)) == DISC_WRONG_REGION, "disc", "Japan");
    check(disc_check_id("R8AE01", 1, msg, sizeof(msg)) == DISC_WRONG_REVISION, "disc", "revision 1");
    check(disc_is_original(verified), "disc", "the original's hashes");
    check(disc_is_original(sameSha) == 0, "disc", "another CRC");
    check(disc_is_original(other) == 0, "disc", "other hashes");
    check(disc_is_original("") == 0, "disc", "no hashes");
    check(disc_is_original(NULL) == 0, "disc", "no output");
    disc_sha1((const uint8_t*)dol, 3, sha1);
    check(!strcmp(sha1, "a9993e364706816aba3e25717850c26c9cd0d89d"), "disc", "sha1(abc) = %s", sha1);
    disc_sha1((const uint8_t*)dol, 0, sha1);
    check(!strcmp(sha1, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "disc", "sha1() = %s", sha1);
    // 120 zeros: padding that needs a block of its own.
    disc_sha1(zeros, sizeof(zeros), sha1);
    check(!strcmp(sha1, "b110a88a11436b215220486c1081dec2fb0f389a"), "disc", "sha1(120 zeros) = %s", sha1);
    check(disc_check_dol(zeros, sizeof(zeros), msg, sizeof(msg)) == DISC_WRONG_CODE, "disc", "another main.dol");
}

// An image file's start, in memory: past its end, zeros, as a file read
// sparsely; how big the file is is said separately.
static uint8_t sImage[0x50400];

static int image_read(void*, uint64_t offset, void* out, size_t len) {
    memset(out, 0, len);
    if (offset < sizeof(sImage)) memcpy(out, sImage + offset, std::min<size_t>(len, sizeof(sImage) - (size_t)offset));
    return 1;
}

static void put_be32(uint32_t at, uint32_t v) {
    sImage[at] = (uint8_t)(v >> 24);
    sImage[at + 1] = (uint8_t)(v >> 16);
    sImage[at + 2] = (uint8_t)(v >> 8);
    sImage[at + 3] = (uint8_t)v;
}

static void test_disc_file() {
    char msg[512];
    // A Wii ISO: its data partition at 0x50000, the data 0x20000 into it,
    // 0x100000 long, so ending at 0x170000. The update partition (type 1)
    // listed first, which isn't the one.
    memset(sImage, 0, sizeof(sImage));
    memcpy(sImage, "R8AE01", 6);
    put_be32(0x18, 0x5D1C9EA3);
    put_be32(0x40000, 2);
    put_be32(0x40004, 0x40020 >> 2);
    put_be32(0x40020, 0x48000 >> 2);
    put_be32(0x40024, 1);
    put_be32(0x40028, 0x50000 >> 2);
    put_be32(0x4002C, 0);
    put_be32(0x502B8, 0x20000 >> 2);
    put_be32(0x502BC, 0x100000 >> 2);
    check(disc_check_file(image_read, NULL, 0x170000, msg, sizeof(msg)) == DISC_OK, "disc file", "whole ISO: %s", msg);
    check(disc_check_file(image_read, NULL, 0x200000, msg, sizeof(msg)) == DISC_OK, "disc file", "ISO with padding");
    check(disc_check_file(image_read, NULL, 0x16FFFF, msg, sizeof(msg)) == DISC_INCOMPLETE, "disc file", "ISO cut short");
    memcpy(sImage + 0x200, "NKIT", 4);
    check(disc_check_file(image_read, NULL, 0x170000, msg, sizeof(msg)) == DISC_NKIT, "disc file", "NKit ISO");
    // An RVZ: its file size at 0x2C.
    memset(sImage, 0, sizeof(sImage));
    memcpy(sImage, "RVZ\1", 4);
    put_be32(0x2C, 0);
    put_be32(0x30, 0x59000000);
    check(disc_check_file(image_read, NULL, 0x59000000, msg, sizeof(msg)) == DISC_OK, "disc file", "whole RVZ");
    check(disc_check_file(image_read, NULL, 0x100000, msg, sizeof(msg)) == DISC_INCOMPLETE, "disc file", "RVZ cut short");
    // Something else: the reader's to judge.
    memset(sImage, 0, sizeof(sImage));
    memcpy(sImage, "WBFS", 4);
    check(disc_check_file(image_read, NULL, 0x100000, msg, sizeof(msg)) == DISC_OK, "disc file", "WBFS passed on");
}

static void test_hook() {
    unsigned before = g_strlen_calls;
    put_str(kBuf2, "[%s]");
    put_str(kBuf2 + 0x200, "Pikachu");
    gCpu.r[3] = kBuf;
    gCpu.r[4] = kBuf2;
    gCpu.r[5] = kBuf2 + 0x200;
    cr_setbit(&gCpu, 6, 0);
    call(kSprintf);
    bool ok = g_strlen_calls > before && strcmp((char*)MA(kBuf), "[Pikachu]") == 0;
    check(ok, "hook", "strlen hook saw %u calls, output \"%s\"", g_strlen_calls - before, (char*)MA(kBuf));
    printf("hook: %s (game-internal call to strlen reached the patch)\n", ok ? "works" : "FAILED");
}

static void test_math() {
    struct Fn {
        const char* name;
        uint32_t addr;
        double (*host)(double);
    } fns[] = {
        {"sin", kSin, [](double x) { return std::sin(x); }},
        {"cos", kCos, [](double x) { return std::cos(x); }},
        {"tan", kTan, [](double x) { return std::tan(x); }},
        {"atan", kAtan, [](double x) { return std::atan(x); }},
        {"sqrt", kSqrt, [](double x) { return std::sqrt(std::fabs(x)); }},
    };
    std::mt19937_64 rng(3);
    std::uniform_real_distribution<double> dist(-100.0, 100.0);
    for (const Fn& f : fns) {
        double worst = 0;
        for (int n = 0; n < 100000; n++) {
            double x = dist(rng);
            if (f.addr == kSqrt) x = std::fabs(x);
            gCpu.f[1] = x;
            call(f.addr);
            double u = ulps(gCpu.f[1], f.host(x));
            if (u > 1 && worst <= 1) fprintf(stderr, "  %s(%.17g) = %.17g, host %.17g\n", f.name, x, gCpu.f[1], f.host(x));
            worst = std::fmax(worst, u);
        }
        // fdlibm and the host library may round the last bit differently.
        check(worst <= 1, f.name, "worst error %.0f ulp", worst);
        printf("%s: worst difference from host libm %.0f ulp over 100K inputs\n", f.name, worst);
    }
    double worst = 0;
    for (int n = 0; n < 100000; n++) {
        double x = std::fabs(dist(rng)) / 10, y = dist(rng) / 10;
        gCpu.f[1] = x;
        gCpu.f[2] = y;
        call(kPow);
        worst = std::fmax(worst, ulps(gCpu.f[1], std::pow(x, y)));
        gCpu.f[1] = y;
        gCpu.f[2] = x;
        call(kAtan2);
        worst = std::fmax(worst, ulps(gCpu.f[1], std::atan2(y, x)));
        gCpu.f[1] = y * 10;
        gCpu.f[2] = x + 0.5;
        call(kFmod);
        worst = std::fmax(worst, ulps(gCpu.f[1], std::fmod(y * 10, x + 0.5)));
    }
    check(worst <= 1, "pow/atan2/fmod", "worst error %.0f ulp", worst);
    printf("pow/atan2/fmod: worst difference from host libm %.0f ulp over 300K inputs\n", worst);
}

// The SD card (hle_sd.c), through FA's calls as the game's SD manager
// makes them to copy a photo: mount, the free space, the scan for the
// next number, the folder made, the file opened "r+" (missing), made,
// closed and opened again, written and closed; then the scan again.
static std::string gSaved;
static int sd_choose() { return 1; }
static int sd_save(const char* name, const uint8_t* data, uint32_t size) {
    gSaved = std::string(name) + ":" + std::string((const char*)data, size);
    return 1;
}

static int32_t fa(uint32_t addr, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    gCpu.r[3] = a;
    gCpu.r[4] = b;
    gCpu.r[5] = c;
    gCpu.r[6] = d;
    call(addr);
    return (int32_t)gCpu.r[3];
}

static void test_sd() {
    enum : uint32_t {
        kAttach = 0x802E4050, kMount = 0x802E46C0, kDevinf = 0x802E4550, kErrnum = 0x802E43B0,
        kOpendir = 0x802E46F0, kMkdir = 0x802E4690, kFopen = 0x802E4450, kCreate = 0x802E4280,
        kFclose = 0x802E43F0, kFsize = 0x802E4420, kFseek = 0x802E4480, kFwrite = 0x802E4540,
        kFsfirst = 0x802E44B0, kFsnext = 0x802E44E0, kFstat = 0x802E4510
    };
    const uint32_t path = kBuf, mode = kBuf + 0x100, info = kBuf + 0x200, data = kBuf + 0x300, ent = kBuf2;
    g_host_sd_choose = sd_choose;
    g_host_sd_save = sd_save;
    memset(MA(info), 0, 0x40);
    fa(kAttach, 0, 0, 0, info);
    check(*(uint8_t*)MA(info + 8) == 'A' && (*(uint8_t*)MA(info + 9) & 0x10), "SD", "no card in drive A");
    check(fa(kMount, 'A') == 0, "SD", "mount");
    fa(kDevinf, 'A', info);
    check((int32_t)(LD32(info + 4) * LD32(info + 8) * LD32(info + 12)) > 100 * 1024 * 1024, "SD", "free space");
    put_str(path, "\\DCIM\\???NIN01\\R8A_????.JPG");
    check(fa(kFsfirst, path, 0x60, ent) != 0 && fa(kErrnum) == 2, "SD", "an empty card's scan");
    put_str(path, "\\DCIM\\100NIN01");
    check(fa(kOpendir, path) == 0, "SD", "a folder not made yet");
    put_str(path, "\\DCIM");
    check(fa(kMkdir, path) == 0, "SD", "mkdir DCIM");
    put_str(path, "\\DCIM\\100NIN01");
    check(fa(kMkdir, path) == 0 && fa(kOpendir, path) != 0, "SD", "mkdir 100NIN01");
    put_str(path, "\\DCIM\\100NIN01\\R8A_0001.JPG");
    put_str(mode, "r+");
    check(fa(kFopen, path, mode) == 0, "SD", "r+ of a missing file");
    int32_t h = fa(kCreate, path, 0);
    check(h != 0 && fa(kFclose, (uint32_t)h) == 0, "SD", "create");
    h = fa(kFopen, path, mode);
    check(h != 0 && fa(kFsize, (uint32_t)h, info) == 0 && LD32(info) == 0, "SD", "reopened, empty");
    put_str(data, "JPEGDATA");
    check(fa(kFseek, (uint32_t)h, 0, 0) == 0 && fa(kFwrite, data, 1, 8, (uint32_t)h) == 8, "SD", "write");
    check(fa(kFclose, (uint32_t)h) == 0 && gSaved == "R8A_0001.JPG:JPEGDATA", "SD", "saved \"%s\"", gSaved.c_str());
    put_str(path, "\\DCIM\\???NIN01");
    check(fa(kFsfirst, path, 0x10, ent) == 0 && !strcmp((char*)MA(ent + 0x22D), "100NIN01"), "SD", "folder scan");
    put_str(path, "\\DCIM\\100NIN01\\R8A_????.JPG");
    check(fa(kFsfirst, path, 0x60, ent) == 0 && !strcmp((char*)MA(ent + 0x22D), "R8A_0001.JPG") &&
              fa(kFsnext, ent) != 0 && fa(kErrnum) == 2,
          "SD", "file scan");
    put_str(path, "\\DCIM\\100NIN01\\R8A_0001.JPG");
    check(fa(kFstat, path, info) == 0 && LD32(info) == 8 && *(uint8_t*)MA(info + 0x10) == 0x20, "SD", "stat");
    printf("SD card: a photo copied as the game's SD manager does it\n");
}

#include "../boot/wii_save.h"

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static std::string read_file(const std::string& path) {
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    char b[4096];
    size_t n;
    if (!f) return "(none)";
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

// A data.bin made as a Wii makes one (encrypted with the SD key), read
// back, and put in place of a save in a NAND folder, twice.
static void test_wii_save() {
    // FIPS-197's example: AES-128 of 00112233... with the key 00010203...
    uint8_t key[16], block[16], iv[16] = {0}, out[16];
    static const uint8_t want[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                                     0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i, block[i] = (uint8_t)(i * 0x11);
    wiisave_aes_cbc(key, iv, block, out, 16, 0);
    check(memcmp(out, want, 16) == 0, "Wii save", "AES-128 encrypts FIPS-197's example wrong");
    memset(iv, 0, 16);
    wiisave_aes_cbc(key, iv, out, out, 16, 1);
    check(memcmp(out, block, 16) == 0, "Wii save", "AES-128 decrypts FIPS-197's example wrong");

    struct { const char* name; std::string data; } files[] = {
        {"GWD", std::string(9376, 'w')}, {"GD0", std::string(14912, '0')}, {"Pic0", std::string(1000, 'p')}};
    std::string banner(0x72A0, 'b');
    std::vector<uint8_t> bin(0xF0C0 + 0x80);
    put_be32(&bin[0], 0x00010000);
    put_be32(&bin[4], 0x52384145);
    put_be32(&bin[8], (uint32_t)banner.size());
    memcpy(&bin[0x20], banner.data(), banner.size());
    memcpy(iv, kWiiSdIv, 16);
    wiisave_aes_cbc(kWiiSdKey, iv, &bin[0], &bin[0], 0xF0C0, 0);
    put_be32(&bin[0xF0C0], 0x70);
    put_be32(&bin[0xF0C4], 0x426B0001);
    put_be32(&bin[0xF0CC], 3);
    for (auto& f : files) {
        size_t at = bin.size(), padded = (f.data.size() + 0x3F) & ~(size_t)0x3F;
        bin.resize(at + 0x80 + padded);
        put_be32(&bin[at], 0x03ADF17E);
        put_be32(&bin[at + 4], (uint32_t)f.data.size());
        bin[at + 0x0A] = 1;
        strcpy((char*)&bin[at + 0x0B], f.name);
        for (int i = 0; i < 16; i++) bin[at + 0x50 + i] = (uint8_t)(i * 7 + f.data.size());
        memcpy(iv, &bin[at + 0x50], 16);
        memcpy(&bin[at + 0x80], f.data.data(), f.data.size());
        wiisave_aes_cbc(kWiiSdKey, iv, &bin[at + 0x80], &bin[at + 0x80], padded, 0);
    }
    bin.resize(bin.size() + 0x340); // the certificates
    std::vector<uint8_t> other = bin;

    WiiSaveFile got[16];
    int n = 0;
    check(wiisave_read_bin(&bin[0], bin.size(), got, 16, &n) == WIISAVE_OK && n == 4, "Wii save",
          "data.bin not read (%d files)", n);
    check(n == 4 && !strcmp(got[0].name, "banner.bin") && std::string((const char*)got[0].data, got[0].size) == banner,
          "Wii save", "banner");
    for (int i = 1; i < n; i++)
        check(!strcmp(got[i].name, files[i - 1].name) &&
                  std::string((const char*)got[i].data, got[i].size) == files[i - 1].data,
              "Wii save", "file %d", i);
    memcpy(iv, kWiiSdIv, 16);
    wiisave_aes_cbc(kWiiSdKey, iv, &other[0], &other[0], 0xF0C0, 1);
    put_be32(&other[4], 0x52384150); // R8AP: Europe's
    memcpy(iv, kWiiSdIv, 16);
    wiisave_aes_cbc(kWiiSdKey, iv, &other[0], &other[0], 0xF0C0, 0);
    check(wiisave_read_bin(&other[0], other.size(), got + 8, 8, &n) == WIISAVE_OTHER_GAME, "Wii save",
          "another game's save taken");
    std::vector<uint8_t> junk(0x20000, 0x5A);
    check(wiisave_read_bin(&junk[0], junk.size(), got + 8, 8, &n) == WIISAVE_NOT_A_SAVE, "Wii save", "junk taken");

    // A zip of Dolphin's folder (Android's picker can't choose a folder), made
    // by Python's zipfile: GWD and GD0 deflated (fixed and dynamic codes),
    // banner.bin stored, and macOS's ._ file passed by.
    static const uint8_t zip[] = {
        0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0x19, 0x11, 0xB7, 0x04, 0x1A, 0x00,
        0x00, 0x00, 0xA0, 0x24, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x47, 0x57, 0x44, 0xED, 0xC1,
        0x31, 0x01, 0x00, 0x00, 0x00, 0xC2, 0xA0, 0xD8, 0xAB, 0x6F, 0x0A, 0x3F, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x3E, 0x06, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0x82, 0x0D,
        0x0C, 0x4A, 0x8B, 0x02, 0x00, 0x00, 0x70, 0x03, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x47,
        0x44, 0x30, 0x63, 0x60, 0x97, 0x71, 0x28, 0xDC, 0xF0, 0x2F, 0xF2, 0x90, 0xD5, 0xFE, 0xA0, 0x2F, 0x8B, 0x13, 0x74, 0x58,
        0xDF, 0x3C, 0x7A, 0xFA, 0x4D, 0xCC, 0xB9, 0xEE, 0x84, 0x7C, 0xCB, 0x8F, 0x4A, 0x8E, 0x65, 0x81, 0x5C, 0x97, 0x96, 0x77,
        0xD5, 0x54, 0x77, 0x2C, 0x39, 0xCB, 0xE2, 0x35, 0xF7, 0x5F, 0xDE, 0xEB, 0x32, 0x81, 0xED, 0x39, 0x06, 0x8C, 0x0F, 0xCE,
        0x9D, 0xBC, 0xF4, 0x8A, 0xDF, 0xA9, 0xE5, 0xB2, 0xC1, 0x1C, 0xD1, 0x39, 0x46, 0x57, 0xDB, 0xDC, 0x84, 0xDF, 0xDD, 0x38,
        0x7F, 0xE5, 0x05, 0xA7, 0x45, 0xD9, 0x41, 0xA9, 0xA6, 0xEF, 0x55, 0x3C, 0xAB, 0x23, 0x44, 0xEE, 0x6E, 0x99, 0xD5, 0xDB,
        0x37, 0x6F, 0xF7, 0x33, 0x85, 0xF4, 0x3D, 0x0A, 0x13, 0x05, 0xE6, 0x59, 0x3E, 0x9A, 0x15, 0x6F, 0x24, 0xC2, 0xCC, 0xC0,
        0xA3, 0xEA, 0xD3, 0x74, 0x54, 0xAC, 0xEC, 0x71, 0xDC, 0x8B, 0x7A, 0x95, 0x1B, 0x33, 0x33, 0xDC, 0x0C, 0xB5, 0x8C, 0xDC,
        0xB3, 0xE6, 0xDC, 0xD6, 0x68, 0x79, 0x9B, 0xF2, 0xAA, 0x56, 0xEE, 0x5C, 0x77, 0x98, 0x81, 0x38, 0x8F, 0x80, 0xA2, 0x43,
        0xDE, 0xCA, 0x4F, 0x5E, 0xEB, 0x95, 0x96, 0x18, 0x5F, 0xA8, 0x31, 0xFD, 0x73, 0x69, 0xEB, 0xB2, 0x65, 0x9B, 0xCF, 0xFD,
        0xD0, 0x2F, 0x39, 0xA1, 0x39, 0x43, 0x6C, 0xA1, 0xD5, 0xA3, 0xE9, 0x51, 0x3A, 0xDC, 0x3F, 0xBE, 0xFC, 0x15, 0xB1, 0xCA,
        0x5D, 0xF7, 0x2F, 0xFA, 0x98, 0xC3, 0xF1, 0x18, 0x86, 0x8D, 0x05, 0x76, 0x92, 0x4C, 0xBF, 0xFE, 0x0B, 0x99, 0x24, 0x2F,
        0x78, 0xE3, 0xBA, 0x46, 0x69, 0xA9, 0xD9, 0xB5, 0x66, 0x3B, 0x8E, 0xFB, 0x47, 0x76, 0xEC, 0x3C, 0xF1, 0x4C, 0xD0, 0x6B,
        0xD2, 0x73, 0xAF, 0x3D, 0xD6, 0x27, 0x52, 0x78, 0x8F, 0x74, 0xC5, 0xDA, 0xE9, 0x69, 0x9B, 0x05, 0x94, 0xAF, 0xF9, 0xE0,
        0xB8, 0x80, 0xAF, 0x53, 0x68, 0x95, 0x3F, 0xD3, 0x91, 0xC9, 0x05, 0x31, 0xA1, 0x31, 0x45, 0x53, 0x8F, 0xB1, 0x05, 0xAF,
        0x93, 0xE8, 0x17, 0x59, 0xE1, 0xF9, 0x63, 0x5B, 0x63, 0x94, 0x93, 0xB9, 0x95, 0x4F, 0xF6, 0x8C, 0x2B, 0xB2, 0x25, 0xB7,
        0x7C, 0xCF, 0xC5, 0xFD, 0x5E, 0x96, 0xA0, 0xFE, 0xE7, 0xC1, 0xC5, 0x0B, 0xF7, 0x7E, 0x2A, 0x45, 0xCD, 0xFF, 0x14, 0x72,
        0xD8, 0xE1, 0x4C, 0x2A, 0xCF, 0xA1, 0xD6, 0x30, 0x33, 0x65, 0x39, 0x0D, 0xFB, 0x94, 0x19, 0x37, 0x35, 0xDA, 0x3E, 0x66,
        0x7D, 0x6A, 0xD7, 0xBA, 0x33, 0x3B, 0xC3, 0x45, 0x57, 0x45, 0xCB, 0x36, 0xAE, 0xEF, 0xB4, 0x58, 0xC1, 0x75, 0x9F, 0x0B,
        0x89, 0x0C, 0xEB, 0x32, 0x8D, 0xB8, 0xDE, 0x3F, 0x7A, 0xF2, 0x59, 0xC0, 0xA6, 0x74, 0x8F, 0x50, 0xE9, 0xB3, 0xB4, 0xCF,
        0x7D, 0x16, 0xEF, 0xB7, 0x74, 0x64, 0x46, 0x84, 0x25, 0x56, 0x2D, 0xBA, 0x2E, 0x95, 0x73, 0xDA, 0x62, 0x8B, 0xED, 0x95,
        0x2A, 0xDD, 0x77, 0xFB, 0x66, 0xB7, 0x35, 0xB4, 0xCF, 0x39, 0xF0, 0xD1, 0xA0, 0xEE, 0xA6, 0xD3, 0x2E, 0xFB, 0x4B, 0x25,
        0xCA, 0x0F, 0xD6, 0xB4, 0xE6, 0x24, 0xA5, 0x96, 0x4D, 0x3B, 0xFC, 0xCF, 0x63, 0x3E, 0x4B, 0xC5, 0xCF, 0x0E, 0xB5, 0x8B,
        0x5D, 0x41, 0xEA, 0x5C, 0x7F, 0x7E, 0x73, 0xA8, 0xF8, 0xB6, 0x9C, 0x92, 0xAD, 0x7B, 0x97, 0xFD, 0x6D, 0x82, 0xF9, 0x9B,
        0x0D, 0x8D, 0x09, 0x7E, 0x9E, 0x41, 0x59, 0xFD, 0x87, 0x58, 0x82, 0x37, 0xC8, 0x4C, 0x95, 0xD9, 0x14, 0xCA, 0x76, 0x6C,
        0x72, 0x5E, 0x84, 0x7F, 0x48, 0x46, 0xE7, 0x8E, 0x6F, 0x8E, 0xB3, 0x98, 0xCA, 0x7F, 0xF5, 0x68, 0xDF, 0x98, 0x12, 0x6B,
        0x22, 0xC5, 0xCB, 0x27, 0x67, 0x9D, 0xB6, 0xE0, 0xB9, 0xCD, 0x02, 0xC1, 0x09, 0x72, 0x3B, 0x93, 0xA4, 0xEE, 0x6F, 0x9A,
        0xD2, 0xDC, 0xD0, 0xB3, 0xF4, 0x0C, 0x93, 0xEB, 0xB4, 0x6F, 0xC9, 0xF7, 0x32, 0xFE, 0x2F, 0x89, 0x90, 0x7C, 0x71, 0x6C,
        0xE3, 0xAA, 0x4D, 0xC7, 0x5F, 0xC9, 0x44, 0xAF, 0x60, 0xC9, 0x7D, 0x92, 0xF5, 0x77, 0x9E, 0x1F, 0xF7, 0xB5, 0x0D, 0xD3,
        0x7B, 0x26, 0x2C, 0x3C, 0xF0, 0x4E, 0xB3, 0xE8, 0x94, 0xFE, 0x22, 0x95, 0xCD, 0x01, 0x7F, 0xB6, 0xD6, 0x04, 0x99, 0xAA,
        0xA9, 0x19, 0xFB, 0x55, 0xAC, 0xFF, 0xE2, 0xB1, 0x52, 0x62, 0x9A, 0xE2, 0xAE, 0x24, 0xF1, 0x5B, 0x6B, 0xBA, 0x2B, 0x4A,
        0x6A, 0xA7, 0xEC, 0x7A, 0xAB, 0x57, 0x77, 0xDB, 0xED, 0x80, 0xFB, 0x9D, 0x06, 0xC3, 0x0F, 0xFB, 0x66, 0x36, 0x55, 0xD5,
        0x4F, 0xDA, 0xF2, 0x58, 0x21, 0xE7, 0xA8, 0xCE, 0x22, 0xD5, 0x6D, 0x61, 0xCC, 0xFB, 0x02, 0xF2, 0xB3, 0x53, 0x03, 0x12,
        0x8B, 0xB2, 0x15, 0x8A, 0x13, 0xCB, 0x52, 0x15, 0x46, 0x79, 0x30, 0x1E, 0x00, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0xE7, 0xB8, 0x9D, 0x6F, 0x06, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0F,
        0x00, 0x00, 0x00, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x62, 0x61, 0x6E, 0x6E, 0x65, 0x72, 0x2E, 0x62, 0x69, 0x6E, 0x62, 0x61,
        0x6E, 0x6E, 0x65, 0x72, 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x39, 0x9C,
        0xFB, 0x06, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x5F, 0x5F, 0x4D, 0x41, 0x43, 0x4F,
        0x53, 0x58, 0x2F, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x2E, 0x5F, 0x47, 0x57, 0x44, 0x6A, 0x75, 0x6E, 0x6B, 0x50, 0x4B, 0x01,
        0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0x19, 0x11, 0xB7, 0x04, 0x1A, 0x00, 0x00,
        0x00, 0xA0, 0x24, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x47, 0x57, 0x44, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00,
        0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x00, 0x82, 0x0D, 0x0C, 0x4A, 0x8B, 0x02, 0x00, 0x00, 0x70, 0x03, 0x00, 0x00, 0x08,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x40, 0x00, 0x00, 0x00, 0x64, 0x61, 0x74,
        0x61, 0x2F, 0x47, 0x44, 0x30, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21,
        0x00, 0xE7, 0xB8, 0x9D, 0x6F, 0x06, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0xF1, 0x02, 0x00, 0x00, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x62, 0x61, 0x6E, 0x6E,
        0x65, 0x72, 0x2E, 0x62, 0x69, 0x6E, 0x50, 0x4B, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x21, 0x00, 0x39, 0x9C, 0xFB, 0x06, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x24, 0x03, 0x00, 0x00, 0x5F, 0x5F, 0x4D, 0x41, 0x43, 0x4F, 0x53, 0x58,
        0x2F, 0x64, 0x61, 0x74, 0x61, 0x2F, 0x2E, 0x5F, 0x47, 0x57, 0x44, 0x50, 0x4B, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x04, 0x00, 0xEA, 0x00, 0x00, 0x00, 0x59, 0x03, 0x00, 0x00, 0x00, 0x00,
    };
    std::string gd0;
    for (int i = 0; i < 600; i++) gd0 += (char)((i * i * 7 + i / 3) & 0xFF);
    for (int i = 0; i < 20; i++) gd0 += "PokePark save ";
    WiiSaveFile in_zip[16];
    check(wiisave_is_zip(zip, sizeof(zip)), "Wii save", "zip not seen");
    check(wiisave_read_zip(zip, sizeof(zip), in_zip, 16, &n) == WIISAVE_OK && n == 3, "Wii save", "zip not read (%d files)", n);
    check(n == 3 && !strcmp(in_zip[0].name, "GWD") && std::string((const char*)in_zip[0].data, in_zip[0].size) == std::string(9376, 'w'),
          "Wii save", "zip's GWD");
    check(n == 3 && !strcmp(in_zip[1].name, "GD0") && std::string((const char*)in_zip[1].data, in_zip[1].size) == gd0, "Wii save",
          "zip's GD0");
    check(n == 3 && !strcmp(in_zip[2].name, "banner.bin") && std::string((const char*)in_zip[2].data, in_zip[2].size) == "banner",
          "Wii save", "zip's banner.bin");
    while (n > 0) free((void*)in_zip[--n].data);
    std::vector<uint8_t> broken(zip, zip + sizeof(zip));
    for (size_t i = 60; i < 200; i++) broken[i] ^= 0x55; // GWD's deflated data
    check(wiisave_read_zip(&broken[0], broken.size(), in_zip, 16, &n) != WIISAVE_OK && n == 0, "Wii save", "broken zip taken");
    check(wiisave_read_zip(&junk[0], junk.size(), in_zip, 16, &n) == WIISAVE_NOT_A_SAVE, "Wii save", "junk taken as a zip");

    // Into a NAND folder with a save already in it, which is kept.
    const char* tmp = getenv("TEMP") ? getenv("TEMP") : "/tmp";
    std::string root = std::string(tmp) + "/recomp_test_nand", title = root + "/title/00010000/52384145";
    WiiSaveFile old[2] = {{"GWD", (const uint8_t*)"old", 3}, {"GD1", (const uint8_t*)"one", 3}};
    wiisave_install(root.c_str(), old, 2); // (left from a run before or not)
    check(wiisave_install(root.c_str(), old, 2) == WIISAVE_OK, "Wii save", "old save not installed");
    check(wiisave_install(root.c_str(), got, 4) == WIISAVE_OK, "Wii save", "not installed");
    check(read_file(title + "/data/GWD") == files[0].data && read_file(title + "/data/banner.bin") == banner &&
              read_file(title + "/data/GD1") == "(none)",
          "Wii save", "installed files");
    check(read_file(title + "/data before import/GWD") == "old" && read_file(title + "/data before import/GD1") == "one",
          "Wii save", "old save not kept");
    WiiSaveFile bad[1] = {{"../GWD", (const uint8_t*)"x", 1}};
    check(wiisave_install(root.c_str(), bad, 1) == WIISAVE_NOT_A_SAVE, "Wii save", "a path taken as a name");

    // Taken out again: a zip of the installed save, which reads back the same.
    uint8_t* exported = NULL;
    size_t zip_size = 0;
    check(wiisave_export_zip(root.c_str(), 0x5A6E0000u, &exported, &zip_size) == WIISAVE_OK && exported, "Wii save", "not exported");
    if (exported) {
        check(wiisave_read_zip(exported, zip_size, in_zip, 16, &n) == WIISAVE_OK && n == 4, "Wii save", "export not read back (%d files)", n);
        for (int i = 0; i < n; i++) {
            int k;
            for (k = 0; k < 4 && strcmp(got[k].name, in_zip[i].name); k++) {}
            check(k < 4 && in_zip[i].size == got[k].size && !memcmp(in_zip[i].data, got[k].data, got[k].size), "Wii save",
                  "exported %s", in_zip[i].name);
        }
        while (n > 0) free((void*)in_zip[--n].data);
        free(exported);
    }
    check(wiisave_export_zip((root + "/none").c_str(), 0, &exported, &zip_size) == WIISAVE_NOT_A_SAVE, "Wii save",
          "nothing exported");
    // Deflated and read back: noise (stored), runs, repeats far apart and near.
    std::string noise(70000, 0), mixed;
    uint32_t seed = 1;
    for (char& c : noise) c = (char)((seed = seed * 1103515245u + 12345u) >> 24);
    for (int i = 0; i < 3000; i++) mixed += std::string(i % 300, (char)i) + noise.substr(i * 7, 20 + i % 50);
    mixed += noise.substr(0, 40000) + std::string(100000, 0) + noise.substr(0, 40000);
    WiiSaveFile packs[3] = {{"GWD", (const uint8_t*)mixed.data(), (uint32_t)mixed.size()},
                            {"GD0", (const uint8_t*)noise.data(), (uint32_t)noise.size()},
                            {"Pic0", (const uint8_t*)"", 0}};
    exported = wiisave_write_zip(packs, 3, 0, &zip_size);
    check(exported && zip_size < mixed.size() / 2 + noise.size() + 1000, "Wii save", "zip not deflated (%u bytes)", (unsigned)zip_size);
    check(exported && wiisave_read_zip(exported, zip_size, in_zip, 16, &n) == WIISAVE_OK && n == 3, "Wii save", "deflated zip not read");
    for (int i = 0; i < n; i++)
        check(in_zip[i].size == packs[i].size && !memcmp(in_zip[i].data, packs[i].data, packs[i].size), "Wii save",
              "deflated %s", packs[i].name);
    while (n > 0) free((void*)in_zip[--n].data);
    free(exported);
    printf("Wii save: a data.bin read and put in place\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <main.dol>\n", argv[0]);
        return 2;
    }
    if (ppc_mem_init() != 0 || ppc_load_dol(argv[1], NULL) != 0) {
        fprintf(stderr, "could not load %s\n", argv[1]);
        return 2;
    }
    // Let the game's own startup code set r1, r2 and r13.
    call(kInitRegisters);
    gReset = gCpu;
    gReset.r[1] = 0x80F00000;  // a stack clear of the arena used for test buffers
    printf("recompiled functions: %u; r2=%08X r13=%08X\n", ppc_func_count, gCpu.r[2], gCpu.r[13]);

    test_unicode_to_sjis();
    test_encode_codepoint();
    test_demangle();
    test_strings();
    test_sprintf();
    test_disc();
    test_disc_file();
    test_hook();
    test_math();
    test_sd();
    test_wii_save();

    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
