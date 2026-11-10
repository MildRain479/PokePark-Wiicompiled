/* The SD card, which the photo album copies photos to. The game reaches it
 * through the SDK's FA library: PrFILE2, a FAT file system on the card's
 * blocks over /dev/sdio/slot0, behind a manager of the game's own
 * (static_8036dbe8, 0x8000EEB0-0x80011700) that runs it on a thread.
 * Here FA's calls are answered on the host instead, by a card kept in
 * memory: one that's always in the slot, starts empty each run, and has
 * room. A photo written to it (\DCIM\100NIN01\R8A_0001.JPG and so on) is
 * saved in a folder the player chooses (host_sdcard.h), asked for once
 * each time photos are copied; backing out fails the copy, as a card
 * that couldn't be written would. The card keeps each file's name and
 * size, so the game numbers the next photos on from them.
 *
 * FA's calls, as the manager makes them (several of the symbols' names
 * are a function off: they're those of the PrFILE2 stubs in turn, and
 * FAFclose is pf_fgetsize's, FAErrnum's neighbour pf_fclose's):
 *   FAInit(heap size), FARegistCB(drive, insert cb, eject cb),
 *   FAAttach(type, , , drive table: +8 its letter, +9 bit 4 a card in),
 *   mount(drive) -1 on failure, FAIsWriteProtected(drive),
 *   devinf(drive, info: +4 free clusters, +8 sectors a cluster, +C bytes
 *   a sector), FAErrnum(), opendir(path) a handle or 0, closedir(handle),
 *   FAMkdir(path), fopen(path, mode) a handle or 0, FACreate(path, attr)
 *   a handle or 0, fclose(handle), fgetsize(handle, size*), fseek(handle,
 *   offset, origin), fwrite(data, size, count, handle) the count written,
 *   fsfirst(pattern, attributes, entry: +0x22D its name) and fsnext(entry)
 *   0 or -1, FAFstat(path, stat: +0 size, +0x10 attributes), FARemove(path)
 *   and FAUnmount(drive, mode). 0 is success unless said; errnum 2 is
 *   PF_ENOENT, which also ends a search. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle.h"
#include "host_sdcard.h"

int (*g_host_sd_choose)(void);
int (*g_host_sd_save)(const char* name, const uint8_t* data, uint32_t size);

#define PF_ENOENT 2
#define PF_EEXIST 7
#define PF_EIO 8
#define ATTR_DIR 0x10
#define ATTR_FILE 0x20 /* "archive": a file */
#define DIRENT_NAME 0x22D

/* A minute between copies is a new one: the player is asked again. */
#define BATCH_SECONDS 60

typedef struct {
    char path[128]; /* from the root, "\DCIM\100NIN01": no drive, no trailing \ */
    int dir;
    uint32_t size;
    uint8_t* data; /* while open: its contents (saved, then let go, when it's closed) */
} Entry;

typedef struct {
    int entry; /* the entry, plus one; 0: free */
    uint32_t pos;
    int wrote;
} Open;

typedef struct {
    uint32_t dirent; /* the game's entry being filled; 0: free */
    char dir[128], pattern[64];
    int attr, next;
} Search;

#define MAX_ENTRIES 4096
static Entry sEntries[MAX_ENTRIES];
static int sCount;
static Open sOpen[16];
static Search sSearches[8];
static int sErr;
static int sChosen = -1;  /* this copy's folder: 1 chosen, 0 backed out, -1 not asked */
static time_t sLastWrite; /* when the card was last written */

/* A path as the card keeps it. */
static void norm(const char* in, char* out, size_t n) {
    size_t o = 0;
    if (in[0] && in[1] == ':') in += 2;
    if (*in != '\\' && *in != '/') out[o++] = '\\';
    for (; *in && o + 1 < n; in++) {
        char ch = *in == '/' ? '\\' : *in;
        if (ch == '\\' && o && out[o - 1] == '\\') continue;
        out[o++] = ch;
    }
    if (o > 1 && out[o - 1] == '\\') o--;
    out[o] = '\0';
}

static int same(const char* a, const char* b) {
    for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) {}
    return tolower((unsigned char)*a) == tolower((unsigned char)*b);
}

/* FAT's wildcards: * any run, ? any one character. */
static int matches(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') return matches(pat + 1, s) || (*s && matches(pat, s + 1));
    if (!*s) return 0;
    return (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) && matches(pat + 1, s + 1);
}

