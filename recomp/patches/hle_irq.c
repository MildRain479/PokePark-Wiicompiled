/* Interrupt delivery. Real hardware raises an interrupt that the CPU takes
 * once external interrupts are enabled; here, pending events are checked
 * whenever the game enables interrupts (OSEnableInterrupts or
 * OSRestoreInterrupts) and run as interrupts (hle_interrupt). Events:
 *  - completions queued by HLE devices (hle_post), such as IOS replies;
 *  - the decrementer, which drives OSAlarm. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#define QUEUE_SIZE 64

static const uint32_t kDecrementerHandler = 0x80207B30; /* DecrementerExceptionHandler */
#define EXC_DECREMENTER 8

typedef struct {
    int irq; /* >= 0: a hardware interrupt, dispatched through the OS table */
    void (*before)(uint32_t data); /* sets device registers the handler reads */
    uint32_t fn, r3, r4;
    void (*after)(ppc_t* c, uint32_t data);
    uint32_t data;
} Pending;

static Pending sQueue[QUEUE_SIZE];
static unsigned sHead, sTail;

int hle_log_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("RECOMP_HLE_LOG");
        enabled = v ? atoi(v) : 0;
    }
    return enabled;
}

void hle_read_str(uint32_t addr, char* out, size_t n) {
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        out[i] = (char)LD8(addr + (uint32_t)i);
        if (!out[i]) return;
    }
    out[i] = '\0';
}

void hle_post(uint32_t fn, uint32_t r3, uint32_t r4, void (*after)(ppc_t* c, uint32_t data), uint32_t data) {
    Pending* p;
    if (sTail - sHead >= QUEUE_SIZE) {
        HLE_WARN("interrupt queue full; dropping a completion");
        return;
    }
    p = &sQueue[sTail++ % QUEUE_SIZE];
    p->irq = -1;
    p->fn = fn;
    p->r3 = r3;
    p->r4 = r4;
    p->after = after;
    p->data = data;
}

uint32_t hle_call(ppc_t* c, uint32_t fn, uint32_t r3, uint32_t r4) {
    ppc_t saved;
    uint32_t ret;
    memcpy(&saved, c, sizeof(saved));
    c->r[1] = (c->r[1] - 0x200) & ~7u;
    c->r[3] = r3;
    c->r[4] = r4;
    c->lr = 0;
    ppc_icall(c, fn);
    ret = c->r[3];
    memcpy(c, &saved, sizeof(saved));
    return ret;
}

static int dec_expired(void) {
    uint64_t deadline;
    return ppc_dec_deadline(&deadline) && (int64_t)(ppc_timebase() - deadline) >= 0;
}

/* Vertical retrace, once VIInit has registered its handler for the VI
 * interrupt. The game's frames follow it. A Wii's NTSC output retraces
 * 59.94 times a second; here the default is 60, to match PC displays (the
 * game runs 0.1% faster; sound is clocked separately). RECOMP_VI_HZ sets
 * another rate, e.g. 59.94 for the original. */
#define INTERRUPT_PI_VI 24
#define VI_DI0 0xCC002030u
static const uint32_t kInterruptHandlerTable = 0x80486AF0; /* holds the table's address */
static uint64_t sRetracePeriod;
static uint64_t sNextRetrace;
uint32_t g_retrace_count;

static uint32_t interrupt_handler(int n) {
    uint32_t table = LD32(kInterruptHandlerTable);
    return table ? LD32(table + 4u * n) : 0;
}

double hle_vi_hz(void) {
    static double hz;
    if (!hz) {
        const char* e = getenv("RECOMP_VI_HZ");
        hz = e ? atof(e) : 60.0;
        if (hz < 1 || hz > 1000) hz = 60.0;
    }
    return hz;
}

static int retrace_due(void) {
    if (!interrupt_handler(INTERRUPT_PI_VI)) return 0;
    if (!sRetracePeriod) sRetracePeriod = (uint64_t)(PPC_TB_HZ / hle_vi_hz() + 0.5);
    if (!sNextRetrace) sNextRetrace = ppc_timebase() + sRetracePeriod;
    return (int64_t)(ppc_timebase() - sNextRetrace) >= 0;
}

void hle_raise_interrupt_ex(int n, void (*before)(uint32_t data), uint32_t data) {
    Pending* p;
    if (sTail - sHead >= QUEUE_SIZE) {
        HLE_WARN("interrupt queue full; dropping interrupt %d", n);
        return;
    }
    p = &sQueue[sTail++ % QUEUE_SIZE];
    memset(p, 0, sizeof(*p));
    p->irq = n;
    p->before = before;
    p->data = data;
}

void hle_raise_interrupt(int n) { hle_raise_interrupt_ex(n, NULL, 0); }

