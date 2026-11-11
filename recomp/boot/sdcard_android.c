/* Where the photos copied to the SD card go (host_sdcard.h): a folder
 * chosen in the system's folder picker, which the activity asks for and
 * keeps lasting permission to (PokeParkActivity's choosePhotoFolder), and
 * each photo a new file there (createPhoto), written through the file
 * descriptor it hands back. The game waits while the picker is up.
 *
 * Environment (env.txt):
 *   RECOMP_SD_DIR=<dir>   save there without asking (for test runs) */
#include <android_native_app_glue.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "host_sdcard.h"

extern struct android_app* g_android_app; /* window_android.c */
void win_pump(void);                       /* window_android.c */

static const char* sDir; /* RECOMP_SD_DIR */

typedef struct {
    const char *name, *sig, *arg;
    int result;
} Call;

/* The activity's method name, with the signature sig, called; its int
 * result, or -1. On a thread of its own (call()), attached for it. */
static void* call_thread(void* p) {
    Call* k = p;
    JavaVM* vm = g_android_app ? g_android_app->activity->vm : NULL;
    jobject act = g_android_app ? g_android_app->activity->clazz : NULL;
    const char *name = k->name, *sig = k->sig, *arg = k->arg;
    JNIEnv* env;
    jclass cls;
    jmethodID m;
    int r = -1;
    k->result = -1;
    if (!vm || (*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    cls = (*env)->GetObjectClass(env, act);
    m = (*env)->GetMethodID(env, cls, name, sig);
    if (m && arg) {
        jstring s = (*env)->NewStringUTF(env, arg);
        r = (*env)->CallIntMethod(env, act, m, s);
        (*env)->DeleteLocalRef(env, s);
    } else if (m && sig[2] == 'V') {
        (*env)->CallVoidMethod(env, act, m);
        r = 0;
    } else if (m) {
        r = (*env)->CallIntMethod(env, act, m);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        r = -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    (*vm)->DetachCurrentThread(vm);
    k->result = r;
    return NULL;
}

/* The game asks on its own thread's fiber stacks (hle_thread.c), which
 * the Java VM takes for an overflowed stack (StackOverflowError), so the
 * call is made on a thread of its own, and waited for. */
static int call(const char* name, const char* sig, const char* arg) {
    Call k = {name, sig, arg, -1};
    pthread_t t;
    if (pthread_create(&t, NULL, call_thread, &k)) return -1;
    pthread_join(t, NULL);
    return k.result;
}

static int choose(void) {
    struct timespec tick = {0, 100 * 1000 * 1000};
    int r;
    sDir = getenv("RECOMP_SD_DIR");
    if (sDir && *sDir) {
        mkdir(sDir, 0770);
        return 1;
    }
    sDir = NULL;
    if (call("choosePhotoFolder", "()V", NULL) < 0) return 0;
    /* The picker covers the game, so its window goes, and the activity
     * waits for the game to let go of it (win_pump, at a frame's end):
     * let go here too, or neither the activity nor the picker's answer
     * gets any further. */
    while ((r = call("takePhotoFolder", "()I", NULL)) < 0) {
        win_pump();
        nanosleep(&tick, NULL);
    }
    return r == 1;
}

static int write_all(int fd, const uint8_t* data, uint32_t size) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        data += n;
        size -= (uint32_t)n;
    }
    return 1;
}

static int save(const char* name, const uint8_t* data, uint32_t size) {
    char file[96], *p;
    int fd, ok;
    /* A lower-case extension, which the system's picker knows a JPEG by. */
    snprintf(file, sizeof(file), "%s", name);
    if ((p = strrchr(file, '.')) != NULL)
        for (; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (sDir) {
        char path[1024];
        const char* ext = strrchr(file, '.') ? strrchr(file, '.') : file + strlen(file);
        int i;
        for (i = 0, fd = -1; i < 10000 && fd < 0; i++) {
            if (!i) snprintf(path, sizeof(path), "%s/%s", sDir, file);
            else snprintf(path, sizeof(path), "%s/%.*s (%d)%s", sDir, (int)(ext - file), file, i + 1, ext);
            fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0660);
            if (fd < 0 && errno != EEXIST) break;
        }
    } else {
        fd = call("createPhoto", "(Ljava/lang/String;)I", file);
    }
    if (fd < 0) return 0;
    ok = write_all(fd, data, size);
    close(fd);
    fprintf(stderr, "sd: %s %s\n", ok ? "saved" : "couldn't save", file);
    return ok;
}

void sdcard_init(void) {
    g_host_sd_choose = choose;
    g_host_sd_save = save;
}
