/* Runtime support for recompiled code: guest memory, DOL loading,
 * indirect calls, and the rarer instructions. */
#include <stdio.h>
#include <stdlib.h>

#include "ppc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#else
#include <sys/mman.h>
#include <time.h>
#endif

uint8_t* g_mem;

static void wgpipe_discard(uint32_t value, int size) { (void)value; (void)size; }
void (*g_wgpipe_write)(uint32_t value, int size) = wgpipe_discard;
void (*g_gx_flush)(void);
void (*g_gx_copy_sync)(uint32_t addr);
void (*g_mem_will_write)(uint32_t addr, uint32_t len);
uint64_t (*g_host_frame_due)(void);
void (*g_host_frame_render)(void);

#define GUEST_SPACE 0x40000000u

/* Physical ranges that are backed by memory: MEM1, the hardware
 * registers, MEM2, and the locked cache. */
static const struct {
    uint32_t base, size;
} kRegions[] = {
    {0x00000000u, 0x01800000u},
    {0x0C000000u, 0x02000000u}, /* GameCube-era registers and Hollywood's (0xCD800000) */
    {0x10000000u, 0x04000000u},
    {0x20000000u, 0x00004000u},
};

int ppc_mem_init(void) {
    size_t i;
#ifdef _WIN32
    /* Written pages are tracked (write watch) so the GPU code can tell
     * which textures may have changed (gx_fifo.c). */
    g_mem = (uint8_t*)VirtualAlloc(NULL, GUEST_SPACE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (!g_mem) return -1;
    for (i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); i++) {
        if (!VirtualAlloc(g_mem + kRegions[i].base, kRegions[i].size, MEM_COMMIT, PAGE_READWRITE)) return -1;
    }
#else
    g_mem = (uint8_t*)mmap(NULL, GUEST_SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) return -1;
    for (i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); i++) {
        if (mprotect(g_mem + kRegions[i].base, kRegions[i].size, PROT_READ | PROT_WRITE)) return -1;
    }
#endif
    return 0;
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int ppc_load_dol(const char* path, uint32_t* entry) {
    FILE* f = fopen(path, "rb");
    uint8_t hdr[0x100];
    int i;
    if (!f) return -1;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return -1; }
    /* The bss range in the header spans .sdata and .sdata2 too, so clear
     * it before the sections are copied in, not after. */
    memset(MA(be32(hdr + 0xD8)), 0, be32(hdr + 0xDC));
    for (i = 0; i < 18; i++) {
        uint32_t off = be32(hdr + 4 * i), addr = be32(hdr + 0x48 + 4 * i), size = be32(hdr + 0x90 + 4 * i);
        if (!size) continue;
        fseek(f, (long)off, SEEK_SET);
        if (fread(MA(addr), 1, size, f) != size) { fclose(f); return -1; }
    }
    if (entry) *entry = be32(hdr + 0xE0);
    fclose(f);
    return 0;
}

