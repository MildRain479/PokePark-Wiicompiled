/* Bringing over a save from a Wii or Dolphin (wii_save.c): the game's
 * save files put in place of its own, in the NAND folder hle_nand.c keeps
 * them in, from either
 *   - a Wii's data.bin, the encrypted copy Data Management puts on an SD
 *     card (private/wii/title/R8AE/data.bin), or
 *   - the files in Dolphin's save folder (Wii/title/00010000/52384145/data:
 *     GD0 to GD3, GWD, Pic0 to Pic3, banner.bin).
 * The save there before is kept beside it, as "data before import". The
 * platform layers (save_import_win32.c, save_import_android.c) ask for the
 * file and restart the game, which reads its saves as it starts. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A file of the save: its name in the save's folder and its contents. */
typedef struct {
    char name[64];
    const uint8_t* data;
    uint32_t size;
} WiiSaveFile;

enum {
    WIISAVE_OK,
    WIISAVE_NOT_A_SAVE,   /* neither a data.bin nor a save's files */
    WIISAVE_OTHER_GAME,   /* a save, of another game */
    WIISAVE_UNREADABLE,   /* couldn't be read */
    WIISAVE_UNWRITABLE,   /* couldn't be put in place (the old one is kept) */
};

/* A few words for the result, for the options page. */
const char* wiisave_message(int result);

/* The files of a data.bin of size bytes at bin, decrypted into it in place,
 * in files (up to max of them; their data points into bin) and their count
 * in *count. Its banner is one of them, as banner.bin. */
int wiisave_read_bin(uint8_t* bin, size_t size, WiiSaveFile* files, int max, int* count);

/* Whether files (as from Dolphin's folder) are this game's save: a GWD or
 * GD0 to GD3 among them, and nothing that isn't a plain name. */
int wiisave_check_files(const WiiSaveFile* files, int count);

/* files put in place of the save in nand_root (the NAND's folder), the one
 * there kept as "data before import" (replacing one kept before). */
int wiisave_install(const char* nand_root, const WiiSaveFile* files, int count);

/* files, as the player chose them, put in place: one that isn't one of a
 * save's own files (wiisave_own_name) taken for a data.bin, decrypted in
 * place; else a save's files, as from Dolphin's folder. */
int wiisave_import_files(const char* nand_root, WiiSaveFile* files, int count);

/* Whether name is one of the save's own files (GWD, GD0, Pic0, banner.bin
 * and the like), as in Dolphin's folder. */
int wiisave_own_name(const char* name);

/* bytes at in, size n (a multiple of 16), AES-128-CBC encrypted (decrypt
 * 0) or decrypted (1) into out, which may be in; iv is updated as the
 * chain goes. */
void wiisave_aes_cbc(const uint8_t key[16], uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n, int decrypt);

/* Asks the player for a save (the platform layer's), puts it in place and
 * restarts the game, so it's read; WIISAVE_OK if the restart couldn't be
 * made, why not otherwise, or -1 if they backed out. */
int save_import(void);

/* The host folder of the NAND (hle_nand.c). */
const char* hle_nand_root(void);

/* The SD card's key and starting IV, which a Wii encrypts saves with. */
extern const uint8_t kWiiSdKey[16], kWiiSdIv[16];

#ifdef __cplusplus
}
#endif
