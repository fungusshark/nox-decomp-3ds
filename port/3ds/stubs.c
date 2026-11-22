#include "platform_3ds.h"
#include "../../src/windows.h"
#include "../../src/netextras.h"

#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <AL/al.h>
#include <AL/alc.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rate-limited stub diagnostic macro: prints first hit and throttles hot per-frame stubs. */
#define LOG_STUB() do { \
    static uint32_t _stub_calls = 0; \
    _stub_calls++; \
    if (_stub_calls == 1 || (_stub_calls % 50000u) == 0) { \
        fprintf(stderr, "[STUB] %s (count=%u)\n", __func__, (unsigned)_stub_calls); \
    } \
} while (0)

/* =========================================================================
 * 1. SDL2 Stubs
 * ========================================================================= */
static const char *g_sdl_err = "3DS stub";
static int g_dummy_window = 0x1001;
static int g_dummy_gl_ctx = 0x2001;
static Uint32 g_win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP;

int SDL_Init(Uint32 flags)
{
    (void)flags;
    LOG_STUB();
    return 0;
}

const char *SDL_GetError(void)
{
    LOG_STUB();
    return g_sdl_err;
}

Uint32 SDL_GetTicks(void)
{
    return nox_3ds_get_ticks_ms();
}

void SDL_Delay(Uint32 ms)
{
    nox_3ds_sleep_ms(ms);
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return (Uint64)nox_3ds_get_perf_counter();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return (Uint64)nox_3ds_get_perf_freq();
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    LOG_STUB();
    return (SDL_Window *)&g_dummy_window;
}

void SDL_RaiseWindow(SDL_Window *window) { (void)window; LOG_STUB(); }
void SDL_SetWindowTitle(SDL_Window *window, const char *title) { (void)window; (void)title; LOG_STUB(); }
void SDL_RestoreWindow(SDL_Window *window) { (void)window; LOG_STUB(); }
void SDL_MinimizeWindow(SDL_Window *window) { (void)window; LOG_STUB(); }
void SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) { (void)window; (void)flags; LOG_STUB(); }
void SDL_SetWindowSize(SDL_Window *window, int w, int h) { (void)window; (void)w; (void)h; LOG_STUB(); }
void SDL_SetWindowPosition(SDL_Window *window, int x, int y) { (void)window; (void)x; (void)y; LOG_STUB(); }
void SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed) { (void)window; (void)grabbed; LOG_STUB(); }
int SDL_UpdateWindowSurface(SDL_Window *window) { (void)window; LOG_STUB(); return 0; }

Uint32 SDL_GetWindowFlags(SDL_Window *window)
{
    (void)window;
    LOG_STUB();
    return g_win_flags;
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    (void)window;
    LOG_STUB();
    if (w) *w = NOX_3DS_LOGICAL_W;
    if (h) *h = NOX_3DS_LOGICAL_H;
}

SDL_Window *SDL_GetWindowFromID(Uint32 id)
{
    (void)id;
    LOG_STUB();
    return (SDL_Window *)&g_dummy_window;
}

int SDL_GL_SetAttribute(int attr, int value) { (void)attr; (void)value; LOG_STUB(); return 0; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window) { (void)window; LOG_STUB(); return (SDL_GLContext)&g_dummy_gl_ctx; }
void SDL_GL_DeleteContext(SDL_GLContext ctx) { (void)ctx; LOG_STUB(); }
int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext ctx) { (void)window; (void)ctx; LOG_STUB(); return 0; }
SDL_GLContext SDL_GL_GetCurrentContext(void) { LOG_STUB(); return (SDL_GLContext)&g_dummy_gl_ctx; }
SDL_Window *SDL_GL_GetCurrentWindow(void) { LOG_STUB(); return (SDL_Window *)&g_dummy_window; }
void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
    (void)window;
    LOG_STUB();
    if (w) *w = NOX_3DS_LOGICAL_W;
    if (h) *h = NOX_3DS_LOGICAL_H;
}
int SDL_GL_SetSwapInterval(int interval) { (void)interval; LOG_STUB(); return 0; }
void SDL_GL_SwapWindow(SDL_Window *window) { (void)window; LOG_STUB(); }

