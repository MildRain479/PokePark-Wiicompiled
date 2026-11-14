/* Boot harness: loads main.dol, sets up low memory the way the Wii's
 * system menu and apploader leave it, and runs the game's __start.
 *
 * Usage: boot [<main.dol> [time limit, seconds]]: without arguments, the
 * game in an R8AE01 folder beside the program or above it, no time limit.
 *
 * The disc is read from a "disc" directory next to the directory holding
 * main.dol (orig/R8AE01/disc for orig/R8AE01/sys/main.dol), holding sys/
 * and files/ as extracted by dtk (see recomp/README.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"
#include "recomp_funcs.h"

int hle_dvd_init(const char* root);
void hle_dvd_boot_setup(void);
void hle_text_boot_setup(void);
void hle_nand_set_root(const char* root);
void hle_dump_threads(void);
void input_init(void);
void audio_init(void);
void font_init(void);
void sdcard_init(void);
void settings_load(void);
void update_check(void); /* update.c */
void hle_game_init(void);

static ppc_t* gCpu;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* RECOMP_WATCH: the watched guest range and its write-protected page. */
static uint32_t gWatchPhys, gWatchLen;
static uint8_t* gWatchPage;
static DWORD gGameThreadId;

/* Prints the faulting code's host call stack, with symbol names, walked
 * from the context the exception was raised in. */
