#include "platform_3ds.h"
#include "nox_3ds_version.h"

#ifndef NOX_3DS_VERSION_STR
#define NOX_3DS_VERSION_STR "3ds-dev"
#endif

#include <3ds.h>
#include <ctype.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Large stack buffers in sub_4DBE10/sub_51F230 are moved to heap; 256 KB gives safe headroom. */
u32 __stacksize__ = 256 * 1024;

static nox_3ds_state_t g_3ds_state;
static PrintConsole g_boot_console;
static bool g_initialized = false;
static bool g_shutdown_done = false;
static bool g_console_detached = false;
static u64 g_start_ms = 0;
static aptHookCookie g_apt_hook_cookie;
static bool g_apt_hook_registered = false;

static void nox_3ds_apt_hook_cb(APT_HookType hook, void *param)
{
    (void)param;
    switch (hook) {
    case APTHOOK_ONSUSPEND:
    case APTHOOK_ONSLEEP:
    case APTHOOK_ONEXIT:
        /* Force a full fflush() and fsync() on open file descriptors when
         * the user presses HOME, closes the lid, or exits. */
        nox_3ds_commit_save();
        break;
    default:
        break;
    }
}

struct nox_3ds_mutex {
    RecursiveLock lock;
};

/* Swallow stdout tile writes once citro2d takes ownership of GFX_BOTTOM. */
static bool nox_3ds_console_sink(void *con, int c)
{
    (void)con;
    (void)c;
    return true;
}

bool nox_3ds_init(void)
{
    if (g_initialized)
        return true;

    osSetSpeedupEnable(true);
    g_start_ms = osGetTime();

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, &g_boot_console);
    consoleDebugInit(debugDevice_CONSOLE);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&g_3ds_state, 0, sizeof(g_3ds_state));
    g_3ds_state.bottom_console_mode = true;
    g_3ds_state.wide_stretch = true;
    g_3ds_state.touch_to_aim = false;
    g_3ds_state.aim_mode = NOX_3DS_AIM_RADIAL;
    g_3ds_state.cstick_mode = NOX_3DS_CSTICK_AIM_CAMERA;

    bool is_n3ds = false;
    if (R_SUCCEEDED(APT_CheckNew3DS(&is_n3ds))) {
        g_3ds_state.is_new_3ds = is_n3ds;
    }

    g_3ds_state.linear_free_bytes = (uint32_t)linearSpaceFree();
    g_3ds_state.heap_used_bytes = (uint32_t)mallinfo().uordblks;

    printf("=== Nox New 3DS Port [%s] ===\n", NOX_3DS_VERSION_STR);
    printf("[3ds] Ported by Fungus Shark & AlbertChen408 (Giraffepus) | Orig: Westwood Studios\n");
    printf("[3ds] Model: %s\n", g_3ds_state.is_new_3ds ? "New 3DS (804MHz + L2)" : "WARNING: Old 3DS (268MHz)");
    printf("[3ds] Stack: %u KB | linearFree: %u KB\n",
           (unsigned)(__stacksize__ / 1024u),
           (unsigned)(g_3ds_state.linear_free_bytes / 1024u));

    Result rc = romfsInit();
    if (R_SUCCEEDED(rc)) {
        g_3ds_state.romfs_mounted = true;
        printf("[3ds] romfs mounted OK\n");
    } else {
        g_3ds_state.romfs_mounted = false;
        printf("[3ds] romfs not bundled; using SDMC\n");
    }

    struct stat st;
    if (stat(NOX_3DS_DATA_ROOT, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (chdir(NOX_3DS_DATA_ROOT) == 0) {
            g_3ds_state.sdmc_root_ready = true;
            printf("[3ds] chdir(%s) OK\n", NOX_3DS_DATA_ROOT);
            nox_3ds_ensure_dir(NOX_3DS_DATA_ROOT "/save");
            nox_3ds_ensure_dir(NOX_3DS_DATA_ROOT "/gui");
        } else {
            printf("[3ds] WARN: chdir(%s) failed\n", NOX_3DS_DATA_ROOT);
        }
    } else {
        printf("[3ds] WARN: %s not found on SD card\n", NOX_3DS_DATA_ROOT);
    }

    nox_3ds_net_init();
    nox_3ds_input_init();
    nox_3ds_audio_init();

    aptHook(&g_apt_hook_cookie, nox_3ds_apt_hook_cb, NULL);
    g_apt_hook_registered = true;

    atexit(nox_3ds_shutdown);
    g_initialized = true;
    g_console_detached = false;
    gspWaitForVBlank();
    return true;
}