int ppc_load_dol_mem(const uint8_t* dol, size_t len, uint32_t* entry) {
    int i;
    if (len < 0x100) return -1;
    memset(MA(be32(dol + 0xD8)), 0, be32(dol + 0xDC)); /* as above */
    for (i = 0; i < 18; i++) {
        uint32_t off = be32(dol + 4 * i), addr = be32(dol + 0x48 + 4 * i), size = be32(dol + 0x90 + 4 * i);
        if (!size) continue;
        if ((uint64_t)off + size > len) return -1;
        memcpy(MA(addr), dol + off, size);
    }
    if (entry) *entry = be32(dol + 0xE0);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Indirect calls */

#define MAX_OVERRIDES 256
static struct {
    uint32_t addr;
    ppc_func fn;
} sOverrides[MAX_OVERRIDES];
static int sOverrideCount;

/* Indirect calls (bctrl, function pointers, virtual calls) look their
 * target up here. For main.dol's code, below 0x80400000, a table indexed
 * by address answers in one load; it is built on first use and follows
 * overrides. */
#define DIRECT_BASE 0x80000000u
#define DIRECT_COUNT 0x100000u /* 4 MB of code, one entry per word */
static ppc_func* sDirect;

static void build_direct(void) {
    unsigned i;
    sDirect = (ppc_func*)calloc(DIRECT_COUNT, sizeof(ppc_func));
    if (!sDirect) return;
    for (i = 0; i < ppc_func_count; i++) {
        uint32_t a = ppc_func_table[i].addr;
        if (a - DIRECT_BASE < DIRECT_COUNT * 4) sDirect[(a - DIRECT_BASE) >> 2] = ppc_func_table[i].fn;
    }
    for (i = 0; i < (unsigned)sOverrideCount; i++) {
        uint32_t a = sOverrides[i].addr;
        if (a - DIRECT_BASE < DIRECT_COUNT * 4) sDirect[(a - DIRECT_BASE) >> 2] = sOverrides[i].fn;
    }
}

void ppc_set_override(uint32_t addr, ppc_func fn) {
    int i;
    if (sDirect && addr - DIRECT_BASE < DIRECT_COUNT * 4) sDirect[(addr - DIRECT_BASE) >> 2] = fn;
    for (i = 0; i < sOverrideCount; i++) {
        if (sOverrides[i].addr == addr) { sOverrides[i].fn = fn; return; }
    }
    if (sOverrideCount < MAX_OVERRIDES) {
        sOverrides[sOverrideCount].addr = addr;
        sOverrides[sOverrideCount].fn = fn;
        sOverrideCount++;
    }
}

ppc_func ppc_lookup(uint32_t addr) {
    unsigned lo = 0, hi = ppc_func_count;
    int i;
    if (!sDirect) build_direct();
    if (sDirect && addr - DIRECT_BASE < DIRECT_COUNT * 4 && !(addr & 3)) return sDirect[(addr - DIRECT_BASE) >> 2];
    for (i = 0; i < sOverrideCount; i++) {
        if (sOverrides[i].addr == addr) return sOverrides[i].fn;
    }
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ppc_func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ppc_func_count && ppc_func_table[lo].addr == addr) return ppc_func_table[lo].fn;
    return NULL;
}

void ppc_icall(ppc_t* c, uint32_t addr) {
    ppc_func fn = ppc_lookup(addr);
    if (!fn) ppc_unresolved(c, addr);
    else fn(c);
}

uint32_t g_trace[256];
unsigned g_trace_pos;
uint32_t g_calls[0x100000];
int g_count_calls;

