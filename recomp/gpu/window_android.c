/* Android: the app (a NativeActivity, through native_app_glue), its window,
 * and the thread the game runs on.
 *
 * The activity's own thread (android_main) takes the app's events: the
 * window coming and going, input (input_android.c), the display's refresh
 * rate. The game runs on a thread of its own, as on Windows it runs on
 * the program's main thread, and draws in the window with Vulkan (no
 * OpenGL ES device).
 *
 * When the app goes to the background its window goes: the activity's
 * thread waits while the game, at its next frame (win_pump), lets go of
 * the window (vk_window_lost) and stops, with the time base held, until a
 * window comes back.
 *
 * The game is the player's own: a disc image, which the activity's Java
 * side asks for with Android's file picker and remembers, read in place
 * (disc_image_android.c); or, when it's there, the game extracted as on
 * Windows in the app's folder on shared storage (Android/data/<package>/
 * files/R8AE01, with sys/main.dol and disc; adb push puts it there). Saves
 * (nand) go in R8AE01/nand there either way; settings.ini in the app's
 * private files. stdout and stderr go to logcat, tagged PokePark. */
#define _GNU_SOURCE /* cpu_set_t */
#include <android/choreographer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gx_device.h"
#include "host_settings.h"
#include "ppc.h"

int boot_main(int argc, char** argv); /* boot.c */
void vk_window_lost(void);           /* vk_backend.c */
void vk_window_back(void);
int input_android_event(AInputEvent* e); /* input_android.c */
void audio_android_pause(int paused);    /* audio_android.c */
int hle_widescreen(void);
void second_screen_start(JavaVM* vm, jobject activity); /* second_screen_android.c */
extern struct AAssetManager* g_android_assets; /* prompts.c */

struct android_app* g_android_app;

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sChanged = PTHREAD_COND_INITIALIZER;
static ANativeWindow* sWindow;
static int sLeaving;  /* the activity's thread waits for the game to let go of the window */
static int sReleased; /* and the game has */
static double sHz;
static double sPixelAspect = 1;

/* ------------------------------------------------------------------------ */
/* stdout and stderr to logcat */

static int sLogPipe[2];

static void* log_thread(void* arg) {
    char buf[1024], line[1024];
    size_t used = 0;
    ssize_t n;
    (void)arg;
    while ((n = read(sLogPipe[0], buf, sizeof(buf))) > 0) {
        ssize_t i;
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || used == sizeof(line) - 1) {
                line[used] = 0;
                __android_log_write(ANDROID_LOG_INFO, "PokePark", line);
                used = 0;
            }
            if (buf[i] != '\n') line[used++] = buf[i];
        }
    }
    return NULL;
}

static void log_to_logcat(void) {
    pthread_t t;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pipe(sLogPipe)) return;
    dup2(sLogPipe[1], 1);
    dup2(sLogPipe[1], 2);
    pthread_create(&t, NULL, log_thread, NULL);
    pthread_detach(t);
}

/* ------------------------------------------------------------------------ */
/* Keeping the game's thread fast */

/* The game's thread sleeps part of every frame, waiting for the next
 * retrace, so Android took it for light work: it kept it on the middle
 * cores at half their clock, and a frame that takes 7 ms on a PC took
 * over 33 ms, half the game's frame rate. So the thread keeps to the
 * fastest cores, and tells Android (its performance hints, ADPF, on
 * Android 13 and later) how long each frame's work took against a target,
 * which raises the clocks to meet it. The target is under a 60 fps frame:
 * Android also lowers them until the work just fits, and with no room left
 * a frame every so often missed its retrace, for as long as that lasted. */
typedef struct APerformanceHintSession APerformanceHintSession;
static APerformanceHintSession* sHint;
static int (*sReportWork)(APerformanceHintSession* session, int64_t ns);

#define WORK_TARGET_NS 14000000

static cpu_set_t sFastCores;
static int sHaveFastCores;

/* The fastest cores alone (the prime ones): on a Snapdragon 8 Gen 2 the
 * next fastest run at 88% of the prime core's clock but do far less work
 * per clock. */