void nox_3ds_console_detach(void)
{
    if (g_console_detached)
        return;

    /* Prevent libctru PrintConsole from writing CPU RGB565 glyphs into GFX_BOTTOM
     * while citro2d owns the bottom screen in BGR8 double-buffered GPU mode. */
    g_boot_console.PrintChar = nox_3ds_console_sink;
    consoleDebugInit(debugDevice_SVC);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    gfxSwapBuffersGpu();
    gspWaitForVBlank();
    g_3ds_state.bottom_console_mode = false;
    g_console_detached = true;
}

void nox_3ds_shutdown(void)
{
    if (g_shutdown_done || !g_initialized)
        return;
    g_shutdown_done = true;

    nox_3ds_commit_save();
    nox_3ds_load_log_flush();
    nox_3ds_crypt_cache_clear();
    if (g_apt_hook_registered) {
        aptUnhook(&g_apt_hook_cookie);
        g_apt_hook_registered = false;
    }
    nox_3ds_audio_shutdown();
    nox_3ds_video_shutdown();
    nox_3ds_net_shutdown();

    /* Only restore the bottom console and pause on exit when requested for diagnostics. */
    g_console_detached = false;
    g_3ds_state.bottom_console_mode = true;

    const char *w = getenv("NOX_3DS_WAIT_ON_EXIT");
    if (w && w[0] == '1') {
        consoleInit(GFX_BOTTOM, NULL);
        consoleDebugInit(debugDevice_CONSOLE);

        printf("\n[3ds] Engine exited. Press START to exit.\n");
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();

        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START)
                break;
            gspWaitForVBlank();
        }
    }

    if (g_3ds_state.romfs_mounted) {
        romfsExit();
        g_3ds_state.romfs_mounted = false;
    }
    gfxExit();
}

bool nox_3ds_apt_ok(void)
{
    if (!aptMainLoop()) {
        return false;
    }
    /* Only swap via GSP CPU buffers before citro3d owns the screens. */
    if (!nox_3ds_video_is_active()) {
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
    return true;
}

uint32_t nox_3ds_get_ticks_ms(void)
{
    u64 now = osGetTime();
    if (now < g_start_ms)
        return 0;
    return (uint32_t)(now - g_start_ms);
}

uint64_t nox_3ds_get_perf_counter(void)
{
    return (uint64_t)svcGetSystemTick();
}

uint64_t nox_3ds_get_perf_freq(void)
{
    return (uint64_t)SYSCLOCK_ARM11;
}

void nox_3ds_sleep_ms(uint32_t ms)
{
    svcSleepThread((s64)ms * 1000000LL);
}

const nox_3ds_state_t *nox_3ds_get_state(void)
{
    g_3ds_state.linear_free_bytes = (uint32_t)linearSpaceFree();
    g_3ds_state.heap_used_bytes = (uint32_t)mallinfo().uordblks;
    return &g_3ds_state;
}

nox_3ds_state_t *nox_3ds_state_mut(void)
{
    return &g_3ds_state;
}

nox_3ds_mutex_t *nox_3ds_mutex_create(void)
{
    nox_3ds_mutex_t *m = (nox_3ds_mutex_t *)calloc(1, sizeof(nox_3ds_mutex_t));
    if (m) {
        RecursiveLock_Init(&m->lock);
    }
    return m;
}

void nox_3ds_mutex_destroy(nox_3ds_mutex_t *m)
{
    free(m);
}

void nox_3ds_mutex_lock(nox_3ds_mutex_t *m)
{
    if (m) {
        RecursiveLock_Lock(&m->lock);
    }
}

void nox_3ds_mutex_unlock(nox_3ds_mutex_t *m)
{
    if (m) {
        RecursiveLock_Unlock(&m->lock);
    }
}

void nox_3ds_movie_log(const char *fmt, ...)
{
    static bool s_log_started = false;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    uint32_t ms = nox_3ds_get_ticks_ms();
    fprintf(stderr, "[movie3ds %ums] %s\n", (unsigned)ms, msg);

    const char *mode = s_log_started ? "a" : "w";
    FILE *f = fopen(NOX_3DS_DATA_ROOT "/movie.log", mode);
    if (f) {
        s_log_started = true;
        fprintf(f, "[%ums] %s\n", (unsigned)ms, msg);
        fflush(f);
        fclose(f);
    }
}