static SDL_Surface *stub_create_surface(int w, int h, int bpp, Uint32 format)
{
    SDL_Surface *s = (SDL_Surface *)calloc(1, sizeof(SDL_Surface));
    SDL_PixelFormat *pf = (SDL_PixelFormat *)calloc(1, sizeof(SDL_PixelFormat));
    if (!s || !pf) {
        free(s);
        free(pf);
        return NULL;
    }
    int bytes_pp = (bpp > 0) ? ((bpp + 7) / 8) : 2;
    pf->format = format;
    pf->BitsPerPixel = (Uint8)(bytes_pp * 8);
    pf->BytesPerPixel = (Uint8)bytes_pp;
    s->w = w;
    s->h = h;
    s->format = pf;

    if (w >= NOX_3DS_LOGICAL_W && h >= NOX_3DS_LOGICAL_H && bytes_pp == 2) {
        /* Backbuffer: pitch=1024*2 in linearAlloc so C3D_SyncDisplayTransfer is zero-copy. */
        int alloc_w = (w <= NOX_3DS_TEX_W) ? NOX_3DS_TEX_W : w;
        int alloc_h = (h <= NOX_3DS_TEX_H) ? NOX_3DS_TEX_H : h;
        s->pitch = alloc_w * bytes_pp;
        s->pixels = nox_3ds_linear_alloc((size_t)s->pitch * (size_t)alloc_h);
        if (s->pixels) {
            s->is_linear = 1;
            return s;
        }
    }

    s->pitch = w * bytes_pp;
    s->is_linear = 0;
    if (w > 0 && h > 0) {
        s->pixels = calloc((size_t)w * (size_t)h, (size_t)bytes_pp);
    }
    return s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
                                  Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    return stub_create_surface(w, h, depth, SDL_PIXELFORMAT_RGBA5551);
}

SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format)
{
    (void)flags;
    return stub_create_surface(w, h, depth, format);
}

void SDL_FreeSurface(SDL_Surface *surface)
{
    if (!surface) return;
    if (surface->is_linear)
        nox_3ds_linear_free(surface->pixels);
    else
        free(surface->pixels);
    free(surface->format);
    free(surface);
}

int SDL_LockSurface(SDL_Surface *surface) { (void)surface; return 0; }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }
int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key)
{
    if (!surface) return -1;
    surface->has_colorkey = flag ? 1 : 0;
    surface->colorkey = key;
    return 0;
}
int SDL_GetColorKey(SDL_Surface *surface, Uint32 *key)
{
    if (!surface || !surface->has_colorkey) return -1;
    if (key) *key = surface->colorkey;
    return 0;
}
int SDL_SetSurfaceBlendMode(SDL_Surface *surface, int blendMode) { (void)surface; (void)blendMode; return 0; }
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b)
{
    if (format && format->format == SDL_PIXELFORMAT_RGB565) {
        return (Uint32)((((r >> 3) & 0x1F) << 11) | (((g >> 2) & 0x3F) << 5) | ((b >> 3) & 0x1F));
    }
    return (Uint32)((((r >> 3) & 0x1F) << 11) | (((g >> 3) & 0x1F) << 6) | (((b >> 3) & 0x1F) << 1) | 1);
}
int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    if (!src || !dst || !src->pixels || !dst->pixels)
        return -1;

    int sx = srcrect ? srcrect->x : 0;
    int sy = srcrect ? srcrect->y : 0;
    int sw = srcrect ? srcrect->w : src->w;
    int sh = srcrect ? srcrect->h : src->h;

    int dx = dstrect ? dstrect->x : 0;
    int dy = dstrect ? dstrect->y : 0;
    int dw = dstrect ? dstrect->w : dst->w;
    int dh = dstrect ? dstrect->h : dst->h;

    return nox_3ds_blit_scaled_16(
        src->pixels, src->w, src->h, src->pitch,
        src->has_colorkey != 0, (uint16_t)src->colorkey,
        sx, sy, sw, sh,
        dst->pixels, dst->w, dst->h, dst->pitch,
        dx, dy, dw, dh
    );
}
int SDL_LockTexture(void *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    (void)texture; (void)rect; (void)pixels; (void)pitch;
    LOG_STUB();
    return -1;
}
void SDL_UnlockTexture(void *texture) { (void)texture; LOG_STUB(); }
int SDL_SetRenderDrawColor(void *r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) { (void)r; (void)cr; (void)cg; (void)cb; (void)ca; LOG_STUB(); return 0; }
int SDL_RenderClear(void *r) { (void)r; LOG_STUB(); return 0; }
int SDL_RenderCopy(void *r, void *t, const SDL_Rect *s, const SDL_Rect *d) { (void)r; (void)t; (void)s; (void)d; LOG_STUB(); return 0; }
void SDL_RenderPresent(void *r) { (void)r; LOG_STUB(); }
int SDL_RenderSetLogicalSize(void *r, int w, int h) { (void)r; (void)w; (void)h; LOG_STUB(); return 0; }

