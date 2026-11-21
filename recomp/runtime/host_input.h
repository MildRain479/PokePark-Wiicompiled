/* Controller input from the host, for the Wii Remote HLE (hle_wpad.c).
 * The platform layer (recomp/boot/input_win32.c) sets the hooks; without
 * them the remote is connected but nothing is pressed. */
#pragma once

#include <stdint.h>

/* The Wii Remote's buttons, as printed on it. The d-pad directions are the
 * remote's own (UP points toward the IR end); hle_wpad.c turns the
 * on-screen directions below into them for a remote held sideways. */
enum {
    HOST_PAD_SCREEN_UP = 1 << 0,
    HOST_PAD_SCREEN_DOWN = 1 << 1,
    HOST_PAD_SCREEN_LEFT = 1 << 2,
    HOST_PAD_SCREEN_RIGHT = 1 << 3,
    HOST_PAD_A = 1 << 4,
    HOST_PAD_B = 1 << 5,
    HOST_PAD_1 = 1 << 6,
    HOST_PAD_2 = 1 << 7,
    HOST_PAD_PLUS = 1 << 8,
    HOST_PAD_MINUS = 1 << 9,
    HOST_PAD_HOME = 1 << 10,
    HOST_PAD_SHAKE = 1 << 11,
    /* The second shake key: in minigames, a shake is a press of one
     * shake key after the other (hle_wpad.c). */
    HOST_PAD_SHAKE2 = 1 << 12,
};

/* Fills the HOST_PAD_* bits held on remote chan (0-3); returns 0 if that
 * remote is not connected. */
extern int (*g_host_pad_read)(int chan, uint32_t* held);
/* The same remote's buttons as the game's menus have them bound (the
 * title, the save files, the Pad, talking): 2, 1, A and B from their own
 * bindings, the rest as in the park. Read right after g_host_pad_read,
 * from the same reading; NULL: the park's. */
extern int (*g_host_menu_read)(int chan, uint32_t* held);
/* The same, as the minigames have them bound (in a minigame's scene). */
extern int (*g_host_game_read)(int chan, uint32_t* held);
/* Turns the rumble motor of remote chan on or off. */
extern void (*g_host_pad_rumble)(int chan, int on);
/* Mouse movement since the last call, in pixels (x right, y down), while
 * the mouse is captured for the camera; 0 otherwise. Set by the platform
 * layer, read by the camera patch (hle_camera.c). */
extern void (*g_host_mouse_read)(float* dx, float* dy);
/* The same movement, collected separately for the remote's pointer in
 * minigames that use it (hle_motion.c). */
extern void (*g_host_pointer_read)(float* dx, float* dy);
/* The controller's sticks, -1 to 1 each (x right, y up), dead zone taken
 * off; 0 without a controller. */
extern void (*g_host_sticks_read)(float* lx, float* ly, float* rx, float* ry);
/* Gyro aiming (boot/motion.h): the controller's turn since the last call,
 * in radians, right and up; returns 0 with it off or no gyroscope. */
extern int (*g_host_gyro_read)(float* yaw, float* pitch);
/* Gyro steering: the controller's tilt now, in radians: its right side
 * down (positive), and turned about its left-right axis, the far edge up
 * (positive), from no set level; returns 0 with it off or no sensor. */
extern int (*g_host_tilt_read)(float* roll, float* pitch);
/* Scripted input (RECOMP_PAD_SCRIPT, for test runs), the HOST_PAD_* bits
 * held, offered to the host's menus first; returns 1 if they took it, and
 * then the game doesn't get it. */
extern int (*g_host_script_input)(uint32_t held);
/* Button prompts (hle_prompts.c). The icon for a Wii Remote button (a
 * HOST_PAD_* bit; HOST_PAD_SCREEN_UP stands for the whole d-pad), as
 * coverage 0-255 in a w x h box, top row first: what the player has bound
 * to it in a layer (LAYER_*: the park, the menus, the minigames), on the
 * device used last. Returns 0 if there is none (the game's own glyph
 * stays). */
extern int (*g_host_prompt_icon)(uint32_t button, int layer, uint8_t* coverage, int w, int h);
/* Changes whenever those icons may have. */
extern uint32_t (*g_host_prompt_generation)(void);
/* The name of what the player has bound to a button (a HOST_PAD_* bit) on
 * the device used last, as the settings menu shows it ("Q", "LB"); "" if
 * nothing is. For text that names it (hle_text.c). */
extern void (*g_host_button_name)(uint32_t button, char* out, int n);