static void keep_to_fast_cores(void) {
    cpu_set_t set;
    long max[CPU_SETSIZE], top = 0;
    int n = (int)sysconf(_SC_NPROCESSORS_CONF), i, count = 0;
    if (n > CPU_SETSIZE) n = CPU_SETSIZE;
    for (i = 0; i < n; i++) {
        char path[96];
        FILE* f;
        max[i] = 0;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if ((f = fopen(path, "r"))) {
            if (fscanf(f, "%ld", &max[i]) != 1) max[i] = 0;
            fclose(f);
        }
        if (max[i] > top) top = max[i];
    }
    if (!top) return;
    CPU_ZERO(&set);
    for (i = 0; i < n; i++) {
        if (max[i] == top) {
            CPU_SET(i, &set);
            count++;
        }
    }
    sFastCores = set;
    sHaveFastCores = 1;
    if (sched_setaffinity(0, sizeof(set), &set) == 0)
        fprintf(stderr, "PokePark: game thread on the %d fastest cores (%ld MHz)\n", count, top / 1000);
}

static void start_hints(void) {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    void* (*getManager)(void);
    APerformanceHintSession* (*createSession)(void*, const int32_t*, size_t, int64_t);
    int32_t tid = gettid();
    void* manager;
    if (!lib) return;
    getManager = (void* (*)(void))dlsym(lib, "APerformanceHint_getManager");
    createSession = (APerformanceHintSession * (*)(void*, const int32_t*, size_t, int64_t))
        dlsym(lib, "APerformanceHint_createSession");
    sReportWork = (int (*)(APerformanceHintSession*, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    if (!getManager || !createSession || !sReportWork || !(manager = getManager())) return;
    sHint = createSession(manager, &tid, 1, WORK_TARGET_NS);
    fprintf(stderr, "PokePark: performance hints %s\n", sHint ? "on" : "unavailable");
}

/* At each of the game's frames, on its thread (gx_frames.c): the time
 * since the last one, less the time spent waiting for events. */
void android_frame_done(void) {
    extern uint64_t g_wait_ticks; /* hle_irq.c */
    static int started;
    static uint64_t last, lastWait;
    static unsigned frames;
    uint64_t now = ppc_timebase();
    /* The system undoes this for the first 20 s or so of a game, and left
     * alone the thread then wanders onto cores that are clocked down. */
    if (sHaveFastCores && ++frames % 30 == 0) {
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof(set), &set) == 0 && !CPU_EQUAL(&set, &sFastCores))
            sched_setaffinity(0, sizeof(sFastCores), &sFastCores);
    }
    if (!started) {
        started = 1;
        keep_to_fast_cores();
        start_hints();
    } else if (sHint) {
        int64_t work = (int64_t)((now - last) - (g_wait_ticks - lastWait));
        if (work > 0) sReportWork(sHint, work * 1000000000 / (int64_t)PPC_TB_HZ);
    }
    last = now;
    lastWait = g_wait_ticks;
}

/* ------------------------------------------------------------------------ */
/* The window, as gx_device.h has it */

void win_create(void) {
    if (hle_widescreen()) sPixelAspect = 16.0 / 9.0 * 456.0 / 640.0;
}

void* win_handle(void) { return sWindow; }
int win_hidden(void) { return 0; }
double win_pixel_aspect(void) { return sPixelAspect; }

void win_size(int* w, int* h) {
    *w = sWindow ? ANativeWindow_getWidth(sWindow) : 0;
    *h = sWindow ? ANativeWindow_getHeight(sWindow) : 0;
    if (*w < 0 || *h < 0) *w = *h = 0;
}

/* The game always fills the screen. */
void win_set_fullscreen(int on) { (void)on; }
void win_set_title(const char* title) { (void)title; }
double win_display_hz(void) { return sHz; }
int gxb_display_hz(void) { return (int)(sHz + 0.5); }

/* Between frames, on the game's thread: if the window is going, let it
 * go and wait for the next. */
