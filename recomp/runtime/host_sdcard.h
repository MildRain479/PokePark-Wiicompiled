/* Where the photos copied to the SD card go (hle_sd.c): a folder the
 * player chooses. The platform layer (recomp/boot/sdcard_win32.c,
 * sdcard_android.c) sets the hooks; without them nothing can be copied to
 * the card. */
#pragma once

#include <stdint.h>

/* Asks the player where the photos about to be copied go; 1 once they've
 * chosen a folder, 0 if they backed out. Blocks until then. */
extern int (*g_host_sd_choose)(void);

/* Saves a photo, size bytes at data, in the folder chosen last, as name
 * or, if that's taken, a free name like it; 1 if it was saved. */
extern int (*g_host_sd_save)(const char* name, const uint8_t* data, uint32_t size);