/* The folder a path is in ("\" for the root's), and its name. */
static void split(const char* path, char* dir, size_t n, const char** name) {
    const char* slash = strrchr(path, '\\');
    size_t len = slash > path ? (size_t)(slash - path) : 1;
    if (len >= n) len = n - 1;
    memcpy(dir, path, len);
    dir[len] = '\0';
    *name = slash ? slash + 1 : path;
}

static int find(const char* path) {
    int i;
    if (!strcmp(path, "\\")) return MAX_ENTRIES; /* the root */
    for (i = 0; i < sCount; i++)
        if (same(sEntries[i].path, path)) return i;
    return -1;
}

static int is_dir(int e) { return e == MAX_ENTRIES || (e >= 0 && sEntries[e].dir); }

static int add(const char* path, int dir) {
    Entry* e;
    char parent[128];
    const char* name;
    split(path, parent, sizeof(parent), &name);
    if (!is_dir(find(parent)) || sCount == MAX_ENTRIES || strlen(path) >= sizeof(e->path)) return -1;
    e = &sEntries[sCount];
    memset(e, 0, sizeof(*e));
    strcpy(e->path, path);
    e->dir = dir;
    return sCount++;
}

static int fail(int err) {
    sErr = err;
    return -1;
}

static void set_r3(ppc_t* c, int32_t v) { c->r[3] = (uint32_t)v; }

static void read_path(uint32_t addr, char* out, size_t n) {
    char raw[256];
    hle_read_str(addr, raw, sizeof(raw));
    norm(raw, out, n);
}

/* Whether this copy has somewhere to go: asks the first time. */
static int have_folder(void) {
    time_t now = time(NULL);
    if (sChosen < 0 || now - sLastWrite > BATCH_SECONDS) {
        sChosen = g_host_sd_choose && g_host_sd_save ? g_host_sd_choose() : 0;
        HLE_LOG("SD: %s", sChosen ? "folder chosen" : "no folder: the copy fails");
    }
    sLastWrite = time(NULL);
    return sChosen;
}

/* ------------------------------------------------------------------------ */
/* The card */

/* FAInit(heap size): the card's driver isn't started. */
void f_802E4580(ppc_t* c) { set_r3(c, 0); }

/* FAAttach(type, ..., drive table): a card is in drive A. */
void f_802E4050(ppc_t* c) {
    uint32_t drive = c->r[6];
    if (drive) {
        ST8(drive + 8, 'A');
        ST8(drive + 9, LD8(drive + 9) | 0x10);
    }
    set_r3(c, 0);
}

/* FADetach, mount(drive), the media check after a failed mount,
 * FAIsWriteProtected(drive) (not), FAUnmount(drive, mode). */
void f_802E4300(ppc_t* c) { set_r3(c, 0); }
void f_802E46C0(ppc_t* c) { set_r3(c, 0); }
void f_802E42B0(ppc_t* c) { set_r3(c, 0); }
void f_802E4650(ppc_t* c) { set_r3(c, 0); }
void f_802E48A0(ppc_t* c) { set_r3(c, 0); }

/* devinf(drive, info): a 1 GB card, empty. */
void f_802E4550(ppc_t* c) {
    uint32_t info = c->r[4];
    if (info) {
        ST32(info + 0x0, 0x8000);    /* clusters */
        ST32(info + 0x4, 0x8000);    /* free ones */
        ST32(info + 0x8, 64);        /* sectors a cluster */
        ST32(info + 0xC, 512);       /* bytes a sector */
    }
    set_r3(c, 0);
}

/* FAErrnum() */
void f_802E43B0(ppc_t* c) { set_r3(c, sErr); }

/* ------------------------------------------------------------------------ */
/* Folders */

/* opendir(path): a handle (the entry, plus one) or 0. */
void f_802E46F0(ppc_t* c) {
    char path[128];
    int e;
    read_path(c->r[3], path, sizeof(path));
    e = find(path);
    if (!is_dir(e)) {
        sErr = PF_ENOENT;
        set_r3(c, 0);
        return;
    }
    set_r3(c, e + 1);
}

/* closedir(handle) */
void f_802E4250(ppc_t* c) { set_r3(c, 0); }

/* FAMkdir(path) */
void f_802E4690(ppc_t* c) {
    char path[128];
    read_path(c->r[3], path, sizeof(path));
    if (find(path) >= 0) set_r3(c, fail(PF_EEXIST));
    else set_r3(c, add(path, 1) >= 0 ? 0 : fail(PF_ENOENT));
    HLE_LOG("SD: mkdir %s = %d", path, (int32_t)c->r[3]);
}

