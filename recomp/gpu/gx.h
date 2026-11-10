/* GX, the Wii's GPU (Flipper/Hollywood), decoded from its command FIFO.
 *
 * The game writes GX commands to the write-gather pipe (0xCC008000). The
 * decoder (gx_fifo.c) keeps the GPU's register state (CP: vertex formats,
 * XF: transforms, BP: rasterizer, textures and TEV) and turns draw
 * commands into triangles in clip space, which a backend (gl_backend.c)
 * draws. Textures are decoded to RGBA8 by gx_texture.c. */
#pragma once

#include <stdint.h>

#include "ppc.h"

/* One vertex as the game sent it, before transformation: the backend's
 * vertex shader transforms it with the XF state (matrices, lights,
 * texture coordinate generation), as the GPU's transform unit does. */
typedef struct GXVertex {
    uint8_t mtx[12];     /* position matrix index, texture matrix indices 0..7, unused */
    float pos[3];        /* object space */
    float nrm[3];
    uint8_t color[2][4]; /* vertex colors 0 and 1, RGBA (255 when absent) */
    float tex[8][2];     /* texture coordinates as sent */
} GXVertex;

/* The XF state as the vertex shader reads it: XF memory words 0x000-0x67F
 * (matrices, normal matrices, post-transform matrices, lights) followed by
 * XF registers 0x1000-0x105F. */
#define GX_XF_MEM_WORDS 0x680
#define GX_XF_REG_WORDS 0x60
#define GX_XF_WORDS (GX_XF_MEM_WORDS + GX_XF_REG_WORDS)

/* One TEV stage, unpacked from the BP registers. */
typedef struct GXTevStage {
    uint8_t color_in[4], color_op, color_bias, color_scale, color_clamp, color_dest;
    uint8_t alpha_in[4], alpha_op, alpha_bias, alpha_scale, alpha_clamp, alpha_dest;
    uint8_t texmap, texcoord, tex_enable, channel;
    uint8_t kcolor_sel, kalpha_sel;
    uint8_t ras_swap, tex_swap;
} GXTevStage;

/* Where the minimap is on the game's screen, in EFB pixels: x0, y0, x1, y1. */
static const float kGxMinimapRect[4] = {456, 255, 597, 431};

/* Everything the backend needs to draw with the current state. */
typedef struct GXDrawState {
    int num_stages;
    GXTevStage stage[16];
    float tev_reg[4][4];   /* PREV, REG0..REG2 initial values (RGBA) */
    float konst[4][4];     /* KONST0..3 */
    uint8_t swap_table[4][4];
    int num_texcoords;
    uint32_t texture[8];   /* backend texture handles, 0 = none */
    int alpha_func[2], alpha_ref[2], alpha_logic;
    int blend_enable, blend_src, blend_dst, blend_subtract, logic_enable, logic_op;
    int color_update, alpha_update;
    int z_enable, z_func, z_update;
    int cull_mode;
    float viewport[6];     /* x, y, width, height, near, far in EFB pixels */
    int scissor[4];        /* x0, y0, x1, y1 */
    int efb_alpha;         /* the EFB pixel format has an alpha channel (RGBA6_Z24) */
    /* Fog (BP 0xEE-0xF2): its kind (0 none, 2 linear, 4 exp, 5 exp2,
     * 6 backward exp, 7 backward exp2), orthographic or not, and from a
     * pixel's 24-bit depth z its distance ze, a 2^24 / (b_mag - (z >>
     * b_shift)) (orthographic: a z / 2^24), whose part past c is fogged. */
    int fog_type, fog_ortho, fog_b_mag, fog_b_shift;
    float fog_a, fog_c, fog_color[3];
    /* Fog range adjustment (BP 0xE8-0xED), if fog_range: a pixel |dx| EFB
     * pixels from column fog_center has its distance divided by fog_k
     * (cosines) at |dx| / 32 - 1, 1 nearer than 32, linear between. */
    int fog_range;
    float fog_center, fog_k[10];
    /* Indirect texturing (BP 0x06-0x27): ind_any if a TEV stage has an
     * indirect command (ind_cmd, BP 0x10 on, raw). Indirect stage i reads
     * texture map ind_map[i] at coordinate ind_coord[i], shifted right by
     * ind_scale[i]; matrix m is two rows of S0.10 ints, scaled by 2 to
     * the power ind_mtx_scale[m] - 17. Coordinates are in texels with 7
     * fractional bits, by tc_size (BP 0x30 on: each texture coordinate's
     * size in texels). */
    int ind_stages, ind_any;
    uint32_t ind_cmd[16];
    int ind_map[4], ind_coord[4], ind_scale[4][2];
    int ind_mtx[3][2][3], ind_mtx_scale[3];
    int tc_size[8][2];
    /* The matrix indices the vertices carry (bit 0 the position's, bit 1
     * texture coordinates'); the rest are XF's MATINDEX registers' alike
     * for every vertex. */
    int mtx_per_vertex;
    /* What the draw is of: 0 the game's picture, 1 the minimap, which a
     * second screen shows (gx_fifo.c). */
    int screen;
    /* On a canvas with margins (gx_canvas, gx_device.h): 1 if the draw is
     * 3D over the whole picture, shown over the margins too, more of the
     * world; 2 if it is 2D covering the whole picture (a fade, a
     * background, an effect's copy of the screen), stretched over it all;
     * 0 otherwise (2D stays where it is in the picture, in the middle). */
    int canvas;
    const uint32_t* xf;    /* GX_XF_WORDS words, laid out as above */
    int xf_dirty_lo, xf_dirty_hi; /* words changed since the last draw (lo >= hi: none) */
    /* What the batch draws, the same from frame to frame (its display
     * lists and their draws), for matching it with the last frame's. */
    uint64_t key;
} GXDrawState;

