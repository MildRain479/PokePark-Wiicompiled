/* What draws for the GX decoder: a graphics API (gl_backend.c) behind
 * gx_frames.c, which takes the gxb_* calls (gx.h), records frames to draw
 * them again between the game's, and paces them. A device only draws:
 * the EFB, textures, copies and the presented picture. The window it
 * shows them in is window_win32.c's.
 *
 * The device's calls are made on the game thread, with g_in_gl set
 * (gx_frames.c), as the sampling profiler wants. RECOMP_GPU=vulkan picks
 * the Vulkan device, else OpenGL; if one can't start, the other is
 * tried. */
#pragma once

#include <stdint.h>

#include "gx.h"

#define EFB_W 640
#define EFB_H 528

/* A device: its calls, as GXDevice gives them. gl_backend.c is the
 * OpenGL one (g_gl_device), vk_backend.c the Vulkan one (g_vk_device). */
typedef struct GXDevice {
    const char* name;
    /* Sets the device up on the window; 0 if it can't be (then another is
     * tried). */
    int (*init)(void);
    uint32_t (*texture)(int pix, int width, int height, int levels, const uint8_t* pixels, uint32_t mode0);
    void (*texture_update)(uint32_t handle, int pix, int width, int height, int levels, const uint8_t* pixels);
    void (*texture_delete)(const uint32_t* handles, int n);
    /* A draw as the decoder makes it: its vertices and indices with it,
     * and st->xf (the XF state) with the words changed since the last
     * draw. */
    void (*draw)(const GXDrawState* st, const GXVertex* v, int first_vertex, int vertices, const uint32_t* index,
                 int first_index, int count);
    /* A recorded frame's vertices and indices, which draw_recorded draws
     * from until the next frame's. */
    void (*frame_data)(const GXVertex* v, int vertices, const uint32_t* index, int count);
    /* A draw of the recorded frame, with its XF state as xf (GX_XF_WORDS
     * words) gives it. */
    void (*draw_recorded)(const GXDrawState* st, const uint32_t* xf, int first_vertex, int first_index, int count);
    void (*clear)(int color, int alpha, int depth, uint32_t rgba, uint32_t z);
    /* The texture copies of a w x h rectangle to addr go to: a number for
     * copy, made the first time; -1 if there is no room for more. Its
     * texture handle is copy_texture's. */
    int (*copy_target)(uint32_t addr, int w, int h);
    uint32_t (*copy_texture)(int target);
    void (*copy)(int target, int x, int y, int w, int h, int fmt, int intensity, int efb_alpha);
    /* Shows the EFB rectangle x, y, w x h in the window, with the settings
     * menu over it while it's open; may_drop: when the display isn't ready
     * for it, the frame may be left out rather than waited for (1
     * returned). */
    int (*present)(int x, int y, int w, int h, int may_drop);
    /* Takes up the display settings (internal resolution, anti-aliasing),
     * as the window now is; between frames. */
    void (*apply_settings)(void);
    /* The internal resolution and anti-aliasing as applied. */
    int (*scale)(void);
    int (*msaa)(void);
    /* The vertex arena (gxb_arena in gx.h); NULL where there is none, and
     * then draw and frame_data are always given vertices. */
    int (*arena_alloc)(int n);
    void (*arena_write)(int first, const GXVertex* v, int n);
    void (*arena_free)(int first, int n);
    /* The texture layouts it takes besides RGBA8 (gxb_texture_pixels); NULL
     * where there are none. */
    unsigned (*texture_pixels)(void);
    /* The copy's texture as it now is (once the frame that made it has
     * been shown): RGBA8, top row first, at the internal resolution
     * (malloc'd; w and h say its size), or NULL. */
    uint8_t* (*copy_read)(int target, int* w, int* h);
} GXDevice;

extern const GXDevice g_gl_device, g_vk_device;

/* The canvas (the aspect ratio setting): the game's picture, EFB pixels
 * 0-640 across and 0-456 down, with margins beside it (mx) or above and
 * below it (my), whole EFB pixels, for a screen wider or taller than its
 * 16:9; 0 and 0 otherwise. A device makes its EFB that much larger (EFB_W
 * + 2 mx by EFB_H + 2 my) and places what it draws, copies and shows on
 * it with these (gx_frames.c), taking the margins up with the display
 * settings. */
#define GX_PICTURE_H 456
void gx_canvas(int* mx, int* my);
/* Whether rectangle x, y, w, h (EFB pixels) is the whole picture. */
int gx_canvas_whole(float x, float y, float w, float h);
/* A draw on a canvas with margins mx, my: its viewport (x, y, w, h) and
 * scissor (x0, y0, x1, y1) in EFB pixels from the canvas's top left
 * corner, and k, what its clip-space x and y are multiplied by. */
void gx_canvas_draw(const GXDrawState* st, int mx, int my, float vp[4], int sc[4], float k[2]);
/* A rectangle of the EFB (a copy's, the one shown) on the canvas: moved
 * by the margins, or, if it is the whole picture, the whole canvas. */
void gx_canvas_rect(int mx, int my, int* x, int* y, int* w, int* h);

/* The internal resolution for a client area h pixels tall: RECOMP_GX_SCALE,
 * else the settings', else the multiple whose picture (456 lines shown)
 * comes nearest the window, rounding up unless within a quarter step
 * (gx_frames.c). */
int gx_wanted_scale(int h);

/* Window (window_win32.c). */
/* Makes the game's window, shown unless RECOMP_GX_HIDDEN=1, sized for the
 * 456 lines the game shows at twice their size. */
void win_create(void);
void* win_handle(void); /* HWND */
int win_hidden(void);
/* Width of an EFB pixel on screen, over its height: 1 in 4:3, wider in 16:9. */
double win_pixel_aspect(void);
/* The client area; 0 x 0 while minimized. */
void win_size(int* w, int* h);
/* Borderless, covering the monitor the window is on, or back to where the
 * window was. */
void win_set_fullscreen(int on);
void win_set_title(const char* title);
/* Handles the window's messages. */
void win_pump(void);
/* The refresh rate of the monitor the window is on; 0 if unknown. */
double win_display_hz(void);