/* Name of the function containing addr, for diagnostics. */
const char* ppc_func_name(uint32_t addr) {
    unsigned lo = 0, hi = ppc_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ppc_func_table[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo ? ppc_func_table[lo - 1].name : "?";
}

void ppc_dump_trace(void) {
#ifdef RECOMP_TRACE
    /* Snapshot the position: the watchdog dumps while the game runs. */
    unsigned end = g_trace_pos;
    unsigned i, n = end < 200 ? end : 200;
    fprintf(stderr, "last %u functions entered (oldest first):\n", n);
    for (i = end - n; i != end; i++) {
        uint32_t a = g_trace[i & 255];
        fprintf(stderr, "  %08X %s\n", a, ppc_func_name(a));
    }
#endif
}

/* Walks the guest stack's back chain. Each frame starts with a pointer to
 * the caller's frame, and a function saves its return address 4 bytes
 * into its caller's frame. */
void ppc_backtrace(ppc_t* c) {
    uint32_t sp = c->r[1];
    int depth;
    fprintf(stderr, "guest backtrace (lr %08X %s):\n", c->lr, ppc_func_name(c->lr));
    for (depth = 0; depth < 24; depth++) {
        uint32_t next, ra;
        if (sp < 0x80000000u || sp >= 0x81800000u) break;
        next = LD32(sp);
        if (next < 0x80000000u || next >= 0x81800000u || next <= sp) break;
        ra = LD32(next + 4);
        fprintf(stderr, "  %08X %s\n", ra, ppc_func_name(ra));
        sp = next;
    }
}

static void fatal(const char* what, uint32_t addr, uint32_t extra) {
    fprintf(stderr, "recomp: %s at %08X [%s] (%08X)\n", what, addr, ppc_func_name(addr), extra);
    ppc_dump_trace();
    exit(1);
}

void ppc_unresolved(ppc_t* c, uint32_t addr) { fatal("call to unknown address", addr, c->lr); }
void ppc_unimpl(ppc_t* c, uint32_t addr, uint32_t ins) { (void)c; fatal("untranslated instruction", addr, ins); }
void ppc_trap(ppc_t* c, uint32_t addr) { fatal("trap", addr, c->lr); }
/* The only system call the RVL OS installs (__OSSystemCallVector) syncs
 * the instruction pipeline and caches, which needs nothing here. */
void ppc_syscall(ppc_t* c) { (void)c; }
/* rfi continues at SRR0 with MSR = SRR1. The SDK uses it to enter real
 * mode (RealMode -> BATConfig) and to come back from it; returning to
 * the caller's return address is treated as a plain return. Thread
 * switches (OSLoadContext) are handled by hooks, not here. */
void ppc_rfi(ppc_t* c) {
    uint32_t target = c->spr[26];
    ppc_func fn;
    c->msr = c->spr[27];
    if (target < 0x01800000u) target |= 0x80000000u; /* physical -> cached virtual */
    if (target == c->lr) return;
    fn = ppc_lookup(target);
    if (!fn) fatal("rfi to unknown address", target, c->lr);
    fn(c);
}

/* ------------------------------------------------------------------------ */
/* Special-purpose registers */

/* Decrementer: counts down at the time-base rate; the exception is taken
 * when it passes zero. Only the deadline is tracked. */
static uint64_t sDecDeadline;
static int sDecArmed;

int ppc_dec_deadline(uint64_t* deadline) {
    if (!sDecArmed) return 0;
    *deadline = sDecDeadline;
    return 1;
}

void ppc_dec_disarm(void) { sDecArmed = 0; }

uint32_t ppc_mfspr(ppc_t* c, int spr) {
    if (spr == 22) return sDecArmed ? (uint32_t)(sDecDeadline - ppc_timebase()) : 0xFFFFFFFFu;
    if (spr == 1) return (c->xer_so << 31) | (c->xer_ca << 29) | (c->xer_low & 0x7F);
    if (spr >= 912 && spr <= 919) return c->gqr[spr - 912];
    return c->spr[spr];
}

void ppc_mtspr(ppc_t* c, int spr, uint32_t v) {
    if (spr == 1) {
        c->xer_so = v >> 31;
        c->xer_ca = (v >> 29) & 1;
        c->xer_low = v & 0x7F;
    } else if (spr >= 912 && spr <= 919) {
        c->gqr[spr - 912] = v;
    } else if (spr == 22) {
        /* A negative value means the exception is already pending. */
        sDecDeadline = ppc_timebase() + (uint64_t)(int64_t)(int32_t)v;
        sDecArmed = 1;
    } else if (spr == 923 && (v & 2)) {
        /* DMAL with the trigger bit: a locked-cache DMA. DMAU holds the
         * memory address and the high bits of the length; DMAL the
         * locked-cache address, the direction (bit 4: 1 = memory to
         * cache), and the low bits of the length, in 32-byte blocks
         * (0 = 128). The transfer completes at once. */
        uint32_t dmau = c->spr[922];
        uint32_t blocks = ((dmau & 0x1F) << 2) | ((v >> 2) & 3);
        uint32_t mem = dmau & ~0x1Fu, lc = v & ~0x1Fu;
        if (!blocks) blocks = 128;
        if (v & 0x10) memmove(MA(lc), MA(mem), blocks * 32);
        else memmove(MA(mem), MA(lc), blocks * 32);
        c->spr[923] = v & ~2u;
    } else {
        c->spr[spr] = v;
    }
}

/* The time base runs at a quarter of the 243 MHz bus clock. */
/* The time base follows the host clock. It is read very often (every
 * check for a due interrupt, which the game makes each time it enables
 * them), so where the CPU's timestamp counter runs at a fixed rate it is
 * read from that (a few nanoseconds), at a rate measured against the
 * performance counter over the first second; until then, and on CPUs
 * without one, from the performance counter itself. Only the game's
 * thread reads it. */
#ifdef _WIN32
static int invariant_tsc(void) {
    int r[4];
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] < 0x80000007u) return 0;
    __cpuid(r, 0x80000007);
    return (r[3] >> 8) & 1;
}
#endif