void win_pump(void) {
    pthread_mutex_lock(&sLock);
    if (sLeaving) {
        vk_window_lost();
        ppc_timebase_pause(1);
        audio_android_pause(1);
        sReleased = 1;
        pthread_cond_broadcast(&sChanged);
        while (sLeaving || !sWindow) pthread_cond_wait(&sChanged, &sLock);
        sReleased = 0;
        vk_window_back();
        audio_android_pause(0);
        ppc_timebase_pause(0);
    }
    pthread_mutex_unlock(&sLock);
}

/* ------------------------------------------------------------------------ */
/* The activity */

static pthread_t sGame;
static int sGameStarted, sHaveGame;
static char sDol[1024];

static void* game_thread(void* arg) {
    char* argv[2] = {"pokepark", sDol};
    (void)arg;
    exit(boot_main(2, argv));
    return NULL;
}

static void start_game(void) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    pthread_create(&sGame, &a, game_thread, NULL);
    sGameStarted = 1;
}

static void on_command(struct android_app* app, int32_t cmd) {
    fprintf(stderr, "PokePark: app event %d, window %p\n", (int)cmd, (void*)app->window);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        pthread_mutex_lock(&sLock);
        sWindow = app->window;
        sLeaving = 0;
        pthread_cond_broadcast(&sChanged);
        pthread_mutex_unlock(&sLock);
        if (!sGameStarted && sHaveGame && sWindow) start_game();
        break;
    case APP_CMD_TERM_WINDOW:
        pthread_mutex_lock(&sLock);
        if (sGameStarted) {
            sLeaving = 1;
            while (!sReleased) pthread_cond_wait(&sChanged, &sLock);
        }
        sWindow = NULL;
        pthread_mutex_unlock(&sLock);
        break;
    case APP_CMD_DESTROY: exit(0);
    }
}

/* The system bars (status, and navigation or the gesture bar) hidden, the
 * picture under where they were, and a swipe from the edge showing them
 * for a moment. Only the activity's UI thread may do this, so it's done
 * in the activity's focus callback (native_app_glue's, which runs there),
 * whenever the app gets focus back, as Android shows the bars again. */
static void (*sGlueFocus)(ANativeActivity* activity, int focused);

