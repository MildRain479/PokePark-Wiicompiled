/* A resource's entry by name, found natively.
 *
 * Entering an area, the scene's scripts look up hundreds of their
 * resources by name in tables of {length, name, offset} entries: two
 * copies of one function (fn_80195F70 and fn_80196200, for two kinds of
 * resource), each walking the table from the start and calling the game's
 * strcmp on every entry. This finds the same entry natively (the game's
 * strcmp is used only for equal or not), as PokePark 2's port does for
 * the same functions. On an AYN Thor, entering the Magma Zone, the
 * slowest frame went from 102 ms to 76.
 *
 * In: r3 the table's header, whose first word is the offset of the table;
 * r4 the name. The table: a 16-bit count at +4, then from +8 entries of a
 * 16-bit length, that many bytes of name (NUL-terminated within), a 32-bit
 * offset (from the table) and 4 more bytes. Out: r3 the table plus the
 * first matching entry's offset, or 0. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The walk: the first entry named want, or 0. */
static uint32_t walk(uint32_t table, const char* want) {
    uint32_t count = LD16(table + 4), i;
    const uint8_t* p = MA(table + 8);
    for (i = 0; i < count; i++) {
        uint32_t len = (uint32_t)p[0] << 8 | p[1];
        const char* have = (const char*)p + 2;
        p += 2 + len;
        if (have[0] == want[0] && strcmp(have, want) == 0) return table + be32(p);
        p += 8;
    }
    return 0;
}

/* Tables of more than a few entries, indexed: an area's scripts make half
 * a million lookups as they start (in PokePark 2, where they were
 * counted), 150,000 in tables of up to hundreds. Each table's index keeps
 * a copy of the table's bytes, compared with the table at the first
 * lookup of each retrace (unchanged bytes are an unchanged table; it is
 * indexed again if they differ). Between, each answer is checked in the
 * table itself: a name found must be the entry's there (its offset read
 * there too). A name not found (a third of the lookups: the scripts look
 * in one table, then another) is taken as not there while the table's
 * count and its first and last 64 bytes are the copy's; otherwise the
 * table is walked, and indexed again if the name is there. */
#define TABLES 64
#define SMALL 16 /* entries: walked, as quick as a lookup */
typedef struct {
    uint32_t table, size, slots, count, checked;
    uint8_t* copy;
    uint32_t* entry; /* by hash: the entry's offset from the table + 1 (0: empty) */
    uint32_t* pos;   /* by number: the entry's offset from the table */
} Index;
static Index sIndex[TABLES];
extern uint32_t g_retrace_count; /* hle_irq.c */

static uint32_t hash_name(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static Index* index_table(uint32_t table, int again) {
    Index* x = &sIndex[(table >> 2) * 2654435761u >> 26]; /* one place per table */
    uint32_t count = LD16(table + 4), p, i;
    if (!again && x->table == table && x->copy && x->count == count) {
        if (x->checked == g_retrace_count) return x;
        if (memcmp(x->copy, MA(table), x->size) == 0) {
            x->checked = g_retrace_count;
            return x;
        }
    }
    for (p = table + 8, i = 0; i < count; i++) p += 2 + LD16(p) + 8;
    free(x->copy);
    free(x->entry);
    free(x->pos);
    x->table = table;
    x->count = count;
    x->checked = g_retrace_count;
    x->size = p - table;
    for (x->slots = 16; x->slots < count * 2; x->slots *= 2) {}
    x->copy = (uint8_t*)malloc(x->size);
    x->entry = (uint32_t*)calloc(x->slots, 4);
    x->pos = (uint32_t*)malloc(count * 4 + 4);
    if (!x->copy || !x->entry || !x->pos) {
        free(x->copy);
        free(x->entry);
        free(x->pos);
        memset(x, 0, sizeof(*x));
        return NULL;
    }
    memcpy(x->copy, MA(table), x->size);
    for (p = 8, i = 0; i < count; i++) {
        const char* name = (const char*)x->copy + p + 2;
        uint32_t slot = hash_name(name) & (x->slots - 1), e;
        x->pos[i] = p;
        while ((e = x->entry[slot]) && strcmp((const char*)x->copy + e - 1 + 2, name) != 0) slot = (slot + 1) & (x->slots - 1);
        if (!e) x->entry[slot] = p + 1; /* (the first one named so is kept) */
        p += 2 + ((uint32_t)x->copy[p] << 8 | x->copy[p + 1]) + 8;
    }
    return x;
}

/* Whether the table's first and last 64 bytes are still the copy's. */
static int same_ends(const Index* x, uint32_t table) {
    uint32_t n = x->size < 64 ? x->size : 64;
    return memcmp(x->copy, MA(table), n) == 0 && memcmp(x->copy + x->size - n, MA(table + x->size - n), n) == 0;
}

static uint32_t find_by_name(uint32_t header, uint32_t name) {
    uint32_t table = header + LD32(header), slot, e, found;
    const char* want = (const char*)MA(name);
    Index* x = LD16(table + 4) <= SMALL ? NULL : index_table(table, 0);
    if (!x) return walk(table, want);
    for (slot = hash_name(want) & (x->slots - 1); (e = x->entry[slot]); slot = (slot + 1) & (x->slots - 1)) {
        const uint8_t* at = x->copy + e - 1;
        if (strcmp((const char*)at + 2, want) == 0) {
            const uint8_t* live = MA(table + e - 1);
            uint32_t len = (uint32_t)live[0] << 8 | live[1];
            if (len == ((uint32_t)at[0] << 8 | at[1]) && strcmp((const char*)live + 2, want) == 0) return table + be32(live + 2 + len);
            break; /* the table changed */
        }
    }
    if (!e && same_ends(x, table)) return 0;
    found = walk(table, want);
    if (found || e) index_table(table, 1);
    return found;
}

/* The table's entry number n (fn_80196030, walking from the start too):
 * found by the index's list of where each entry is, while the table's
 * count and ends are the copy's, the offset read in the table itself. */
static uint32_t find_by_number(uint32_t header, uint32_t n) {
    uint32_t table = header + LD32(header), count = LD16(table + 4), at, i;
    Index* x;
    if (n >= count) return 0;
    x = count <= SMALL ? NULL : index_table(table, 0);
    if (x && !same_ends(x, table)) x = index_table(table, 1);
    if (x) {
        at = table + x->pos[n];
    } else {
        for (at = table + 8, i = 0; i < n; i++) at += 2 + LD16(at) + 8;
    }
    return table + LD32(at + 2 + LD16(at));
}

void f_80195F70(ppc_t* c) { c->r[3] = find_by_name(c->r[3], c->r[4]); }
void f_80196200(ppc_t* c) { c->r[3] = find_by_name(c->r[3], c->r[4]); }
void f_80196030(ppc_t* c) { c->r[3] = find_by_number(c->r[3], c->r[4]); }