int SDL_PollEvent(SDL_Event *event) { (void)event; LOG_STUB(); return 0; }
void SDL_PumpEvents(void) { LOG_STUB(); }
int SDL_PushEvent(SDL_Event *event) { (void)event; LOG_STUB(); return 1; }
Uint8 SDL_EventState(Uint32 type, int state) { (void)type; (void)state; LOG_STUB(); return SDL_IGNORE; }
Uint8 SDL_GetEventState(Uint32 type) { (void)type; LOG_STUB(); return SDL_ENABLE; }
int SDL_GetModState(void) { LOG_STUB(); return 0; }
Uint32 SDL_GetMouseState(int *x, int *y) { LOG_STUB(); if (x) *x = 0; if (y) *y = 0; return 0; }
int SDL_SetRelativeMouseMode(SDL_bool enabled) { (void)enabled; LOG_STUB(); return 0; }
SDL_bool SDL_GetRelativeMouseMode(void) { LOG_STUB(); return SDL_FALSE; }
int SDL_CaptureMouse(SDL_bool enabled) { (void)enabled; LOG_STUB(); return 0; }
int SDL_ShowCursor(int toggle) { (void)toggle; LOG_STUB(); return SDL_DISABLE; }
void SDL_StartTextInput(void) { LOG_STUB(); }
void SDL_StopTextInput(void) { LOG_STUB(); }

int SDL_NumJoysticks(void) { LOG_STUB(); return 0; }
SDL_bool SDL_IsGameController(int idx) { (void)idx; LOG_STUB(); return SDL_FALSE; }
SDL_GameController *SDL_GameControllerOpen(int idx) { (void)idx; LOG_STUB(); return NULL; }
void SDL_GameControllerClose(SDL_GameController *gc) { (void)gc; LOG_STUB(); }
const char *SDL_GameControllerName(SDL_GameController *gc) { (void)gc; LOG_STUB(); return "3DS"; }
char *SDL_GameControllerMapping(SDL_GameController *gc) { (void)gc; LOG_STUB(); return NULL; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc, SDL_GameControllerAxis axis) { (void)gc; (void)axis; LOG_STUB(); return 0; }
Uint8 SDL_GameControllerGetButton(SDL_GameController *gc, SDL_GameControllerButton button) { (void)gc; (void)button; LOG_STUB(); return 0; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gc) { (void)gc; LOG_STUB(); return NULL; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joy) { (void)joy; LOG_STUB(); return -1; }