static void host_backtrace(const CONTEXT* fault) {
    CONTEXT ctx = *fault;
    STACKFRAME64 frame;
    HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
    int depth;
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 128];
    } sym;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    fprintf(stderr, "host backtrace:\n");
    for (depth = 0; depth < 40; depth++) {
        DWORD64 disp = 0;
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL) || !frame.AddrPC.Offset)
            break;
        memset(&sym, 0, sizeof(sym));
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = 127;
        {
            /* The module and offset too: in a driver without symbols the
             * name is only the nearest export, often far away. */
            HMODULE mod = NULL;
            char path[MAX_PATH] = "?";
            const char* base;
            DWORD64 pc = frame.AddrPC.Offset;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)pc, &mod))
                GetModuleFileNameA(mod, path, sizeof(path));
            base = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
            if (SymFromAddr(proc, pc, &disp, &sym.info))
                fprintf(stderr, "  %016llX %s+0x%llx (%s+0x%llx)\n", (unsigned long long)pc, sym.info.Name,
                        (unsigned long long)disp, base, (unsigned long long)(pc - (DWORD64)mod));
            else
                fprintf(stderr, "  %016llX ? (%s+0x%llx)\n", (unsigned long long)pc, base,
                        (unsigned long long)(pc - (DWORD64)mod));
        }
    }
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* info) {
    EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && gWatchPage) {
        /* Finished a write elsewhere on the watched page: protect it again. */
        DWORD old;
        VirtualProtect(gWatchPage, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t a = (uintptr_t)r->ExceptionInformation[1];
        if (gWatchPage && (uint8_t*)a >= gWatchPage && (uint8_t*)a < gWatchPage + 0x1000 &&
            (uint32_t)(a - (uintptr_t)g_mem - gWatchPhys) >= gWatchLen) {
            /* Not the watched range: let this one write through, then
             * single-step and re-protect. */
            DWORD old;
            VirtualProtect(gWatchPage, 0x1000, PAGE_READWRITE, &old);
            info->ContextRecord->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (gWatchPage && (uint8_t*)a >= gWatchPage && (uint8_t*)a < gWatchPage + 0x1000 && getenv("RECOMP_WATCH_LOG")) {
            /* RECOMP_WATCH_LOG=1: log who writes the range (the guest
             * function entered last), once per change of writer, and let
             * the write through. */
            static uint32_t lastWriter;
            uint32_t writer = g_trace[(g_trace_pos - 1) & 255];
            DWORD old;
            if (writer != lastWriter) {
                fprintf(stderr, "watch: %08X written by %08X %s (lr %08X %s)\n", (unsigned)(a - (uintptr_t)g_mem),
                        writer, ppc_func_name(writer), gCpu ? gCpu->lr : 0, gCpu ? ppc_func_name(gCpu->lr) : "");
                lastWriter = writer;
            }
            VirtualProtect(gWatchPage, 0x1000, PAGE_READWRITE, &old);
            info->ContextRecord->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        /* Faults elsewhere go to the faulting code's own handlers first:
         * the graphics driver probes memory and catches the faults itself
         * (NVIDIA's OpenGL driver does, on its own threads), and this
         * handler sees them before it does. Only one nobody handles ends
         * the run (on_unhandled). */
        if (a < (uintptr_t)g_mem || a >= (uintptr_t)g_mem + 0x40000000u) return EXCEPTION_CONTINUE_SEARCH;
        fprintf(stderr, "boot: guest memory fault at physical %08X (%s)\n", (unsigned)(a - (uintptr_t)g_mem),
                r->ExceptionInformation[0] ? "write" : "read");
        if (gCpu) ppc_backtrace(gCpu);
        ppc_dump_trace();
        fflush(stderr);
        ExitProcess(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    EXCEPTION_RECORD* r = info->ExceptionRecord;
    const char* thread = GetCurrentThreadId() == gGameThreadId ? "game" : "another";
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, "boot: host memory fault at %p (%s), on the %s thread\n", (void*)r->ExceptionInformation[1],
                r->ExceptionInformation[0] ? "write" : "read", thread);
    else
        fprintf(stderr, "boot: host exception %08lX at %p, on the %s thread\n", (unsigned long)r->ExceptionCode,
                r->ExceptionAddress, thread);
    host_backtrace(info->ContextRecord);
    ppc_dump_trace();
    fflush(stderr);
    if (getenv("RECOMP_MINIDUMP")) {
        /* RECOMP_MINIDUMP=<file>: a minidump of the process, for a
         * debugger (with all memory if RECOMP_MINIDUMP_FULL=1). */
        HANDLE f = CreateFileA(getenv("RECOMP_MINIDUMP"), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        MINIDUMP_EXCEPTION_INFORMATION e;
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                             MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                                             (getenv("RECOMP_MINIDUMP_FULL") ? MiniDumpWithFullMemory : 0));
        e.ThreadId = GetCurrentThreadId();
        e.ExceptionPointers = info;
        e.ClientPointers = FALSE;
        if (f != INVALID_HANDLE_VALUE) {
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &e, NULL, NULL);
            CloseHandle(f);
            fprintf(stderr, "boot: wrote %s\n", getenv("RECOMP_MINIDUMP"));
        }
    }
    ExitProcess(3);
}

/* Stops a boot that hangs (usually polling hardware that isn't emulated)
 * and shows where it was. */
static int by_calls(const void* a, const void* b) {
    uint32_t x = g_calls[*(const uint32_t*)a], y = g_calls[*(const uint32_t*)b];
    return x < y ? 1 : x > y ? -1 : 0;
}

/* RECOMP_CALLS=<n>: the n functions called most in the run's last 5 s. */
static void dump_calls(int n) {
    static uint32_t idx[0x100000];
    uint32_t i, used = 0;
    for (i = 0; i < 0x100000; i++) {
        if (g_calls[i]) idx[used++] = i;
    }
    qsort(idx, used, sizeof(idx[0]), by_calls);
    fprintf(stderr, "most called in the last 5 s:\n");
    for (i = 0; i < used && (int)i < n; i++) {
        uint32_t a = 0x80000000u | idx[i] << 2;
        fprintf(stderr, "  %8u  %08X %s\n", g_calls[idx[i]], a, ppc_func_name(a));
    }
}

/* RECOMP_PROFILE=<seconds>: from that many seconds in until the watchdog
 * stops the run, samples where the game's host thread is about once a
 * millisecond, then lists the host functions it was in most (recompiled
 * guest functions are f_<address>). */
#pragma comment(lib, "winmm.lib")

#define MAX_SAMPLES 400000
static HANDLE gGameThread;
extern volatile long g_in_gl; /* gx_frames.c */
#define SAMPLE_IN_GL 1 /* a sample taken while the game thread was inside OpenGL */
static DWORD64 gSamples[MAX_SAMPLES];
static volatile LONG gSampleCount;

static DWORD WINAPI profiler(LPVOID arg) {
    CONTEXT ctx;
    Sleep((DWORD)(uintptr_t)arg * 1000);
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        if (SuspendThread(gGameThread) == (DWORD)-1) continue;
        if (g_in_gl) {
            /* Inside OpenGL: let it go at once (holding a thread
             * suspended in the driver can crash it) and count the sample
             * as graphics driver time. */
            ResumeThread(gGameThread);
            if (gSampleCount < MAX_SAMPLES) gSamples[gSampleCount++] = SAMPLE_IN_GL;
            continue;
        }
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(gGameThread, &ctx) && gSampleCount < MAX_SAMPLES) gSamples[gSampleCount++] = ctx.Rip;
        ResumeThread(gGameThread);
    }
    return 0;
}