/* FARemove(path): a file, or an empty folder. */
void f_802E4870(ppc_t* c) {
    char path[128];
    int e, i;
    read_path(c->r[3], path, sizeof(path));
    e = find(path);
    if (e < 0 || e == MAX_ENTRIES) {
        set_r3(c, fail(PF_ENOENT));
        return;
    }
    for (i = 0; i < 16; i++)
        if (sOpen[i].entry == e + 1) {
            set_r3(c, fail(PF_EIO));
            return;
        }
    free(sEntries[e].data);
    sEntries[e] = sEntries[--sCount];
    for (i = 0; i < 16; i++)
        if (sOpen[i].entry == sCount + 1) sOpen[i].entry = e + 1;
    set_r3(c, 0);
}

/* FAFstat(path, stat) */
void f_802E4510(ppc_t* c) {
    char path[128];
    uint32_t st = c->r[4];
    int e;
    read_path(c->r[3], path, sizeof(path));
    e = find(path);
    if (e < 0) {
        set_r3(c, fail(PF_ENOENT));
        return;
    }
    ST32(st, e == MAX_ENTRIES ? 0 : sEntries[e].size);
    ST8(st + 0x10, is_dir(e) ? ATTR_DIR : ATTR_FILE);
    set_r3(c, 0);
}

/* The next entry of a search, into the game's; 0, or -1 at the end. */
static int search_next(Search* s) {
    for (; s->next < sCount; s->next++) {
        const Entry* e = &sEntries[s->next];
        char parent[128];
        const char* name;
        split(e->path, parent, sizeof(parent), &name);
        if (!same(parent, s->dir) || !matches(s->pattern, name)) continue;
        /* Folders when asked for (as when the game empties one), files
         * otherwise. */
        if (e->dir != ((s->attr & ATTR_DIR) != 0)) continue;
        {
            char out[256];
            uint32_t i;
            snprintf(out, sizeof(out), "%s", name);
            for (i = 0; i < 256; i++) ST8(s->dirent + DIRENT_NAME + i, (uint8_t)out[i]);
        }
        s->next++;
        return 0;
    }
    s->dirent = 0;
    return fail(PF_ENOENT);
}

/* fsfirst(pattern, attributes, entry) */
void f_802E44B0(ppc_t* c) {
    char path[128];
    const char* pattern;
    Search* s = NULL;
    int i;
    read_path(c->r[3], path, sizeof(path));
    for (i = 0; i < 8 && !s; i++)
        if (sSearches[i].dirent == c->r[5]) s = &sSearches[i];
    for (i = 0; i < 8 && !s; i++)
        if (!sSearches[i].dirent) s = &sSearches[i];
    if (!s) s = &sSearches[0];
    memset(s, 0, sizeof(*s));
    s->dirent = c->r[5];
    s->attr = (int)c->r[4];
    split(path, s->dir, sizeof(s->dir), &pattern);
    snprintf(s->pattern, sizeof(s->pattern), "%s", pattern);
    set_r3(c, search_next(s));
    HLE_LOG("SD: fsfirst %s (0x%X) = %d", path, s->attr, (int32_t)c->r[3]);
}

/* fsnext(entry) */
void f_802E44E0(ppc_t* c) {
    int i;
    for (i = 0; i < 8; i++)
        if (sSearches[i].dirent && sSearches[i].dirent == c->r[3]) {
            set_r3(c, search_next(&sSearches[i]));
            return;
        }
    set_r3(c, fail(PF_ENOENT));
}

/* ------------------------------------------------------------------------ */
/* Files */

static Open* handle(uint32_t h) { return h >= 1 && h <= 16 && sOpen[h - 1].entry ? &sOpen[h - 1] : NULL; }

/* Opens a file (made if create), from the start; a handle or 0. */
static uint32_t open_file(const char* path, int create, int truncate) {
    int e = find(path), i;
    if (e == MAX_ENTRIES || (e >= 0 && sEntries[e].dir)) return (uint32_t)(fail(PF_EIO) + 1);
    if (e < 0) {
        if (!create) return (uint32_t)(fail(PF_ENOENT) + 1);
        if (!have_folder()) return (uint32_t)(fail(PF_EIO) + 1);
        if ((e = add(path, 0)) < 0) return (uint32_t)(fail(PF_ENOENT) + 1);
    }
    for (i = 0; i < 16; i++) {
        if (!sOpen[i].entry) {
            sOpen[i].entry = e + 1;
            sOpen[i].pos = 0;
            sOpen[i].wrote = 0;
            if (truncate) sEntries[e].size = 0;
            return (uint32_t)(i + 1);
        }
    }
    return (uint32_t)(fail(PF_EIO) + 1);
}