SDL_mutex *SDL_CreateMutex(void)
{
    return (SDL_mutex *)nox_3ds_mutex_create();
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
    nox_3ds_mutex_destroy((nox_3ds_mutex_t *)mutex);
}

int SDL_LockMutex(SDL_mutex *mutex)
{
    nox_3ds_mutex_lock((nox_3ds_mutex_t *)mutex);
    return 0;
}

int SDL_UnlockMutex(SDL_mutex *mutex)
{
    nox_3ds_mutex_unlock((nox_3ds_mutex_t *)mutex);
    return 0;
}

SDL_Thread *SDL_CreateThread(int (*fn)(void *), const char *name, void *data)
{
    (void)fn; (void)name; (void)data;
    LOG_STUB();
    return NULL;
}

void SDL_DetachThread(SDL_Thread *thread) { (void)thread; LOG_STUB(); }
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback cb, void *param)
{
    (void)interval; (void)cb; (void)param;
    LOG_STUB();
    return 0;
}

int SDL_AtomicGet(SDL_atomic_t *a) { return a ? __atomic_load_n(&a->value, __ATOMIC_SEQ_CST) : 0; }
int SDL_AtomicSet(SDL_atomic_t *a, int v) { return a ? __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST) : 0; }
SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval)
{
    if (!a) return SDL_FALSE;
    return __atomic_compare_exchange_n(&a->value, &oldval, newval, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? SDL_TRUE : SDL_FALSE;
}

static void u32_to_str(unsigned int v, char *buf, int radix)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[36];
    int n = 0;
    if (radix < 2 || radix > 36) radix = 10;
    do {
        tmp[n++] = digits[v % (unsigned int)radix];
        v /= (unsigned int)radix;
    } while (v && n < (int)sizeof(tmp));
    for (int i = 0; i < n; ++i)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

char *SDL_itoa(int value, char *str, int radix)
{
    if (!str) return NULL;
    if (value < 0 && radix == 10) {
        *str++ = '-';
        u32_to_str((unsigned int)(-value), str, radix);
        return str - 1;
    }
    u32_to_str((unsigned int)value, str, radix);
    return str;
}

char *SDL_uitoa(unsigned int value, char *str, int radix)
{
    if (!str) return NULL;
    u32_to_str(value, str, radix);
    return str;
}

/* =========================================================================
 * 2. OpenGL Stubs
 * ========================================================================= */
static GLuint g_gl_next_id = 1;

void glActiveTexture(GLenum texture) { (void)texture; LOG_STUB(); }
void glAttachShader(GLuint program, GLuint shader) { (void)program; (void)shader; LOG_STUB(); }
void glBindBuffer(GLenum target, GLuint buffer) { (void)target; (void)buffer; LOG_STUB(); }
void glBindFramebuffer(GLenum target, GLuint framebuffer) { (void)target; (void)framebuffer; LOG_STUB(); }
void glBindTexture(GLenum target, GLuint texture) { (void)target; (void)texture; LOG_STUB(); }
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) { (void)target; (void)size; (void)data; (void)usage; LOG_STUB(); }
void glClear(GLbitfield mask) { (void)mask; LOG_STUB(); }
void glCompileShader(GLuint shader) { (void)shader; LOG_STUB(); }
GLuint glCreateProgram(void) { LOG_STUB(); return g_gl_next_id++; }
GLuint glCreateShader(GLenum shaderType) { (void)shaderType; LOG_STUB(); return g_gl_next_id++; }
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { (void)mode; (void)first; (void)count; LOG_STUB(); }
void glEnableVertexAttribArray(GLuint index) { (void)index; LOG_STUB(); }
void glGenBuffers(GLsizei n, GLuint *buffers) { LOG_STUB(); for (GLsizei i = 0; i < n; ++i) buffers[i] = g_gl_next_id++; }
void glGenTextures(GLsizei n, GLuint *textures) { LOG_STUB(); for (GLsizei i = 0; i < n; ++i) textures[i] = g_gl_next_id++; }
GLint glGetAttribLocation(GLuint program, const GLchar *name) { (void)program; (void)name; LOG_STUB(); return 0; }
GLenum glGetError(void) { LOG_STUB(); return GL_NO_ERROR; }
void glGetIntegerv(GLenum pname, GLint *data) { (void)pname; if (data) *data = 0; LOG_STUB(); }
void glGetProgramiv(GLuint program, GLenum pname, GLint *params) { (void)program; (void)pname; if (params) *params = GL_TRUE; LOG_STUB(); }
void glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) { (void)shader; (void)maxLength; if (length) *length = 0; if (infoLog && maxLength > 0) infoLog[0] = '\0'; LOG_STUB(); }
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) { (void)shader; (void)pname; if (params) *params = GL_TRUE; LOG_STUB(); }
const GLubyte *glGetString(GLenum name) { (void)name; LOG_STUB(); return (const GLubyte *)"3DS-PICA200-Stub"; }
GLint glGetUniformLocation(GLuint program, const GLchar *name) { (void)program; (void)name; LOG_STUB(); return 0; }
GLboolean glIsProgram(GLuint program) { (void)program; LOG_STUB(); return GL_TRUE; }
GLboolean glIsTexture(GLuint texture) { (void)texture; LOG_STUB(); return GL_TRUE; }
void glLinkProgram(GLuint program) { (void)program; LOG_STUB(); }
void glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; LOG_STUB(); }
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length) { (void)shader; (void)count; (void)string; (void)length; LOG_STUB(); }
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data) { (void)target; (void)level; (void)internalformat; (void)width; (void)height; (void)border; (void)format; (void)type; (void)data; LOG_STUB(); }
void glTexParameteri(GLenum target, GLenum pname, GLint param) { (void)target; (void)pname; (void)param; LOG_STUB(); }
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params) { (void)target; (void)pname; (void)params; LOG_STUB(); }
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) { (void)target; (void)level; (void)xoffset; (void)yoffset; (void)width; (void)height; (void)format; (void)type; (void)pixels; LOG_STUB(); }
void glUniform1f(GLint location, GLfloat v0) { (void)location; (void)v0; LOG_STUB(); }
void glUniform1i(GLint location, GLint v0) { (void)location; (void)v0; LOG_STUB(); }
void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) { (void)location; (void)count; (void)transpose; (void)value; LOG_STUB(); }
void glUseProgram(GLuint program) { (void)program; LOG_STUB(); }
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) { (void)index; (void)size; (void)type; (void)normalized; (void)stride; (void)pointer; LOG_STUB(); }
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { (void)x; (void)y; (void)width; (void)height; LOG_STUB(); }

