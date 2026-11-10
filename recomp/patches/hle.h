/* Shared helpers for the high-level replacements (HLE) of SDK and
 * hardware functions. */
#pragma once

#include <stdio.h>

#include "recomp_funcs.h"

/* RECOMP_HLE_LOG=1 logs HLE requests; 2 also logs thread switches with
 * guest backtraces. */
int hle_log_enabled(void);
#define HLE_LOG(...) do { if (hle_log_enabled()) { fprintf(stderr, "hle: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
/* Warnings are printed at most three times per call site. */
#define HLE_WARN(...) do { static int n_; if (n_ < 3) { fprintf(stderr, "hle: " __VA_ARGS__); fputs(++n_ == 3 ? " (further repeats hidden)\n" : "\n", stderr); } } while (0)

/* Queues a call fn(r3, r4) into guest code, made the next time the game
 * enables interrupts, the way a hardware interrupt would be taken. */
void hle_post(uint32_t fn, uint32_t r3, uint32_t r4, void (*after)(ppc_t* c, uint32_t data), uint32_t data);

/* Raises hardware interrupt n (__OS_INTERRUPT_*); its OS handler runs
 * like a queued call. */
void hle_raise_interrupt(int n);
/* ...calling before(data) just before the handler runs, e.g. to set the
 * device register that tells the handler what happened. */
void hle_raise_interrupt_ex(int n, void (*before)(uint32_t data), uint32_t data);

/* Periodic timers in guest time-base ticks, fired like interrupts. */
void hle_timer_start(int id, uint64_t period, void (*fire)(ppc_t* c));
void hle_timer_stop(int id);
enum { HLE_TIMER_AUDIO = 0, HLE_TIMER_PAD = 1 };

/* The emulated video refresh rate (RECOMP_VI_HZ, default 60). */
double hle_vi_hz(void);

/* What remote 0 had held at its last read (HOST_PAD_*). */
uint32_t hle_wpad_held(void);

/* Whether the console is set up for a 16:9 TV (the aspect ratio setting,
 * or RECOMP_WIDESCREEN: 1, 0 for 4:3). The game then draws 16:9 pictures squeezed into its
 * 640-pixel-wide frame, as the Wii does for a TV to stretch out. */
int hle_widescreen(void);
/* hle_game.c: whether the scene is a minigame (an attraction). */
int hle_in_minigame(void);
/* hle_game.c: the minigame being played (its At number, 1-16), or 0. */
int hle_minigame(void);
/* hle_game.c: enters minigame at (1-16) for testing, from the park. */
void hle_request_minigame(int at);
/* hle_motion.c: the remote's motion in the minigame being played, from
 * keys, sticks and the mouse, for report number report. Returns 0 outside
 * a minigame; otherwise sets acc (accelerometer x, y, z) and returns 1,
 * or 3 with dots (two sensor-bar dots in the IR camera's view) set too. */
int hle_motion(uint32_t held, uint32_t report, int16_t acc[3], int16_t dots[4][2]);
/* hle_prompts.c: redraws the button prompt glyphs if they should change. */
void hle_prompts_update(void);
int hle_password_press(void);
int hle_password_unlock_saying(void);
/* hle_pause.c: a texture of a model file, as the file is set up. */
void hle_pause_texture(uint32_t tex);

/* Runs queued calls if any; preserves all guest registers. */
void hle_deliver(ppc_t* c);

/* Waits for the next event (a queued completion; later, timers and
 * retrace) and delivers it. Returns 0 if nothing could ever arrive. */
int hle_wait_for_event(ppc_t* c);

/* Thread scheduling and interrupt frames: see hle_thread.c. */
void hle_idle_check(ppc_t* c);
void hle_interrupt(ppc_t* c, uint32_t fn, uint32_t r3, uint32_t r4);
int hle_in_interrupt(void);

/* Calls a guest function with up to two arguments and returns r3,
 * preserving the caller's registers. */
uint32_t hle_call(ppc_t* c, uint32_t fn, uint32_t r3, uint32_t r4);

/* The disc (hle_dvd.c). hle_dvd_init takes the directory holding sys/
 * and files/ extracted from the disc; hle_dvd_init_image, a disc image's
 * data partition instead (disc_image.c): its boot.bin, FST and TMD, and
 * read, which reads the partition at an offset (0 on failure). */
int hle_dvd_init(const char* root);
int hle_dvd_init_image(const uint8_t* boot, const uint8_t* fst, uint32_t fstSize, const uint8_t* tmd,
                       uint32_t tmdSize, int (*read)(uint64_t offset, void* out, uint32_t len));
void hle_dvd_boot_setup(void);
void hle_text_boot_setup(void);
int hle_dvd_read(uint64_t offset, uint32_t len, uint32_t dst);
uint32_t hle_dvd_tmd(uint32_t dst, uint32_t max);
const uint8_t* hle_dvd_disc_id(void);

/* Copies a NUL-terminated guest string (at most n - 1 bytes). */
void hle_read_str(uint32_t addr, char* out, size_t n);
