/* Bringing over a save (wii_save.h), on Android: the files chosen in the
 * system's picker (PokeParkActivity's chooseSave), a Wii's data.bin or
 * the files of Dolphin's save folder, read through the file descriptors it
 * hands back, and the game started again (restartGame), as it reads its
 * saves as it starts. The game waits while the picker is up. */
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wii_save.h"

extern struct android_app* g_android_app; /* window_android.c */
void win_pump(void);                       /* window_android.c */

#define MAX_FILES 32
#define MAX_SIZE (16u << 20) /* a save's files are 3 MB at most */

enum { ASK, COUNT, FILES, RESTART };

typedef struct {
    int what, result, count;
    WiiSaveFile* files;
    int fds[MAX_FILES];
} Job;

/* The activity's methods, called on a thread of their own, attached for
 * it: the game's own thread runs on fiber stacks (hle_thread.c), which
 * the Java VM takes for an overflowed stack. */
static void* job_thread(void* p) {
    Job* j = p;
    JavaVM* vm = g_android_app ? g_android_app->activity->vm : NULL;
    jobject act = g_android_app ? g_android_app->activity->clazz : NULL;
    JNIEnv* env;
    jclass cls;
    int i;
    j->result = -1;
    if (!vm || (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    cls = (*env)->GetObjectClass(env, act);
    if (j->what == ASK) {
        jmethodID m = (*env)->GetMethodID(env, cls, "chooseSave", "()V");
        if (m) (*env)->CallVoidMethod(env, act, m), j->result = 0;
    } else if (j->what == COUNT) {
        jmethodID m = (*env)->GetMethodID(env, cls, "takeSaveCount", "()I");
        if (m) j->result = (*env)->CallIntMethod(env, act, m);
    } else if (j->what == FILES) {
        jmethodID name = (*env)->GetMethodID(env, cls, "saveName", "(I)Ljava/lang/String;");
        jmethodID open = (*env)->GetMethodID(env, cls, "openSave", "(I)I");
        for (i = 0; name && open && i < j->count; i++) {
            jstring s = (*env)->CallObjectMethod(env, act, name, i);
            const char* c = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
            snprintf(j->files[i].name, sizeof(j->files[i].name), "%s", c ? c : "");
            if (c) (*env)->ReleaseStringUTFChars(env, s, c);
            if (s) (*env)->DeleteLocalRef(env, s);
            j->fds[i] = (*env)->CallIntMethod(env, act, open, i);
        }
        j->result = 0;
    } else {
        jmethodID m = (*env)->GetMethodID(env, cls, "restartGame", "()V");
        if (m) (*env)->CallVoidMethod(env, act, m);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        j->result = -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*vm)->DetachCurrentThread(vm);
    return NULL;
}

static int run(Job* j) {
    pthread_t t;
    if (pthread_create(&t, NULL, job_thread, j)) return -1;
    pthread_join(t, NULL);
    return j->result;
}

/* All of fd, read, into f's data; 0 if it couldn't be. */
static int load(int fd, WiiSaveFile* f) {
    uint8_t* data = NULL;
    size_t size = 0, cap = 0;
    ssize_t n;
    for (;;) {
        if (size == cap) {
            uint8_t* more;
            if (cap >= MAX_SIZE || !(more = realloc(data, cap = cap ? cap * 2 : 65536))) break;
            data = more;
        }
        n = read(fd, data + size, cap - size);
        if (n <= 0) {
            close(fd);
            if (n < 0) break;
            f->data = data;
            f->size = (uint32_t)size;
            return 1;
        }
        size += (size_t)n;
    }
    close(fd);
    free(data);
    return 0;
}

int save_import(void) {
    struct timespec tick = {0, 100 * 1000 * 1000};
    WiiSaveFile files[MAX_FILES];
    Job j;
    int count, r = WIISAVE_OK, i, loaded = 0;
    memset(&j, 0, sizeof(j));
    j.what = ASK;
    if (run(&j) < 0) return WIISAVE_UNREADABLE;
    /* The picker covers the game, so its window goes, and the activity
     * waits for the game to let go of it (win_pump): let go here too. */
    j.what = COUNT;
    while ((count = run(&j)) < 0) {
        win_pump();
        nanosleep(&tick, NULL);
    }
    if (count == 0) return -1;
    if (count > MAX_FILES) return WIISAVE_NOT_A_SAVE;
    memset(files, 0, sizeof(files));
    j.what = FILES;
    j.count = count;
    j.files = files;
    for (i = 0; i < MAX_FILES; i++) j.fds[i] = -1;
    run(&j);
    for (i = 0; i < count; i++) {
        if (j.fds[i] < 0 || !load(j.fds[i], &files[i])) r = WIISAVE_UNREADABLE;
        else loaded++;
    }
    if (r == WIISAVE_OK) r = wiisave_import_files(hle_nand_root(), files, count);
    for (i = 0; i < count; i++) free((void*)files[i].data);
    fprintf(stderr, "save import: %s (%d files)\n", wiisave_message(r), loaded);
    if (r == WIISAVE_OK) {
        j.what = RESTART;
        run(&j);
    }
    return r;
}