typedef struct {
    char name[96];
    unsigned count;
} ProfileEntry;

static int by_count(const void* a, const void* b) {
    unsigned x = ((const ProfileEntry*)a)->count, y = ((const ProfileEntry*)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void dump_profile(void) {
    static ProfileEntry entries[4096];
    int used = 0, i, j, n = (int)gSampleCount;
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 128];
    } sym;
    SuspendThread(gGameThread);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    for (i = 0; i < n; i++) {
        DWORD64 disp;
        const char* name = "?";
        memset(&sym, 0, sizeof(sym));
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = 127;
        if (gSamples[i] == SAMPLE_IN_GL) name = "gl (inside OpenGL)";
        else if (SymFromAddr(GetCurrentProcess(), gSamples[i], &disp, &sym.info)) name = sym.info.Name;
        for (j = 0; j < used && strcmp(entries[j].name, name); j++) {}
        if (j == used) {
            if (used == 4096) continue;
            strncpy(entries[used].name, name, sizeof(entries[used].name) - 1);
            entries[used++].count = 0;
        }
        entries[j].count++;
    }
    qsort(entries, used, sizeof(entries[0]), by_count);
    fprintf(stderr, "profile: %d samples\n", n);
    {
        /* Totals by kind: recompiled game code, graphics, sound, waiting,
         * and the rest (runtime helpers, HLE, the OS and drivers). */
        static const struct { const char* kind; const char* names; } kKinds[] = {
            {"game code", "f_"},
            {"graphics", "run read_vertex read_comp read_color transform_vertex channel_color build_state draw gxb_ gx_ "
                         "texture_for_map hash_bytes decode_ tex_ gl"},
            {"sound", "mix_voice read_sample dsp_run_voices ai_frame push"},
            {"waiting", "NtWaitForSingleObject ZwDelayExecution WaitForSingleObject RtlDelayExecution Sleep "
                        "RtlQueryPerformanceCounter QueryPerformanceCounter ppc_timebase sleep_until"},
        };
        double share[5] = {0};
        int k;
        for (i = 0; i < used; i++) {
            int kind = 4;
            for (k = 0; k < 4 && kind == 4; k++) {
                const char* p = kKinds[k].names;
                while (*p) {
                    size_t len = strcspn(p, " ");
                    if (!strncmp(entries[i].name, p, len) && (p[len - 1] == '_' || p[0] == 'g' && p[1] == 'l' && len == 2 ||
                                                              !entries[i].name[len] || entries[i].name[len] == '+')) {
                        kind = k;
                        break;
                    }
                    p += len;
                    while (*p == ' ') p++;
                }
            }
            share[kind] += 100.0 * entries[i].count / (n ? n : 1);
        }
        fprintf(stderr, "  by kind: game code %.1f%%, graphics %.1f%%, sound %.1f%%, waiting %.1f%%, other %.1f%%\n",
                share[0], share[1], share[2], share[3], share[4]);
    }
    for (i = 0; i < used && i < 60; i++) {
        uint32_t guest = 0;
        sscanf(entries[i].name, "f_%x", &guest);
        fprintf(stderr, "  %5.1f%%  %-40s %s\n", 100.0 * entries[i].count / (n ? n : 1), entries[i].name,
                guest ? ppc_func_name(guest) : "");
    }
}