/* =========================================================================
 * 3. OpenAL Stubs
 * ========================================================================= */
static int g_al_device = 1;
static int g_al_context = 1;
static ALuint g_al_next_id = 1;

ALCdevice *alcOpenDevice(const ALCchar *devicename) { (void)devicename; LOG_STUB(); return (ALCdevice *)&g_al_device; }
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist) { (void)device; (void)attrlist; LOG_STUB(); return (ALCcontext *)&g_al_context; }
ALCboolean alcMakeContextCurrent(ALCcontext *context) { (void)context; LOG_STUB(); return AL_TRUE; }

void alGenSources(ALsizei n, ALuint *sources) { LOG_STUB(); for (ALsizei i = 0; i < n; ++i) sources[i] = g_al_next_id++; }
void alDeleteSources(ALsizei n, const ALuint *sources) { (void)n; (void)sources; LOG_STUB(); }
void alGenBuffers(ALsizei n, ALuint *buffers) { LOG_STUB(); for (ALsizei i = 0; i < n; ++i) buffers[i] = g_al_next_id++; }
void alDeleteBuffers(ALsizei n, const ALuint *buffers) { (void)n; (void)buffers; LOG_STUB(); }
void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq) { (void)buffer; (void)format; (void)data; (void)size; (void)freq; LOG_STUB(); }
void alSourcePlay(ALuint source) { (void)source; LOG_STUB(); }
void alSourcePause(ALuint source) { (void)source; LOG_STUB(); }
void alSourceStop(ALuint source) { (void)source; LOG_STUB(); }
void alSourcef(ALuint source, ALenum param, ALfloat value) { (void)source; (void)param; (void)value; LOG_STUB(); }
void alSourcefv(ALuint source, ALenum param, const ALfloat *values) { (void)source; (void)param; (void)values; LOG_STUB(); }
void alSourceQueueBuffers(ALuint source, ALsizei nb, const ALuint *buffers) { (void)source; (void)nb; (void)buffers; LOG_STUB(); }
void alSourceUnqueueBuffers(ALuint source, ALsizei nb, ALuint *buffers) { (void)source; (void)nb; (void)buffers; LOG_STUB(); }
void alGetSourcei(ALuint source, ALenum param, ALint *value)
{
    (void)source;
    LOG_STUB();
    if (!value) return;
    if (param == AL_BUFFERS_PROCESSED) *value = 2;
    else if (param == AL_SOURCE_STATE) *value = AL_STOPPED;
    else *value = 0;
}
void alListenerf(ALenum param, ALfloat value) { (void)param; (void)value; LOG_STUB(); }
void alListener3f(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) { (void)param; (void)v1; (void)v2; (void)v3; LOG_STUB(); }
ALenum alGetError(void) { LOG_STUB(); return AL_NO_ERROR; }
const ALchar *alGetString(ALenum param) { (void)param; LOG_STUB(); return "3DS-OpenAL-Stub"; }