/* Periodic device timers (audio DMA, ...), driven from host time. */
#define MAX_TIMERS 4
static struct {
    int active;
    uint64_t next, period;
    void (*fire)(ppc_t* c);
} sTimers[MAX_TIMERS];

void hle_timer_start(int id, uint64_t period, void (*fire)(ppc_t* c)) {
    sTimers[id].period = period ? period : 1;
    sTimers[id].fire = fire;
    if (!sTimers[id].active) sTimers[id].next = ppc_timebase() + sTimers[id].period;
    sTimers[id].active = 1;
}

void hle_timer_stop(int id) { sTimers[id].active = 0; }

static int timer_due(void) {
    uint64_t now = ppc_timebase();
    int i;
    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].active && (int64_t)(now - sTimers[i].next) >= 0) return i;
    }
    return -1;
}

/* Set while a completion's after-step runs (free_request's iosFree, which
 * IOS does inside its interrupt). */
static int sInAfter;

void hle_deliver(ppc_t* c) {
    /* Interrupts are off inside a handler; a thread the handler switched
     * to takes them as usual. */
    if (hle_in_interrupt() || sInAfter) return;
    for (;;) {
        if (sHead != sTail) {
            Pending p = sQueue[sHead++ % QUEUE_SIZE];
            if (p.irq >= 0) {
                uint32_t fn = interrupt_handler(p.irq);
                if (p.before) p.before(p.data);
                if (fn) hle_interrupt(c, fn, (uint32_t)p.irq, LD32(0x800000D4));
                continue;
            }
            HLE_LOG("interrupt: %08X [%s](%08X, %08X)", p.fn, ppc_func_name(p.fn), p.r3, p.r4);
            if (p.fn) hle_interrupt(c, p.fn, p.r3, p.r4);
            if (p.after) {
                /* With interrupts still off, as on the console. Taken from
                 * the iosFree it calls, the next completion ran inside
                 * that call: a chain of disc commands, each queued by the
                 * last one's callback, went 0x220 bytes deeper a command
                 * until the thread ran off its stack into the memory below
                 * (the sound mixer's channels, the disc library's
                 * contexts), and the ending movie hung or crashed. */
                sInAfter = 1;
                p.after(c, p.data);
                sInAfter = 0;
            }
            continue;
        }
        /* Timed events that are due, oldest first. After a stall several
         * are overdue at once, and they must interleave as they would
         * have on time: the THP player, for one, refills its audio ring
         * from the retrace callback while the audio timer plays it, and
         * running all the audio ticks first would play past the refill. */
        {
            uint64_t now = ppc_timebase(), when = 0, dec;
            int which = -1, i; /* 0..MAX_TIMERS-1 timer, MAX_TIMERS dec, MAX_TIMERS+1 retrace */
            if (ppc_dec_deadline(&dec) && (int64_t)(now - dec) >= 0) {
                which = MAX_TIMERS;
                when = dec;
            }
            for (i = 0; i < MAX_TIMERS; i++) {
                if (sTimers[i].active && (int64_t)(now - sTimers[i].next) >= 0 &&
                    (which < 0 || (int64_t)(sTimers[i].next - when) < 0)) {
                    which = i;
                    when = sTimers[i].next;
                }
            }
            if (retrace_due() && (which < 0 || (int64_t)(sNextRetrace - when) < 0)) which = MAX_TIMERS + 1;
            if (which < 0) break;
            if (which == MAX_TIMERS) {
                ppc_dec_disarm();
                hle_interrupt(c, kDecrementerHandler, EXC_DECREMENTER, LD32(0x800000D4));
            } else if (which == MAX_TIMERS + 1) {
                sNextRetrace += sRetracePeriod;
                g_retrace_count++;
                if (g_gx_flush) g_gx_flush();
                ST16(VI_DI0, LD16(VI_DI0) | 0x8000);
                hle_interrupt(c, interrupt_handler(INTERRUPT_PI_VI), INTERRUPT_PI_VI, LD32(0x800000D4));
                return;
            } else {
                sTimers[which].next += sTimers[which].period;
                /* Ticks delayed by a short stall are caught up (each audio
                 * tick is 3 ms of sound, so skipping them would cut the
                 * sound); after a long one (over 250 ms), the cadence
                 * resumes instead of bursting. */
                if ((int64_t)(now - sTimers[which].next) > (int64_t)(PPC_TB_HZ / 4)) sTimers[which].next = now + sTimers[which].period;
                sTimers[which].fire(c);
                /* One tick per delivery: a tick often wakes a thread that
                 * must run before the next one (the THP player refills
                 * its audio ring from a thread each audio frame wakes),
                 * so overdue ticks are spread over the following delivery
                 * points instead of run back to back. */
                return;
            }
        }
    }
}

