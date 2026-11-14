/* The game read straight from the player's disc image (RVZ, ISO, WBFS,
 * CISO, WIA, ...), which Android's file picker hands over as a file
 * descriptor (window_android.c), so nothing is extracted. nod (Luke
 * Street's disc library, built by tools/recomp/build.py; recomp/assets/
 * nod_license.txt) opens the image and reads its data partition,
 * decrypted and decompressed, at the offsets the game's DVD reads ask for
 * (hle_dvd.c); main.dol, boot.bin, the FST and the TMD come from the
 * partition's header. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disc_verify.h"
#include "nod.h"
#include "../patches/hle.h"

static NodHandle* sDisc;
static NodHandle* sPartition;
static NodPartitionMeta sMeta;

static int64_t stream_read_at(void* user, uint64_t offset, void* out, size_t len) {
    ssize_t n = pread((int)(intptr_t)user, out, len, (off_t)offset);
    return n < 0 ? -1 : (int64_t)n;
}

static int64_t stream_len(void* user) {
    struct stat st;
    return fstat((int)(intptr_t)user, &st) == 0 ? (int64_t)st.st_size : -1;
}

static void stream_close(void* user) { close((int)(intptr_t)user); }

static void close_image(void) {
    nod_free(sPartition);
    nod_free(sDisc);
    sPartition = sDisc = NULL;
}

/* The image file's bytes, for disc_check_file. */
static int file_read(void* user, uint64_t offset, void* out, size_t len) {
    return pread((int)(intptr_t)user, out, len, (off_t)offset) == (ssize_t)len;
}

/* Opens the image in fd (taking it over); 0, or 1 with what's wrong in err. */
int disc_image_open(int fd, char* err, size_t n) {
    NodDiscStream stream = {(void*)(intptr_t)fd, stream_read_at, stream_len, stream_close};
    NodDiscOptions options = {0};
    NodDiscHeader header;
    const char* why;
    struct stat st;
    close_image();
    /* Copied part way, or an NKit ISO (disc_verify.h). */
    if (fstat(fd, &st) == 0 && disc_check_file(file_read, (void*)(intptr_t)fd, (uint64_t)st.st_size, err, n) != DISC_OK) {
        close(fd);
        return 1;
    }
    memset(&header, 0, sizeof(header));
    options.preloader_threads = 1;
    if (nod_disc_open_stream(&stream, &options, &sDisc) != NOD_RESULT_OK) {
        why = nod_error_message();
        snprintf(err, n, "That file isn't a disc image this can read%s%s.", why ? ": " : "", why ? why : "");
        return 1;
    }
    if (nod_disc_header(sDisc, &header) != NOD_RESULT_OK ||
        disc_check_id(header.game_id, header.disc_version, err, n) != DISC_OK) {
        close_image();
        return 1;
    }
    if (nod_disc_open_partition_kind(sDisc, 0, NULL, &sPartition) != NOD_RESULT_OK ||
        nod_partition_meta(sPartition, &sMeta) != NOD_RESULT_OK || !sMeta.raw_dol.data || !sMeta.raw_fst.data ||
        !sMeta.raw_boot.data || sMeta.raw_boot.size < 0x440) {
        why = nod_error_message();
        snprintf(err, n, "The disc image's game data couldn't be read%s%s.", why ? ": " : "", why ? why : "");
        close_image();
        return 1;
    }
    if (disc_check_dol(sMeta.raw_dol.data, sMeta.raw_dol.size, err, n) != DISC_OK) {
        close_image();
        return 1;
    }
    fprintf(stderr, "PokePark: disc image %.6s, version %d\n", header.game_id, header.disc_version);
    return 0;
}

int disc_image_active(void) { return sPartition != NULL; }

const uint8_t* disc_image_dol(size_t* len) {
    *len = sMeta.raw_dol.size;
    return sMeta.raw_dol.data;
}

/* The game's DVD reads, on its thread: the partition at offset. */
static int read_partition(uint64_t offset, void* out, uint32_t len) {
    uint8_t* p = (uint8_t*)out;
    if (nod_seek(sPartition, (int64_t)offset, 0) != (int64_t)offset) return 0;
    while (len) {
        int64_t got = nod_read(sPartition, p, len);
        if (got <= 0) return 0;
        p += got;
        len -= (uint32_t)got;
    }
    return 1;
}

int disc_image_dvd_init(void) {
    return hle_dvd_init_image(sMeta.raw_boot.data, sMeta.raw_fst.data, (uint32_t)sMeta.raw_fst.size,
                              sMeta.raw_tmd.data, (uint32_t)sMeta.raw_tmd.size, read_partition);
}
