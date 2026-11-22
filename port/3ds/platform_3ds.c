#include "platform_3ds.h"

#include <3ds.h>
#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Nox has functions with 75-144 KB stack frames; libctru's default 32 KB is insufficient. */
u32 __stacksize__ = 1024 * 1024;

static nox_3ds_state_t g_3ds_state;
static PrintConsole g_boot_console;
static bool g_initialized = false;
static bool g_shutdown_done = false;
static bool g_console_detached = false;
static u64 g_start_ms = 0;

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

    printf("=== Nox New 3DS Port (Dual-Screen UI) ===\n");
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
            mkdir(NOX_3DS_DATA_ROOT "/save", 0777);
        } else {
            printf("[3ds] WARN: chdir(%s) failed\n", NOX_3DS_DATA_ROOT);
        }
    } else {
        printf("[3ds] WARN: %s not found on SD card\n", NOX_3DS_DATA_ROOT);
    }

    nox_3ds_net_init();
    nox_3ds_input_init();

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

    nox_3ds_video_shutdown();
    nox_3ds_net_shutdown();

    /* Restore bottom console so shutdown diagnostics are visible before exit. */
    consoleInit(GFX_BOTTOM, NULL);
    consoleDebugInit(debugDevice_CONSOLE);
    g_console_detached = false;
    g_3ds_state.bottom_console_mode = true;

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

void nox_3ds_resolve_path(const char *path, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!path || !*path)
        return;

    /* Strip Windows drive prefix (e.g. C:\ or c:/) */
    if (isalpha((unsigned char)path[0]) && path[1] == ':') {
        path += 2;
    }

    char norm[512];
    size_t n = 0;
    for (size_t i = 0; path[i] && n + 1 < sizeof(norm); ++i) {
        char c = (path[i] == '\\') ? '/' : path[i];
        /* Collapse duplicate slashes */
        if (c == '/' && n > 0 && norm[n - 1] == '/')
            continue;
        norm[n++] = c;
    }
    norm[n] = '\0';

    /* Strip trailing slash unless it's the device root (e.g. "sdmc:/") */
    while (n > 1 && norm[n - 1] == '/' && !(n >= 2 && norm[n - 2] == ':')) {
        norm[--n] = '\0';
    }

    if (strncmp(norm, "sdmc:/", 6) == 0 || strncmp(norm, "romfs:/", 7) == 0) {
        snprintf(out, out_sz, "%s", norm);
        return;
    }

    const char *rel = norm;
    while (*rel == '/')
        rel++;
    if (rel[0] == '.' && rel[1] == '/')
        rel += 2;

    /* Avoid double-prefixing if getcwd() returned "/3ds/nox/..." without "sdmc:" */
    if (strncasecmp(rel, "3ds/nox/", 8) == 0) {
        rel += 8;
    } else if (strcasecmp(rel, "3ds/nox") == 0) {
        rel = "";
    }

    if (*rel == '\0') {
        snprintf(out, out_sz, "%s", NOX_3DS_DATA_ROOT);
    } else {
        snprintf(out, out_sz, "%s/%s", NOX_3DS_DATA_ROOT, rel);
    }
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
