/* Making a language pack in the game (langpack.c): the player's disc of
 * another language, chosen in Options, read beside the USA game, and the
 * pack (as tools/recomp/langpack.py makes it) written to its languages
 * folder, on a thread of its own while the game goes on. */
#ifndef LANGPACK_H
#define LANGPACK_H

#include <stddef.h>
#include <stdint.h>

/* A disc's files: its FST, and a file's bytes read by its place in it. */
typedef struct LangDisc {
    const uint8_t* fst;
    uint32_t fst_size;
    /* Reads size bytes from pos on of the file at path (offset in the
     * partition) into out; 1 if all were read. */
    int (*read)(void* user, const char* path, uint64_t offset, uint32_t pos, void* out, uint32_t size);
    void* user;
} LangDisc;

/* A disc extracted by dtk at root (root/sys/fst.bin and root/files);
 * 0 if it isn't one. Freed with langdisc_free. */
int langdisc_folder(LangDisc* d, const char* root);
void langdisc_free(LangDisc* d);

/* The disc's ID (6 characters) and what language that disc's pack is
 * (LANGUAGE_*; the first of the European disc's four), or -1 if none
 * this knows. */
int langpack_language_of(const char* id);

enum { LANGPACK_IDLE, LANGPACK_RUNNING, LANGPACK_DONE, LANGPACK_FAILED };

/* Where making a pack is: LANGPACK_*, how far (0 to 100), and what to
 * show (its result once done or failed). */
int langpack_state(int* percent, const char** message);

/* Makes the pack for lang from other beside usa (on the calling thread:
 * the state says how it goes). */
void langpack_run(int lang, const LangDisc* usa, const LangDisc* other);

/* Every pack the disc of that ID makes (the Japanese one, or French,
 * German, Italian and Spanish from the European one), one after another;
 * how many it has. */
int langpack_run_disc(const char* id, const LangDisc* usa, const LangDisc* other);

/* What's said of a disc that has no pack. */
#define LANGPACK_WHICH_DISCS "%s is not a Japanese (R8AJ01) or European (R8AP01) disc" /* the disc's ID */

/* Runs fn(arg) on a thread of its own, below the game's priority; 0 if it
 * started. */
int langpack_thread(void (*fn)(void*), void* arg);

/* Asks for the other disc (the system's picker) and starts making its
 * pack (langpack_win32.c, langpack_android.c); the state says how it
 * goes. */
void langpack_choose(void);

/* A pack finished while the game used the one before it is put in its
 * place as the game starts (boot.c, before the packs are read). */
void langpack_promote(int lang);

/* Used by the platform files: the state set from outside the build
 * (a disc that couldn't be read, say). */
void langpack_fail(const char* message);
void langpack_set_running(const char* message);

#endif