static void hide_system_bars(ANativeActivity* activity) {
    JNIEnv* env = activity->env;
    jclass actClass = (*env)->GetObjectClass(env, activity->clazz);
    jobject window = (*env)->CallObjectMethod(
        env, activity->clazz, (*env)->GetMethodID(env, actClass, "getWindow", "()Landroid/view/Window;"));
    if (window && !(*env)->ExceptionCheck(env)) {
        jclass winClass = (*env)->GetObjectClass(env, window);
        jclass ctlClass = (*env)->FindClass(env, "android/view/WindowInsetsController");
        jclass typeClass = (*env)->FindClass(env, "android/view/WindowInsets$Type");
        jclass lpClass = (*env)->FindClass(env, "android/view/WindowManager$LayoutParams");
        jobject ctl, lp;
        (*env)->CallVoidMethod(env, window, (*env)->GetMethodID(env, winClass, "setDecorFitsSystemWindows", "(Z)V"),
                               JNI_FALSE);
        /* Under a display cutout too (LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES). */
        lp = (*env)->CallObjectMethod(
            env, window, (*env)->GetMethodID(env, winClass, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;"));
        if (lp) {
            jfieldID mode = (*env)->GetFieldID(env, lpClass, "layoutInDisplayCutoutMode", "I");
            if ((*env)->GetIntField(env, lp, mode) != 1) {
                (*env)->SetIntField(env, lp, mode, 1);
                (*env)->CallVoidMethod(
                    env, window,
                    (*env)->GetMethodID(env, winClass, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V"), lp);
            }
            (*env)->DeleteLocalRef(env, lp);
        }
        ctl = (*env)->CallObjectMethod(
            env, window, (*env)->GetMethodID(env, winClass, "getInsetsController", "()Landroid/view/WindowInsetsController;"));
        if (ctl) {
            jint bars = (*env)->CallStaticIntMethod(env, typeClass,
                                                    (*env)->GetStaticMethodID(env, typeClass, "systemBars", "()I"));
            /* BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE */
            (*env)->CallVoidMethod(env, ctl, (*env)->GetMethodID(env, ctlClass, "setSystemBarsBehavior", "(I)V"), 2);
            (*env)->CallVoidMethod(env, ctl, (*env)->GetMethodID(env, ctlClass, "hide", "(I)V"), bars);
            (*env)->DeleteLocalRef(env, ctl);
        }
        (*env)->DeleteLocalRef(env, lpClass);
        (*env)->DeleteLocalRef(env, typeClass);
        (*env)->DeleteLocalRef(env, ctlClass);
        (*env)->DeleteLocalRef(env, winClass);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    if (window) (*env)->DeleteLocalRef(env, window);
    (*env)->DeleteLocalRef(env, actClass);
}

static void on_focus(ANativeActivity* activity, int focused) {
    if (focused) hide_system_bars(activity);
    sGlueFocus(activity, focused);
}

static int32_t on_input(struct android_app* app, AInputEvent* e) {
    (void)app;
    return input_android_event(e);
}

static void on_refresh_rate(int64_t vsyncPeriodNanos, void* data) {
    (void)data;
    if (vsyncPeriodNanos > 0) sHz = 1e9 / (double)vsyncPeriodNanos;
}

/* The game's main.dol, in R8AE01 (or R8AE01/disc, as dtk extracts a
 * disc) in the app's shared folder. */
static int find_game(const char* dir) {
    static const char* const kPlaces[] = {"R8AE01/sys/main.dol", "R8AE01/disc/sys/main.dol"};
    struct stat st;
    size_t i;
    for (i = 0; i < sizeof(kPlaces) / sizeof(kPlaces[0]); i++) {
        snprintf(sDol, sizeof(sDol), "%s/%s", dir, kPlaces[i]);
        if (stat(sDol, &st) == 0) return 1;
    }
    return 0;
}

/* RECOMP_* settings for test runs, which Android has no other way to
 * give: env.txt in the shared folder, a NAME=value a line, then
 * env_test.txt, for a second test run's own. */
static void read_env_file(const char* dir, const char* name) {
    static char line[16384]; /* (a long RECOMP_PAD_SCRIPT, as a loop of many runs is) */
    char path[1024];
    FILE* f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        line[strcspn(line, "\r\n")] = 0;
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "PokePark: %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

static void read_env(const char* dir) {
    read_env_file(dir, "env.txt");
    read_env_file(dir, "env_test.txt");
}

/* Otherwise the player's disc image (disc_image_android.c), which the
 * activity's Java side (PokeParkActivity.java) asks for and opens. */
int disc_image_open(int fd, char* err, size_t n);

static const char kAsk[] =
    "PokePark plays from your own copy of the game: a disc image of PokePark Wii: Pikachu's Adventure (USA), "
    "such as an .rvz, .iso or .wbfs file. Choose it on this device.";
static int sAsking; /* waiting for a disc image to be chosen */

/* Calls one of the activity's Java methods, on this thread (attached to
 * the VM once): "I" ones return an int, "S" ones take a string. */
static int java(struct android_app* app, const char* name, const char* arg) {
    static JNIEnv* env;
    jclass cls;
    jint r = -1;
    if (!env && (*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL) != JNI_OK) return -1;
    cls = (*env)->GetObjectClass(env, app->activity->clazz);
    if (arg) {
        jstring s = (*env)->NewStringUTF(env, arg);
        (*env)->CallVoidMethod(env, app->activity->clazz, (*env)->GetMethodID(env, cls, name, "(Ljava/lang/String;)V"), s);
        (*env)->DeleteLocalRef(env, s);
    } else if (!strcmp(name, "forgetGame")) {
        (*env)->CallVoidMethod(env, app->activity->clazz, (*env)->GetMethodID(env, cls, name, "()V"));
    } else {
        r = (*env)->CallIntMethod(env, app->activity->clazz, (*env)->GetMethodID(env, cls, name, "()I"));
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        r = -1;
    }
    (*env)->DeleteLocalRef(env, cls);
    return r;
}

/* Shown when this device's Vulkan can't run the game: before the disc
 * image is asked for, rather than its being chosen and the game closing
 * as it starts drawing. */
static const char kNoVulkan[] =
    "PokePark needs a GPU with Vulkan 1.3 and the features its renderer uses, and this device's doesn't have "
    "them: %s.\n\nUpdating Android or the GPU's driver may add them.";

/* Tells the player why the game can't go on (the activity's fatal()),
 * and stops this thread: the player closes the game. */
void android_fatal(const char* msg) {
    JNIEnv* env;
    JavaVM* vm = g_android_app->activity->vm;
    fprintf(stderr, "PokePark: %s\n", msg);
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK) {
        jobject act = g_android_app->activity->clazz;
        jclass cls = (*env)->GetObjectClass(env, act);
        jstring s = (*env)->NewStringUTF(env, msg);
        (*env)->CallVoidMethod(env, act, (*env)->GetMethodID(env, cls, "fatal", "(Ljava/lang/String;)V"), s);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
    for (;;) pause();
}

/* Plays from the disc image in fd, or asks again, saying what's wrong. */
static void use_image(struct android_app* app, int fd) {
    char err[512], msg[768];
    if (disc_image_open(fd, err, sizeof(err)) == 0) {
        sAsking = 0;
        sHaveGame = 1;
        if (!sGameStarted && sWindow) start_game();
        return;
    }
    fprintf(stderr, "PokePark: %s\n", err);
    java(app, "forgetGame", NULL);
    snprintf(msg, sizeof(msg), "%s\n\nChoose your disc image of PokePark Wii: Pikachu's Adventure (USA).", err);
    sAsking = 1;
    java(app, "chooseGame", msg);
}

void android_main(struct android_app* app) {
    const char* shared = app->activity->externalDataPath;
    char path[1024], why[512], msg[1024];
    log_to_logcat();
    g_android_app = app;
    g_android_assets = app->activity->assetManager;
    app->onAppCmd = on_command;
    app->onInputEvent = on_input;
    /* Before the window has focus: the activity's thread is still
     * starting it, after native_app_glue set its callbacks. */
    sGlueFocus = app->activity->callbacks->onWindowFocusChanged;
    app->activity->callbacks->onWindowFocusChanged = on_focus;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    snprintf(path, sizeof(path), "%s/settings.ini", app->activity->internalDataPath);
    setenv("RECOMP_SETTINGS", path, 0);
    if (shared) {
        setenv("RECOMP_SHARED", shared, 1); /* for files written for test runs (boot.c's profile) */
        read_env(shared);
    }
    if (!vk_check(why, sizeof(why))) {
        /* Nothing to ask for: the game couldn't draw. */
        snprintf(msg, sizeof(msg), kNoVulkan, why);
        fprintf(stderr, "PokePark: %s\n", msg);
        java(app, "fatal", msg);
    } else if (shared && find_game(shared)) {
        sHaveGame = 1;
        fprintf(stderr, "PokePark: %s\n", sDol);
    } else {
        /* The saves go where they would beside an extracted game. */
        int fd;
        if (shared) mkdir(shared, 0770);
        snprintf(sDol, sizeof(sDol), "%s/R8AE01/sys/main.dol", shared ? shared : app->activity->internalDataPath);
        if ((fd = java(app, "savedGameFd", NULL)) >= 0) {
            use_image(app, fd);
        } else {
            sAsking = 1;
            java(app, "chooseGame", kAsk);
        }
    }
    AChoreographer_registerRefreshRateCallback(AChoreographer_getInstance(), on_refresh_rate, NULL);
    second_screen_start(app->activity->vm, app->activity->clazz);
    for (;;) {
        int events, fd;
        struct android_poll_source* source;
        /* While asking, looking every 100 ms for the answer. */
        if (ALooper_pollOnce(sAsking ? 100 : -1, NULL, &events, (void**)&source) >= 0 && source)
            source->process(app, source);
        if (app->destroyRequested) exit(0);
        if (sAsking && (fd = java(app, "takePickedFd", NULL)) >= 0) use_image(app, fd);
    }
}