/* How a texture's pixels are laid out: RGBA8 (gx_decode_texture's), or
 * one of the smaller layouts that hold some formats exactly, read as
 * RGBA all the same (gx_decode_texture_pix's): GX_PIX_I8, an intensity
 * byte a texel, read as (I, I, I, I); GX_PIX_IA8, intensity and alpha
 * bytes, read as (I, I, I, A); GX_PIX_BC1, 4x4 texel blocks of 8 bytes,
 * opaque. The GPU reads a smaller one in less time. */
enum { GX_PIX_RGBA8, GX_PIX_I8, GX_PIX_IA8, GX_PIX_BC1 };

/* Backend interface. */
void gxb_init(void);
/* A texture of levels mipmap levels, each half the last's size (at least
 * 1), one after another in pixels, laid out as pix says; sampled as
 * TX_SETMODE0 mode0 says (wrap, filtering, LOD bias). */
uint32_t gxb_texture(uint64_t key, int pix, int width, int height, int levels, const uint8_t* pixels, uint32_t mode0);
/* Replaces a texture's contents (same layout, size and levels). */
void gxb_texture_update(uint32_t handle, int pix, int width, int height, int levels, const uint8_t* pixels);
/* The layouts textures may have besides RGBA8, a bit (1 << pix) each. */
unsigned gxb_texture_pixels(void);
/* Changes when textures should be made again (the filtering or
 * texture pack setting changed). */
int gxb_texture_generation(void);
void gxb_texture_free(uint32_t handle);
/* Draws triangles: count indices from index + first_index, three to a
 * triangle, into the vertices from v + first_vertex. */
void gxb_draw_triangles(const GXDrawState* st, const GXVertex* v, int first_vertex, int vertices, const uint32_t* index,
                        int first_index, int count);
/* Whether the backend records frames to draw them again (interpolation):
 * then the decoder keeps a whole frame's vertices and indices, draws give
 * their place in them, and gxb_frame_data hands them over before the
 * frame is presented. Asked at the start of each frame. */
int gxb_recording(void);
void gxb_frame_data(const GXVertex* v, int vertices, const uint32_t* index, int count);
/* The vertex arena, where the device has one (gxb_arena): vertices kept
 * in video memory from frame to frame. Draws and frame data given no
 * vertices (v NULL) index it from its start. A span is allocated (its
 * first vertex returned), written, and freed; once freed, it may be used
 * again as soon as the GPU no longer draws from it, so it is freed only
 * when no recorded frame draws from it either. */
int gxb_arena(void);
int gxb_arena_alloc(int n);
void gxb_arena_write(int first, const GXVertex* v, int n);
void gxb_arena_free(int first, int n);
void gxb_clear(int color, int alpha, int depth, uint32_t rgba, uint32_t z);
/* Copies an EFB rectangle to a texture, converted to copy format `fmt`
 * (exec bits 3-6) and stored as the game reads it back: an I8 copy as
 * intensity in every channel, an RGB565 copy with opaque alpha. */
void gxb_copy_to_texture(uint32_t addr, int x, int y, int w, int h, int fmt, int intensity, int efb_alpha, int stride);
/* Shows the EFB rectangle the game copied to the XFB. */
void gxb_present(int x, int y, int w, int h);

/* Texture packs (texpack.c): whether one is on and has pictures, how
 * many it has, and a texture's replacement, if it has one: fmt, width x
 * height, its first level's size bytes at data, mipmaps if it has a
 * mipmap filter, and a colour-indexed one's palette (tlut_size bytes:
 * all its entries); RGBA8 levels (malloc'd), as many as levels at most,
 * the first out_w x out_h. */
int texpack_on(void);
int texpack_count(void);
uint8_t* texpack_find(const uint8_t* data, int size, int fmt, int width, int height, int mipmaps, const uint8_t* tlut,
                      int tlut_size, int levels, int* out_w, int* out_h, int* out_levels);

/* Texture decoding (gx_texture.c). Output is RGBA8, row-major. */
int gx_texture_size(int fmt, int width, int height);
void gxb_copy_sync(uint32_t addr);
void gx_frame_data_so_far(void);
void gx_encode_copy(uint8_t* dst, int fmt, int intensity, int w, int h, int stride, const uint8_t* rgba);
void gx_decode_texture(uint8_t* out, const uint8_t* src, int fmt, int width, int height,
                       const uint8_t* tlut, int tlut_fmt);
/* The smallest of the layouts in `allowed` (bits, 1 << GX_PIX_...) that
 * holds the texture at src (levels of them) exactly; GX_PIX_RGBA8 if none. */
int gx_texture_pix(int fmt, const uint8_t* src, int width, int height, int levels, unsigned allowed);
/* A level's size in layout pix, and the level decoded to it (pix not RGBA8,
 * and one gx_texture_pix gives for the format). */
size_t gx_pix_size(int pix, int width, int height);
void gx_decode_texture_pix(uint8_t* out, const uint8_t* src, int fmt, int width, int height, int pix);

/* FIFO entry points (gx_fifo.c). */
void gx_init(void);
void gx_flush(void); /* decode everything written so far */