static DWORD WINAPI watchdog(LPVOID arg) {
    DWORD secs = (DWORD)(uintptr_t)arg;
    const char* calls = getenv("RECOMP_CALLS");
    if (calls && secs > 5) {
        Sleep((secs - 5) * 1000);
        memset(g_calls, 0, sizeof(g_calls));
        g_count_calls = 1;
        Sleep(5000);
        dump_calls(atoi(calls));
    } else {
        Sleep(secs * 1000);
    }
    if (gSampleCount) dump_profile();
    fprintf(stderr, "boot: still running after %u s; stopping\n", (unsigned)secs);
    ppc_dump_trace();
    if (gCpu) ppc_backtrace(gCpu);
    hle_dump_threads();
    {
        /* RECOMP_DUMP_MEM=<file>: save MEM1 (24 MB) then MEM2 (64 MB), as
         * the guest sees them, for offline inspection. */
        const char* path = getenv("RECOMP_DUMP_MEM");
        FILE* f = path ? fopen(path, "wb") : NULL;
        if (f) {
            fwrite(MA(0x80000000), 1, 0x01800000, f);
            fwrite(MA(0x90000000), 1, 0x04000000, f);
            fclose(f);
            fprintf(stderr, "boot: guest memory saved to %s\n", path);
        }
    }
    {
        /* RECOMP_PEEK=<address>,<length>: dump guest memory on timeout. */
        const char* p = getenv("RECOMP_PEEK");
        if (p) {
            char* end;
            uint32_t addr = (uint32_t)strtoul(p, &end, 16), len = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 16) : 0x40, i;
            for (i = 0; i < len; i += 4) {
                if (i % 16 == 0) fprintf(stderr, "\n  %08X:", addr + i);
                fprintf(stderr, " %08X", LD32(addr + i));
            }
            fputc(10, stderr);
        }
    }
    fflush(stderr);
    ExitProcess(4);
    return 0;
}
#endif

/* What the system menu leaves in low memory before jumping to the game,
 * following Dolphin's HLE boot (Core/Boot/Boot_BS2Emu.cpp). */
static void setup_low_memory(void) {
    static const char kDiscId[] = "R8AE01";
    memcpy(MA(0x80000000), kDiscId, 6);
    ST32(0x80000018, 0x5D1C9EA3); /* Wii disc magic */
    ST32(0x80000020, 0x0D15EA5E); /* boot magic */
    ST32(0x80000024, 0x00000001);
    ST32(0x80000028, 0x01800000); /* MEM1 size */
    ST32(0x8000002C, 0x00000023); /* console type: retail Wii */
    ST32(0x80000030, 0x00000000); /* arena lo: set by the OS */
    ST32(0x80000034, 0x817FEC60); /* arena hi */
    ST32(0x800000CC, 0x00000000); /* video mode: NTSC */
    ST32(0x800000F0, 0x01800000); /* simulated memory size */
    ST32(0x800000F8, 0x0E7BE2C0); /* bus clock, 243 MHz */
    ST32(0x800000FC, 0x2B73A840); /* CPU clock, 729 MHz */
    ST32(0x800030D8, 0x00000000); /* system time */
    ST16(0x800030E6, 0x0000);     /* retail console (bit 15 would mean a development kit) */
    ST32(0x80003100, 0x01800000); /* MEM1 physical size */
    ST32(0x80003104, 0x01800000); /* MEM1 simulated size */
    ST32(0x80003108, 0x817FEC60); /* end of usable MEM1 */
    ST32(0x8000310C, 0x00000000);
    ST32(0x80003110, 0x817FEC60); /* MEM1 arena end */
    ST32(0x80003118, 0x04000000); /* MEM2 physical size */
    ST32(0x8000311C, 0x04000000); /* MEM2 simulated size */
    /* On a Wii, IOS keeps the top of MEM2 (0x933E0000 up). Nothing runs on
     * the Starlet here, so the game gets that memory too, and the IPC
     * buffer moves to the very top. */
    ST32(0x80003120, 0x94000000); /* end of MEM2 */
    ST32(0x80003124, 0x90000800); /* usable MEM2 start */
    ST32(0x80003128, 0x93FE0000); /* usable MEM2 end */
    ST32(0x80003130, 0x93FE0000); /* IOS IPC buffer start */
    ST32(0x80003134, 0x94000000); /* IOS IPC buffer end */
    ST32(0x80003138, 0x00000011); /* Hollywood revision */
    ST32(0x80003140, 0x00090204); /* IOS version: IOS9 v516 */
    ST32(0x80003144, 0x00062507); /* IOS build date */
    ST32(0x80003158, 0x0000FF16); /* DDR vendor */
    ST32(0x80003188, 0x00090204); /* IOS version the apploader saw */
}

/* The game's main.dol, in an R8AE01 folder (or orig\R8AE01, as in the
 * source tree) beside the program or in a folder above it. */
int import_game(char* mainDol, size_t n); /* import_win32.c */

