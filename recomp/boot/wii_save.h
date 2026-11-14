/* Bringing over a save from a Wii or Dolphin (wii_save.c): the game's
 * save files put in place of its own, in the NAND folder hle_nand.c keeps
 * them in, from either
 *   - a Wii's data.bin, the encrypted copy Data Management puts on an SD
 *     card (private/wii/title/R8AE/data.bin), or
 *   - the files in Dolphin's save folder (Wii/title/00010000/52384145/data:
 *     GD0 to GD3, GWD, Pic0 to Pic3, banner.bin), or a zip of them.
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

/* Whether size bytes at data are a zip. */
int wiisave_is_zip(const uint8_t* data, size_t size);

/* The save in a zip of size bytes at zip (Android's picker can't choose a
 * folder, so Dolphin's is zipped): a save's files, in whatever folder in
 * it, or a lone data.bin, unpacked into files (up to max; each one's data
 * malloc'd, for the caller to free) and their count in *count. Nothing is
 * left to free unless WIISAVE_OK. */
int wiisave_read_zip(const uint8_t* zip, size_t size, WiiSaveFile* files, int max, int* count);

/* files, as the player chose them, put in place: a lone zip unpacked
 * (wiisave_read_zip); one that isn't one of a save's own files
 * (wiisave_own_name) taken for a data.bin, decrypted in place; else a
 * save's files, as from Dolphin's folder. */
int wiisave_import_files(const char* nand_root, WiiSaveFile* files, int count);

/* Whether name is one of the save's own files (GWD, GD0, Pic0, banner.bin
 * and the like), as in Dolphin's folder. */
int wiisave_own_name(const char* name);

/* bytes at in, size n (a multiple of 16), AES-128-CBC encrypted (decrypt
 * 0) or decrypted (1) into out, which may be in; iv is updated as the
 * chain goes. */
void wiisave_aes_cbc(const uint8_t key[16], uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n, int decrypt);

/* Taking the save out, to Dolphin or another copy of this: a zip of its
 * files, as in Dolphin's save folder (unzipped into it, they're Dolphin's
 * save; importing the zip brings it back). */

/* A few words for an export's result, for the options page. */
const char* wiisave_export_message(int result);

/* files, deflated, as a zip (each dated dos_time, MS-DOS's date and time):
 * malloc'd, its size in *size, or NULL. */
uint8_t* wiisave_write_zip(const WiiSaveFile* files, int count, uint32_t dos_time, size_t* size);

/* The save in nand_root as a zip (wiisave_write_zip) in *zip, malloc'd,
 * for the caller to free: WIISAVE_OK, WIISAVE_NOT_A_SAVE if there's none
 * yet, or WIISAVE_UNREADABLE. */
int wiisave_export_zip(const char* nand_root, uint32_t dos_time, uint8_t** zip, size_t* size);

/* Asks the player where to put the save's zip (the platform layer's) and
 * writes it there: WIISAVE_OK, why not, or -1 if they backed out. */
int save_export(void);

/* Now as MS-DOS keeps it, for a zip. */
uint32_t wiisave_dos_now(void);

/* Asks the player for a save (the platform layer's), puts it in place and
 * restarts the game, so it's read; WIISAVE_OK if the restart couldn't be
 * made, why not otherwise, or -1 if they backed out. */
int save_import(void);

#ifdef __ANDROID__
/* As the game starts, before its saves are read: a save chosen for a game
 * that Android stopped while the picker was up, put in place
 * (save_import_android.c). */
void save_import_pending(void);
#endif

/* The host folder of the NAND (hle_nand.c). */
const char* hle_nand_root(void);

/* The SD card's key and starting IV, which a Wii encrypts saves with. */
extern const uint8_t kWiiSdKey[16], kWiiSdIv[16];

#ifdef __cplusplus
}
#endif