/* =========================================================================
 * 4. Socket / Networking / Control Server Stubs (M1: no socInit)
 * ========================================================================= */
extern DWORD last_socket_error;
static int g_stub_sock_fd = 1024;

int WINAPI WSAStartup(WORD wVersionRequested, struct WSAData *lpWSAData)
{
    (void)wVersionRequested; (void)lpWSAData;
    LOG_STUB();
    return 0;
}

int WINAPI WSACleanup(void)
{
    LOG_STUB();
    return 0;
}

SOCKET WINAPI socket(int domain, int type, int protocol)
#undef socket
{
    (void)domain; (void)type; (void)protocol;
    LOG_STUB();
    last_socket_error = 0;
    return g_stub_sock_fd++;
}

int WINAPI closesocket(SOCKET s)
{
    (void)s;
    LOG_STUB();
    last_socket_error = 0;
    return 0;
}

char *WINAPI inet_ntoa(struct in_addr in)
#undef in_addr
#undef inet_ntoa
{
    static char buf[16];
    uint32_t ip = in.S_un.S_addr;
    LOG_STUB();
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)(ip & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu),
             (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 24) & 0xFFu));
    return buf;
}

int WINAPI setsockopt(SOCKET s, int level, int opt, const void *value, unsigned int len)
#undef setsockopt
{
    (void)s; (void)level; (void)opt; (void)value; (void)len;
    LOG_STUB();
    last_socket_error = 0;
    return 0;
}

int WINAPI ioctlsocket(SOCKET s, long cmd, unsigned long *argp)
{
    (void)s; (void)cmd;
    LOG_STUB();
    if (argp && (unsigned long)cmd == 0x4004667ful) {
        *argp = 0;
    }
    last_socket_error = 0;
    return 0;
}

int WINAPI bind(int sockfd, const struct sockaddr *addr, unsigned int addrlen)
#undef bind
{
    (void)sockfd; (void)addr; (void)addrlen;
    LOG_STUB();
    last_socket_error = 0;
    return 0;
}

int WINAPI recvfrom(int sockfd, void *buffer, unsigned int length, int flags,
                    struct sockaddr *addr, unsigned int *addrlen)
