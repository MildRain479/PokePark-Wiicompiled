/* GX, the graphics API. Commands written to the FIFO are decoded and drawn
 * by recomp/gpu (in the boot target); the GPU finishes instantly, so waits
 * for it decode the pending commands and return. */
#include "recomp_funcs.h"

/* void GXDrawDone(void): queues a draw-done token and sleeps until the
 * GPU's pixel engine reports it. */
void f_80220890(ppc_t* c) {
    (void)c;
    if (g_gx_flush) g_gx_flush();
}

/* The JPEG encoder (TMCC's), for the photos taken with minus: (context,
 * RGB565 source, width, height, ...). The source is the screen's EFB
 * copy, which the game makes every frame and the GPU module keeps on the
 * GPU; it goes into memory first. */
void f_8028EF88(ppc_t* c) {
    if (g_gx_copy_sync) g_gx_copy_sync(c->r[4] & 0x3FFFFFFF);
    f_8028EF88_orig(c);
}

/* The minimap: in the layer drawn last, its translucent pass (fn_801CDCE0)
 * from the frame's drawing (fn_800323B0), with the berry counter in the
 * park and the menus elsewhere. That pass is marked in the FIFO, a write
 * to CP register 0xF0 (one the GPU has not) before and after, for the
 * second screen to pick the minimap out of (gx_fifo.c). */
static void mark_minimap(uint32_t on) {
    g_wgpipe_write(0x08, 1);
    g_wgpipe_write(0xF0, 1);
    g_wgpipe_write(on, 4);
}

void hle_friends_update(ppc_t* c);
void hle_autosave_update(ppc_t* c);

/* void fn_801CDCE0(Scene*) */
void f_801CDCE0(ppc_t* c) {
    int minimap = c->lr == 0x800328A0u;
    if (minimap) mark_minimap(1);
    f_801CDCE0_orig(c);
    if (minimap) {
        mark_minimap(0);
        hle_friends_update(c); /* hle_friends.c: on the game's thread, in the park */
        hle_autosave_update(c); /* hle_autosave.c */
    }
}
