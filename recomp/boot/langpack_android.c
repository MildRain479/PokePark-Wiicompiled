/* Making a language pack (langpack.h) on Android: the disc image chosen in
 * the system's picker (PokeParkActivity's chooseLanguageDisc) is read
 * straight from the file descriptor it hands back, by nod (as
 * disc_image_android.c reads the game), beside the USA game: its image
 * opened again (savedGameFd), or its extracted files. On a thread of its
 * own, the game going on. */
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "host_settings.h"
#include "langpack.h"
#include "nod.h"

extern struct android_app* g_android_app; /* window_android.c */
void win_pump(void);                       /* window_android.c */
const char* hle_dvd_root(void);            /* hle_dvd.c */

typedef struct {
    const char* method;
    int result;
} Call;

/* The activity's method, taking nothing and giving an int (0 for a void
 * one), on a thread attached for it; -1 if it couldn't be called. */
static void* call_thread(void* p) {
    Call* c = (Call*)p;
    JavaVM* vm = g_android_app ? g_android_app->activity->vm : NULL;
    jobject act = g_android_app ? g_android_app->activity->clazz : NULL;
    JNIEnv* env;
    jclass cls;
    jmethodID m;
    int is_void = !strcmp(c->method, "chooseLanguageDisc");
    c->result = -1;
    if (!vm || (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, c->method, is_void ? "()V" : "()I");
    if (m && is_void) (*env)->CallVoidMethod(env, act, m), c->result = 0;
    else if (m) c->result = (*env)->CallIntMethod(env, act, m);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        c->result = -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}

static int call(const char* method) {
    Call c = {method, -1};
    pthread_t t;
    if (pthread_create(&t, NULL, call_thread, &c)) return -1;
    pthread_join(t, NULL);
    return c.result;
}

/* ------------------------------------------------------------------------ */
/* A disc image, through nod */

typedef struct {
    NodHandle *disc, *partition;
    NodPartitionMeta meta;
} Image;

static int64_t stream_read_at(void* user, uint64_t offset, void* out, size_t len) {
    ssize_t n = pread((int)(intptr_t)user, out, len, (off_t)offset);
    return n < 0 ? -1 : (int64_t)n;
}

static int64_t stream_len(void* user) {
    struct stat st;
    return fstat((int)(intptr_t)user, &st) == 0 ? (int64_t)st.st_size : -1;
}

static void stream_close(void* user) { close((int)(intptr_t)user); }

static int image_read(void* user, const char* path, uint64_t offset, uint32_t pos, void* out, uint32_t size) {
    Image* im = (Image*)user;
    uint8_t* p = (uint8_t*)out;
    int64_t at = (int64_t)(offset + pos);
    (void)path;
    if (nod_seek(im->partition, at, 0) != at) return 0;
    while (size) {
        int64_t got = nod_read(im->partition, p, size);
        if (got <= 0) return 0;
        p += got;
        size -= (uint32_t)got;
    }
    return 1;
}

static void image_close(Image* im) {
    if (!im) return;
    nod_free(im->partition);
    nod_free(im->disc);
    free(im);
}

/* The image in fd (taken over) as a disc (its ID in id); NULL if it can't
 * be read. */
static Image* image_open(int fd, LangDisc* d, char id[7]) {
    NodDiscStream stream = {(void*)(intptr_t)fd, stream_read_at, stream_len, stream_close};
    NodDiscOptions options;
    NodDiscHeader header;
    Image* im = (Image*)calloc(1, sizeof(Image));
    memset(&options, 0, sizeof(options));
    options.preloader_threads = 1;
    if (!im) {
        close(fd);
        return NULL;
    }
    if (nod_disc_open_stream(&stream, &options, &im->disc) != NOD_RESULT_OK) {
        free(im);
        return NULL;
    }
    memset(&header, 0, sizeof(header));
    if (nod_disc_header(im->disc, &header) != NOD_RESULT_OK ||
        nod_disc_open_partition_kind(im->disc, 0, NULL, &im->partition) != NOD_RESULT_OK ||
        nod_partition_meta(im->partition, &im->meta) != NOD_RESULT_OK || !im->meta.raw_fst.data) {
        image_close(im);
        return NULL;
    }
    memcpy(id, header.game_id, 6);
    id[6] = '\0';
    memset(d, 0, sizeof(*d));
    d->fst = im->meta.raw_fst.data;
    d->fst_size = (uint32_t)im->meta.raw_fst.size;
    d->read = image_read;
    d->user = im;
    return im;
}

/* ------------------------------------------------------------------------ */

static int sFd = -1;

static void work(void* arg) {
    LangDisc usa, other;
    Image *theirs, *ours = NULL;
    char id[7] = "", our_id[7];
    int lang, fd;
    (void)arg;
    if (!(theirs = image_open(sFd, &other, id))) {
        langpack_fail("That disc image couldn't be read. Is the file complete?");
        return;
    }
    if ((lang = langpack_language_of(id)) < 0) {
        char m[160];
        snprintf(m, sizeof(m), "%s is not a Japanese disc (R8AJ01)", id);
        langpack_fail(m);
        image_close(theirs);
        return;
    }
    /* The USA game: its extracted files, or its image opened again (the
     * game's own reads go on through the first). */
    if (hle_dvd_root()[0]) {
        if (!langdisc_folder(&usa, hle_dvd_root())) {
            langpack_fail("The game's own files couldn't be read");
            image_close(theirs);
            return;
        }
    } else if ((fd = call("savedGameFd")) < 0 || !(ours = image_open(fd, &usa, our_id))) {
        langpack_fail("The game's own disc image couldn't be opened again");
        image_close(theirs);
        return;
    }
    langpack_run(lang, &usa, &other);
    if (ours) image_close(ours);
    else langdisc_free(&usa);
    image_close(theirs);
}

void langpack_choose(void) {
    struct timespec tick = {0, 100 * 1000 * 1000};
    int fd;
    const char* test = getenv("RECOMP_LANGPACK_IMAGE"); /* tests: the image, not the picker */
    if (langpack_state(NULL, NULL) == LANGPACK_RUNNING) return;
    if (test && *test) {
        if ((fd = open(test, O_RDONLY)) < 0) {
            langpack_fail("That disc image couldn't be opened");
            return;
        }
        sFd = fd;
        langpack_set_running("Reading the disc image");
        if (langpack_thread(work, NULL) != 0) langpack_fail("Couldn't start making the pack");
        return;
    }
    if (call("chooseLanguageDisc") < 0) return;
    /* As save_import: the picker covers the game. */
    while ((fd = call("takeLanguageFd")) == -2) {
        win_pump();
        nanosleep(&tick, NULL);
    }
    if (fd < 0) return;
    sFd = fd;
    langpack_set_running("Reading the disc image");
    if (langpack_thread(work, NULL) != 0) {
        close(fd);
        langpack_fail("Couldn't start making the pack");
    }
}