#ifndef _WIN32
static uint64_t sPausedTicks;
#endif

uint64_t ppc_timebase(void) {
    uint64_t ticks;
#ifdef _WIN32
    enum { UNSET, COUNTER, MEASURING, TSC };
    static int mode;
    static double scale, perTsc;
    static uint64_t tsc0, tb0;
    static LONGLONG qpc0;
    LARGE_INTEGER now;
    uint64_t tsc;
    if (mode == TSC) return tb0 + (uint64_t)((double)(__rdtsc() - tsc0) * perTsc);
    if (mode == UNSET) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        scale = 60750000.0 / (double)freq.QuadPart;
        mode = invariant_tsc() ? MEASURING : COUNTER;
        QueryPerformanceCounter(&now);
        tsc0 = __rdtsc();
        qpc0 = now.QuadPart;
    }
    QueryPerformanceCounter(&now);
    tsc = __rdtsc();
    ticks = (uint64_t)((double)now.QuadPart * scale);
    if (mode == MEASURING && (double)(now.QuadPart - qpc0) * scale >= 60750000.0 && tsc > tsc0) {
        /* Carries on from this reading, so the time base never steps. */
        perTsc = (double)(now.QuadPart - qpc0) * scale / (double)(tsc - tsc0);
        tsc0 = tsc;
        tb0 = ticks;
        mode = TSC;
    }
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ticks = (uint64_t)ts.tv_sec * 60750000ull + (uint64_t)ts.tv_nsec * 60750000ull / 1000000000ull - sPausedTicks;
#endif
    return ticks;
}

#ifndef _WIN32
/* Stops and restarts the time base, so time spent away (an Android app
 * in the background) isn't caught up on when the game carries on. */
void ppc_timebase_pause(int paused) {
    static uint64_t since;
    if (paused && !since) {
        since = ppc_timebase() + 1;
    } else if (!paused && since) {
        sPausedTicks += ppc_timebase() + 1 - since;
        since = 0;
    }
}
#endif

uint32_t ppc_mftb(ppc_t* c, int tbr) {
    uint64_t ticks = ppc_timebase();
    (void)c;
    return tbr == 269 ? (uint32_t)(ticks >> 32) : (uint32_t)ticks;
}

double ppc_fctiw(ppc_t* c, double v) {
    double r;
    int32_t i;
    switch (c->fpscr & 3) {
    case 0: r = nearbyint(v); break; /* host default is round-to-nearest-even */
    case 1: r = trunc(v); break;
    case 2: r = ceil(v); break;
    default: r = floor(v); break;
    }
    if (r != r) i = (int32_t)0x80000000;
    else if (r >= 2147483647.0) i = 0x7FFFFFFF;
    else if (r <= -2147483648.0) i = (int32_t)0x80000000;
    else i = (int32_t)r;
    return bits_f64(0xFFF8000000000000ull | (uint32_t)i);
}
