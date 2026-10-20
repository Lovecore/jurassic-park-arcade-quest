/*
 * android_main.c — the Quest app (NativeActivity, no Java): sets up paths
 * and starts the recompiled process (jprt_main) on its own thread; the
 * activity thread only services the Android looper.
 *
 * Data (never inside the APK; pushed with tools/quest/push_data.sh):
 *   <external files>/game/   the arcade game directory (game, g5/, ...)
 *   <external files>/libs/   the i386 libraries the lifted code was built
 *                            from (ld/libc/libm/... + PhysX), flat
 *   <external files>/jprt.env  optional KEY=VALUE lines (JPRT_* settings)
 *   <external files>/jprt.log  the runtime log (also in logcat, tag jprt)
 * <external files> = /sdcard/Android/data/<package>/files
 */
#if defined(__ANDROID__)
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

JavaVM *g_android_vm;
jobject g_android_activity;
ANativeWindow *volatile g_android_window;     /* flat (non-XR) mode draws here */

int jprt_main(int argc, char **argv);
void vid_flat_touch(float x, float y, int down);   /* video_android.c */
void vid_flat_key(int keycode, int down);

static char g_game[1024];

static void load_env_file(const char *dir)
{
    char path[1100], line[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/jprt.env", dir);
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='), *e;
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        for (e = eq + 1 + strlen(eq + 1); e > eq + 1 && (e[-1] == '\n' || e[-1] == '\r'); e--) e[-1] = 0;
        setenv(line, eq + 1, 1);
        __android_log_print(ANDROID_LOG_INFO, "jprt", "env %s=%s", line, eq + 1);
    }
    fclose(f);
}

static void *game_thread(void *p)
{
    char *argv[] = { "jp2015", g_game, NULL };
    (void)p;
    exit(jprt_main(2, argv));
    return NULL;
}

/* flat mode input: touch / mouse -> gun cursor + fire, keys -> keys */
static int32_t on_input(struct android_app *app, AInputEvent *e)
{
    (void)app;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
        int32_t a = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
        int down = a == AMOTION_EVENT_ACTION_DOWN || a == AMOTION_EVENT_ACTION_MOVE ||
                   a == AMOTION_EVENT_ACTION_POINTER_DOWN;
        vid_flat_touch(AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0), down);
        return 1;
    }
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int32_t a = AKeyEvent_getAction(e);
        if (AKeyEvent_getRepeatCount(e) == 0 && (a == AKEY_EVENT_ACTION_DOWN || a == AKEY_EVENT_ACTION_UP))
            vid_flat_key(AKeyEvent_getKeyCode(e), a == AKEY_EVENT_ACTION_DOWN);
        return AKeyEvent_getKeyCode(e) != AKEYCODE_BACK;
    }
    return 0;
}

static void on_cmd(struct android_app *app, int32_t cmd)
{
    if (cmd == APP_CMD_INIT_WINDOW) g_android_window = app->window;
    if (cmd == APP_CMD_TERM_WINDOW) g_android_window = NULL;
    if (cmd == APP_CMD_DESTROY) {
        __android_log_print(ANDROID_LOG_INFO, "jprt", "activity destroyed: exiting");
        exit(0);
    }
}

void android_main(struct android_app *app)
{
    const char *ext = app->activity->externalDataPath, *in = app->activity->internalDataPath;
    char buf[1100];
    pthread_t t;
    pthread_attr_t a;
    g_android_vm = app->activity->vm;
    g_android_activity = app->activity->clazz;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;

    mkdir(ext, 0777);
    if (chdir(ext) != 0) __android_log_print(ANDROID_LOG_ERROR, "jprt", "chdir %s failed", ext);
    snprintf(g_game, sizeof(g_game), "%s/game", ext);
    snprintf(buf, sizeof(buf), "%s/libs", ext);
    setenv("JPRT_LIBS", buf, 0);
    snprintf(buf, sizeof(buf), "%s/rootfs", in);
    setenv("JPRT_ROOT", buf, 0);
    snprintf(buf, sizeof(buf), "%s/jprt.log", ext);
    setenv("JPRT_LOG", buf, 0);
    load_env_file(ext);
    __android_log_print(ANDROID_LOG_INFO, "jprt", "game %s, libs %s", g_game, getenv("JPRT_LIBS"));

    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256u << 20);      /* lifted code recurses on the host stack */
    pthread_create(&t, &a, game_thread, NULL);
    pthread_attr_destroy(&a);

    for (;;) {
        int events;
        struct android_poll_source *src;
        while (ALooper_pollOnce(-1, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) exit(0);
        }
    }
}
#endif