#undef recvfrom
{
    (void)sockfd; (void)buffer; (void)length; (void)flags; (void)addr; (void)addrlen;
    LOG_STUB();
    last_socket_error = 10035u; /* WSAEWOULDBLOCK */
    errno = EWOULDBLOCK;
    return -1;
}

int WINAPI sendto(int sockfd, void *buffer, unsigned int length, int flags,
                  const struct sockaddr *addr, unsigned int addrlen)
#undef sendto
{
    (void)sockfd; (void)buffer; (void)flags; (void)addr; (void)addrlen;
    LOG_STUB();
    last_socket_error = 0;
    return (int)length;
}

int WINAPI WSAGetLastError(void)
{
    LOG_STUB();
    return (int)last_socket_error;
}

int gethostname(char *name, size_t len)
{
    LOG_STUB();
    if (name && len > 0) {
        snprintf(name, len, "n3ds-local");
    }
    return 0;
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static uint32_t addr = 0x0100007Fu; /* 127.0.0.1 in network byte order */
    static char *addr_list[2] = { (char *)&addr, NULL };
    static char hname[] = "n3ds-local";
    (void)name;
    LOG_STUB();
    he.h_name = hname;
    he.h_aliases = NULL;
    he.h_addrtype = 2; /* AF_INET */
    he.h_length = 4;
    he.h_addr_list = addr_list;
    return &he;
}

void nox_control_server_init(void) { LOG_STUB(); }
void nox_control_server_pump(void) { LOG_STUB(); }

void nox_netextras_on_discovery_ping_send(int sockfd, const void *buf, size_t len, unsigned dest_port)
{
    (void)sockfd; (void)buf; (void)len; (void)dest_port;
    LOG_STUB();
}

int nox_netextras_try_fake_recvfrom(int sockfd, void *buf, size_t buf_cap, struct sockaddr *from, socklen_t *fromlen)
{
    (void)sockfd; (void)buf; (void)buf_cap; (void)from; (void)fromlen;
    LOG_STUB();
    return -1;
}

void nox_netextras_on_host_bind_success(int sockfd, unsigned bound_port)
{
    (void)sockfd; (void)bound_port;
    LOG_STUB();
}

void nox_netextras_on_host_serverinfo(const void *packet, size_t len)
{
    (void)packet; (void)len;
    LOG_STUB();
}

void nox_netextras_host_tick(void) { LOG_STUB(); }

int nox_netextras_fake_pending(int sockfd)
{
    (void)sockfd;
    LOG_STUB();
    return 0;
}

/* =========================================================================
 * 5. VQA Movie Stubs
 * ========================================================================= */
volatile int g_movie_skip_requested = 0;

void __cdecl sub_555430(HWND *a1)
{
    (void)a1;
    LOG_STUB();
}

char __cdecl sub_555500(char a1)
{
    LOG_STUB();
    g_movie_skip_requested = (a1 != 0);
    return a1;
}

int sub_555510(void)
{
    LOG_STUB();
    return g_movie_skip_requested;
}

/* =========================================================================
 * 6. Env Helpers (normally in excluded lobby.c)
 * ========================================================================= */
int nox_env_truthy(const char *s)
{
    if (!s) return 0;
    while (*s && (unsigned char)*s <= ' ') s++;
    if (!*s) return 0;
    if (s[0] == '1' && s[1] == '\0') return 1;

    char buf[8];
    size_t n = 0;
    while (s[n] && n + 1 < sizeof(buf)) {
        buf[n] = (char)tolower((unsigned char)s[n]);
        n++;
    }
    buf[n] = '\0';

    return (strcmp(buf, "true") == 0) ||
           (strcmp(buf, "yes")  == 0) ||
           (strcmp(buf, "on")   == 0);
}

int nox_env_int(const char *name, int defv)
{
    const char *s = name ? getenv(name) : NULL;
    if (!s || !*s) return defv;
    return atoi(s);
}