static int find_game(char* out, size_t n) {
#ifdef _WIN32
    char dir[1024], *slash;
    static const char* const kPlaces[] = {"R8AE01\\sys\\main.dol", "orig\\R8AE01\\sys\\main.dol"};
    size_t i;
    if (!GetModuleFileNameA(NULL, dir, sizeof(dir) - 64)) return 0;
    while ((slash = strrchr(dir, '\\')) != NULL) {
        *slash = '\0';
        for (i = 0; i < sizeof(kPlaces) / sizeof(kPlaces[0]); i++) {
            snprintf(out, n, "%s\\%s", dir, kPlaces[i]);
            if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
#endif
    (void)out;
    (void)n;
    return 0;
}

#ifdef _WIN32
/* For a launcher such as RecompLauncher, which has the player's disc image
 * already: "--game <disc image>" sets the game up from it, with no window,
 * if it isn't set up yet, and "--prepare" does only that, printing
 * "progress <0-1> <text>" lines, and exits (0 if it worked). Without them
 * nothing here runs. They are taken out of argv, so the rest of main sees
 * what it always did. */
int import_image(const char* image, int progress, char* mainDol, size_t n, char* why, size_t whyN); /* import_win32.c */

static void launcher_args(int* argc, char** argv, const char** image, int* prepare) {
    int i, kept = 1;
    for (i = 1; i < *argc; i++) {
        if (!strcmp(argv[i], "--prepare")) {
            *prepare = 1;
        } else if (!strcmp(argv[i], "--game") && i + 1 < *argc) {
            *image = argv[++i];
        } else {
            argv[kept++] = argv[i];
        }
    }
    argv[kept] = NULL;
    *argc = kept;
}

static int set_up_from_image(const char* image, int prepare, char* mainDol, size_t n) {
    char why[512];
    if (!image) {
        fprintf(stderr, "--prepare needs --game <disc image>\n");
        return 0;
    }
    if (import_image(image, prepare, mainDol, n, why, sizeof(why))) return 1;
    fprintf(stderr, "%s\n", why);
    if (!prepare) MessageBoxA(NULL, why, "PokePark", MB_ICONERROR);
    return 0;
}
#endif

#ifdef __ANDROID__
int disc_image_active(void); /* disc_image_android.c */
const uint8_t* disc_image_dol(size_t* len);
int disc_image_dvd_init(void);
#define main boot_main /* started by the Android layer (window_android.c), on a thread of its own */

/* RECOMP_PROFILE=<seconds>: from that many seconds in, for 20 seconds,
 * samples where the game's thread is about once a millisecond (a timer
 * signal to that thread), then logs the functions it was in most
 * (recompiled guest functions are f_<address>). */
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define MAX_SAMPLES 100000
#define DEPTH 8 /* callers kept per sample */
static uintptr_t gSamples[MAX_SAMPLES], gCallers[MAX_SAMPLES][DEPTH];
static uintptr_t gStackLo, gStackHi; /* the game thread's stack */
static volatile int gSampleCount;
static timer_t gProfileTimer;

static void on_sample(int sig, siginfo_t* si, void* uc) {
    int n = gSampleCount;
    (void)sig;
    (void)si;
    if (n < MAX_SAMPLES) {
        gSamples[n] = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.pc;
        /* The link register (right for a leaf function such as memcpy),
         * then the frame records the frame pointer chains together. */
        uintptr_t fp = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.regs[29];
        uintptr_t sp = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.sp, lo = sp;
        int d;
        /* On the thread's stack, or on a guest thread's fiber (8 MB,
         * hle_thread.c): frames only go up from the stack pointer. */
        uintptr_t hi = sp >= gStackLo && sp < gStackHi ? gStackHi : sp + (8u << 20) - 4096;
        gCallers[n][0] = (uintptr_t)((ucontext_t*)uc)->uc_mcontext.regs[30];
        for (d = 1; d < DEPTH; d++) {
            if (fp < lo || fp + 16 > hi || (fp & 7)) break;
            lo = fp + 16;
            gCallers[n][d] = ((uintptr_t*)fp)[1];
            fp = ((uintptr_t*)fp)[0];
        }
        for (; d < DEPTH; d++) gCallers[n][d] = 0;
        gSampleCount = n + 1;
    }
}

typedef struct {
    const char* name;
    unsigned count;
} ProfileEntry;

static int by_count(const void* a, const void* b) {
    unsigned x = ((const ProfileEntry*)a)->count, y = ((const ProfileEntry*)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void* profiler(void* arg) {
    static ProfileEntry e[4096];
    struct itimerspec on = {{0, 1000000}, {0, 1000000}}, off = {{0, 0}, {0, 0}};
    int n = 0, i, k, total;
    sleep((unsigned)(uintptr_t)arg);
    fprintf(stderr, "profile: sampling for 20 s\n");
    timer_settime(gProfileTimer, 0, &on, NULL);
    sleep(20);
    timer_settime(gProfileTimer, 0, &off, NULL);
    total = gSampleCount;
    {
        /* Every sample and its caller (the link register, right for a
         * leaf function such as memcpy), as library and offset, for
         * llvm-symbolizer: profile.txt beside the saves. */
        char path[1100];
        FILE* f;
        snprintf(path, sizeof(path), "%s/profile.txt", getenv("RECOMP_SHARED") ? getenv("RECOMP_SHARED") : ".");
        if ((f = fopen(path, "w"))) {
            for (i = 0; i < total; i++) {
                for (k = 0; k <= DEPTH; k++) {
                    uintptr_t a = k ? gCallers[i][k - 1] : gSamples[i];
                    Dl_info info;
                    const char* end = k == DEPTH ? "\n" : " ";
                    if (a && dladdr((void*)a, &info) && info.dli_fname)
                        fprintf(f, "%s 0x%lx%s", strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                                (unsigned long)(a - (uintptr_t)info.dli_fbase), end);
                    else
                        fprintf(f, "? 0x%lx%s", (unsigned long)a, end);
                }
            }
            fclose(f);
        }
    }
    for (i = 0; i < total; i++) {
        Dl_info info;
        const char* name = "?";
        if (dladdr((void*)gSamples[i], &info)) {
            if (info.dli_sname) name = info.dli_sname;
            else if (info.dli_fname) name = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
        }
        for (k = 0; k < n && strcmp(e[k].name, name); k++) {}
        if (k == n && n < 4096) e[n++] = (ProfileEntry){name, 0};
        if (k < n) e[k].count++;
    }
    qsort(e, n, sizeof(e[0]), by_count);
    fprintf(stderr, "profile: %d samples\n", total);
    for (i = 0; i < n && i < 60; i++)
        fprintf(stderr, "profile: %5.1f%% %s\n", 100.0 * e[i].count / (total ? total : 1), e[i].name);
    return NULL;
}

/* On the game's thread. */
static void start_profiler(int after) {
    struct sigaction sa;
    struct sigevent ev;
    pthread_t t;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev.sigev_notify_thread_id = gettid();
    {
        pthread_attr_t attr;
        void* lo;
        size_t size;
        if (pthread_getattr_np(pthread_self(), &attr) == 0 && pthread_attr_getstack(&attr, &lo, &size) == 0) {
            gStackLo = (uintptr_t)lo;
            gStackHi = (uintptr_t)lo + size;
        }
    }
    if (timer_create(CLOCK_MONOTONIC, &ev, &gProfileTimer) != 0) return;
    pthread_create(&t, NULL, profiler, (void*)(uintptr_t)after);
    pthread_detach(t);
}
#endif

int main(int argc, char** argv) {
    ppc_t* c;
    uint32_t entry;
    static char found[1024];
    static char* defaultArgs[2];
#ifdef _WIN32
    const char* image = NULL;
    int prepare = 0;
    launcher_args(&argc, argv, &image, &prepare);
    if (prepare) return set_up_from_image(image, 1, found, sizeof(found)) ? 0 : 1;
    if (image && !find_game(found, sizeof(found)) && !set_up_from_image(image, 0, found, sizeof(found))) return 2;
    /* Not set up yet: the player's disc image, extracted beside the
     * program (import_win32.c). */
    if (argc < 2 && !find_game(found, sizeof(found)) && !import_game(found, sizeof(found))) return 0;
#endif
    if (argc < 2 && (found[0] || find_game(found, sizeof(found)))) {
        /* Started without arguments (double-clicked): the game found
         * beside the program or above it, and no time limit. */
        defaultArgs[0] = argv[0];
        defaultArgs[1] = found;
        argv = defaultArgs;
        argc = 2;
#ifdef _WIN32
        {
            /* A console of our own (not a terminal's): close it. */
            DWORD pid;
            if (GetConsoleProcessList(&pid, 1) == 1) FreeConsole();
        }
#endif
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s <main.dol> [seconds]\n(or put the game's R8AE01 folder, with sys\\main.dol and disc, beside it)\n", argv[0]);
#ifdef _WIN32
        MessageBoxA(NULL, "Put the game's R8AE01 folder (with sys\\main.dol and disc) beside the program.", "PokePark",
                    MB_ICONERROR);
#endif
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    gGameThreadId = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, on_fault);
    SetUnhandledExceptionFilter(on_unhandled);
    /* A time limit (seconds), for test runs. */
    if (argc > 2) CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)atoi(argv[2]), 0, NULL);
    if (getenv("RECOMP_PROFILE")) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gGameThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, profiler, (LPVOID)(uintptr_t)atoi(getenv("RECOMP_PROFILE")), 0, NULL);
    }
#endif
#ifdef __ANDROID__
    if (getenv("RECOMP_PROFILE")) start_profiler(atoi(getenv("RECOMP_PROFILE")));
    if (disc_image_active()) {
        /* The disc image the player chose (disc_image_android.c); argv[1]
         * is still where its main.dol would be extracted, for the saves. */
        size_t len;
        const uint8_t* dol = disc_image_dol(&len);
        if (ppc_mem_init() != 0 || ppc_load_dol_mem(dol, len, &entry) != 0) {
            fprintf(stderr, "could not load the disc image's main.dol\n");
            return 2;
        }
    } else
#endif
    if (ppc_mem_init() != 0 || ppc_load_dol(argv[1], &entry) != 0) {
        fprintf(stderr, "could not load %s\n", argv[1]);
        return 2;
    }
    setup_low_memory();
    hle_game_init();
    {
        /* <dir>/sys/main.dol -> <dir>/disc */
        char root[1024];
        char* slash;
        strncpy(root, argv[1], sizeof(root) - 16);
        root[sizeof(root) - 16] = '\0';
        for (slash = root; *slash; slash++) {
            if (*slash == '\\') *slash = '/';
        }
        slash = strrchr(root, '/');
        if (slash) *slash = '\0';
        slash = strrchr(root, '/');
        if (slash) strcpy(slash + 1, "disc");
        else strcpy(root, "disc");
        {
            /* dtk's own layout, <dir>/disc/sys/main.dol with the disc's
             * files beside sys: that disc folder itself. */
            char probe[1100];
            FILE* f;
            snprintf(probe, sizeof(probe), "%s/sys/boot.bin", root);
            if (!(f = fopen(probe, "rb")) && (slash = strrchr(root, '/')) && slash - root >= 4 &&
                !strncmp(slash - 4, "disc", 4)) {
                *slash = '\0';
                snprintf(probe, sizeof(probe), "%s/sys/boot.bin", root);
                if (!(f = fopen(probe, "rb"))) *slash = '/';
            }
            if (f) fclose(f);
        }
#ifdef __ANDROID__
        if (disc_image_active() ? disc_image_dvd_init() == 0 : hle_dvd_init(root) == 0) {
#else
        if (hle_dvd_init(root) == 0) {
#endif
            hle_dvd_boot_setup();
            hle_text_boot_setup();
        }
        /* Saves live in "nand" beside "disc", or RECOMP_NAND (test runs,
         * which keep their own). */
        strcpy(root + strlen(root) - 4, "nand");
        hle_nand_set_root(getenv("RECOMP_NAND") ? getenv("RECOMP_NAND") : root);
    }
#ifdef _WIN32
    {
        /* RECOMP_WATCH=<guest address>[,<length>]: stop on the first write
         * to that range, with a trace (for finding memory corruption). */
        const char* w = getenv("RECOMP_WATCH");
        if (w) {
            DWORD old;
            char* end;
            uint32_t addr = (uint32_t)strtoul(w, &end, 16);
            gWatchLen = *end == ',' ? (uint32_t)strtoul(end + 1, NULL, 16) : 4;
            gWatchPhys = addr & 0x3FFFFFFF;
            gWatchPage = MA(addr & ~0xFFFu);
            VirtualProtect(gWatchPage, 0x1000, PAGE_READONLY, &old);
        }
    }
    /* The whole game runs on this thread and sleeps between retraces;
     * above-normal priority gets it woken on time when other programs
     * keep the CPU busy. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    settings_load();
    update_check();
    gx_init();
    input_init();
    audio_init();
    font_init();
    sdcard_init();
    c = gCpu = (ppc_t*)calloc(1, sizeof(ppc_t));
    c->msr = 0x00002032; /* FP available, instruction and data translation on */
    printf("boot: entering __start at %08X\n", entry);
    ppc_lookup(entry)(c);
    printf("boot: __start returned\n");
    return 0;
}