/* fopen(path, mode) */
void f_802E4450(ppc_t* c) {
    char path[128], mode[8];
    read_path(c->r[3], path, sizeof(path));
    hle_read_str(c->r[4], mode, sizeof(mode));
    c->r[3] = open_file(path, strchr(mode, 'w') || strchr(mode, 'a'), strchr(mode, 'w') != NULL);
    if (c->r[3] && strchr(mode, 'a')) sOpen[c->r[3] - 1].pos = sEntries[sOpen[c->r[3] - 1].entry - 1].size;
    HLE_LOG("SD: fopen %s \"%s\" = %u", path, mode, c->r[3]);
}

/* FACreate(path, attributes): made empty, and open. */
void f_802E4280(ppc_t* c) {
    char path[128];
    read_path(c->r[3], path, sizeof(path));
    c->r[3] = open_file(path, 1, 1);
    HLE_LOG("SD: create %s = %u", path, c->r[3]);
}

/* fclose(handle): a photo written is saved. */
void f_802E43F0(ppc_t* c) {
    Open* o = handle(c->r[3]);
    Entry* e;
    int ok = 1;
    if (!o) {
        set_r3(c, fail(PF_EIO));
        return;
    }
    e = &sEntries[o->entry - 1];
    if (o->wrote) {
        const char* name = strrchr(e->path, '\\') + 1;
        ok = sChosen > 0 && g_host_sd_save && g_host_sd_save(name, e->data, e->size);
        HLE_LOG("SD: %s (%u bytes) %s", e->path, e->size, ok ? "saved" : "not saved");
        sLastWrite = time(NULL);
    }
    o->entry = 0;
    {
        int i, open = 0;
        for (i = 0; i < 16; i++) open |= sOpen[i].entry == (int)(e - sEntries) + 1;
        if (!open) {
            free(e->data);
            e->data = NULL;
        }
    }
    set_r3(c, ok ? 0 : fail(PF_EIO));
}

/* fgetsize(handle, size*) */
void f_802E4420(ppc_t* c) {
    Open* o = handle(c->r[3]);
    if (!o) {
        set_r3(c, fail(PF_EIO));
        return;
    }
    ST32(c->r[4], sEntries[o->entry - 1].size);
    set_r3(c, 0);
}

/* fseek(handle, offset, origin: 0 the start, 1 here, 2 the end) */
void f_802E4480(ppc_t* c) {
    Open* o = handle(c->r[3]);
    int64_t to;
    if (!o) {
        set_r3(c, fail(PF_EIO));
        return;
    }
    to = (int32_t)c->r[4];
    if (c->r[5] == 1) to += o->pos;
    else if (c->r[5] == 2) to += sEntries[o->entry - 1].size;
    if (to < 0) {
        set_r3(c, fail(PF_EIO));
        return;
    }
    o->pos = (uint32_t)to;
    set_r3(c, 0);
}

/* fwrite(data, size, count, handle): the count written. A file closed
 * before is written over, not added to: its contents are let go. */
void f_802E4540(ppc_t* c) {
    Open* o = handle(c->r[6]);
    Entry* e;
    uint64_t n = (uint64_t)c->r[4] * c->r[5], end;
    uint8_t* grown;
    if (!o || !c->r[4]) {
        set_r3(c, o ? 0 : fail(PF_EIO));
        return;
    }
    e = &sEntries[o->entry - 1];
    end = o->pos + n;
    if (end > 64u << 20 || !(grown = (uint8_t*)realloc(e->data, (size_t)end > e->size ? (size_t)end : e->size + 1))) {
        set_r3(c, fail(PF_EIO));
        return;
    }
    if (!e->data && e->size) memset(grown, 0, e->size); /* (its old contents are gone) */
    e->data = grown;
    if (o->pos > e->size) memset(e->data + e->size, 0, o->pos - e->size);
    memcpy(e->data + o->pos, MA(c->r[3]), (size_t)n);
    o->pos = (uint32_t)end;
    if (end > e->size) e->size = (uint32_t)end;
    o->wrote = 1;
    sLastWrite = time(NULL);
    c->r[3] = c->r[5];
}