/* Waits until the time base reaches deadline. Sleep() wakes on the
 * system timer tick (15.6 ms by default), up to a tick late. A
 * high-resolution waitable timer wakes within a fraction of a millisecond
 * (0.3 ms late on average here). For a deadline the game's frames hang on
 * (the retrace and its alarms), it is set a quarter millisecond early and
 * a short spin covers the rest: without that, the 60 fps screens dropped
 * to 49 fps. The device timers (sound every 3 ms, the remote every 5 ms)
 * catch up on lateness, so for them it just sleeps: they are over half
 * the wakes, and spinning for them cost a sixth of a core. */
#define SPIN_TICKS (PPC_TB_HZ / 4000) /* 0.25 ms */
static void sleep_until_inner(uint64_t deadline, int precise);

/* Time spent waiting here, in time-base ticks (for the busy share the fps
 * line shows). */
uint64_t g_wait_ticks;

static void sleep_until(uint64_t deadline, int precise) {
    uint64_t start = ppc_timebase();
    sleep_until_inner(deadline, precise);
    g_wait_ticks += ppc_timebase() - start;
}

static void sleep_until_inner(uint64_t deadline, int precise) {
#ifdef _WIN32
    static HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    for (;;) {
        int64_t left = (int64_t)(deadline - ppc_timebase()), early = precise ? (int64_t)SPIN_TICKS : 0;
        if (left <= 0) return;
        if (left > early) {
            /* In 100 ns units, negative for a relative time. */
            LARGE_INTEGER due;
            due.QuadPart = -((left - early) * 10000000 / (int64_t)PPC_TB_HZ);
            if (due.QuadPart == 0) due.QuadPart = -1;
            if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
            else Sleep(1);
            if (!precise) return;
        } else {
            YieldProcessor();
        }
    }
#else
    for (;;) {
        int64_t left = (int64_t)(deadline - ppc_timebase()), early = precise ? (int64_t)SPIN_TICKS : 0;
        if (left <= 0) return;
        if (left > early) {
            int64_t ns = (left - early) * 1000000000 / (int64_t)PPC_TB_HZ;
            struct timespec ts;
            ts.tv_sec = (time_t)(ns / 1000000000);
            ts.tv_nsec = (long)(ns % 1000000000);
            nanosleep(&ts, NULL);
            if (!precise) return;
        }
    }
#endif
}

int hle_wait_for_event(ppc_t* c) {
    uint64_t deadline = 0, dec;
    int have = 0;
    if (sHead == sTail && !dec_expired() && !retrace_due() && timer_due() < 0) {
        int precise = 1;
        if (ppc_dec_deadline(&dec)) {
            deadline = dec;
            have = 1;
        }
        if (interrupt_handler(INTERRUPT_PI_VI) && (!have || (int64_t)(sNextRetrace - deadline) < 0)) {
            deadline = sNextRetrace;
            have = 1;
        }
        {
            int i;
            for (i = 0; i < MAX_TIMERS; i++) {
                if (sTimers[i].active && (!have || (int64_t)(sTimers[i].next - deadline) < 0)) {
                    deadline = sTimers[i].next;
                    have = 1;
                    precise = 0;
                }
            }
        }
        if (!have) return 0;
        /* Frames shown between the game's (above 60 fps) are drawn while
         * it waits, but only when one should be done before the wait is
         * over: a late wake-up slows the game itself. Otherwise the
         * frame waits for the next one (or is left out). */
        if (g_host_frame_due) {
            static uint64_t cost; /* a frame's drawing, smoothed, in ticks */
            uint64_t due;
            while ((due = g_host_frame_due()) != 0 && (int64_t)(due - deadline) < 0) {
                uint64_t start;
                if ((int64_t)(deadline - due) < (int64_t)(cost + cost / 2)) {
                    cost -= cost / 16; /* so one slow stretch doesn't leave them out for good */
                    break;
                }
                sleep_until(due, 1);
                start = ppc_timebase();
                g_host_frame_render();
                cost = (cost * 7 + (ppc_timebase() - start)) / 8;
            }
        }
        sleep_until(deadline, precise);
    }
    hle_deliver(c);
    return 1;
}

/* BOOL OSEnableInterrupts(void) */
void f_8020D310(ppc_t* c) {
    uint32_t lr = c->lr;
    f_8020D310_orig(c);
    hle_deliver(c);
    c->lr = lr;
    hle_idle_check(c);
}

/* BOOL OSRestoreInterrupts(BOOL level) */
void f_8020D330(ppc_t* c) {
    uint32_t enable = c->r[3];
    f_8020D330_orig(c);
    if (enable) hle_deliver(c);
}
