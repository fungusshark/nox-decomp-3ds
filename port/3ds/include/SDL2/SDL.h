#ifndef NOX_3DS_SDL_H
#define NOX_3DS_SDL_H

#include "SDL_stdinc.h"

/* Protect SDL shim structs from src/proto.h's #pragma pack(push, 1). */
#pragma pack(push)
#pragma pack()

#ifdef __cplusplus
extern "C" {
#endif

#define SDL_VERSION_ATLEAST(X, Y, Z) 1

#define SDL_INIT_TIMER          0x00000001u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u

#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000u
#define SDL_WINDOWPOS_CENTERED  0x2FFF0000u

#define SDL_WINDOW_OPENGL             0x00000002u
#define SDL_WINDOW_RESIZABLE          0x00000020u
#define SDL_WINDOW_INPUT_GRABBED      0x00000100u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u

#define SDL_GL_DEPTH_SIZE            6
#define SDL_GL_STENCIL_SIZE          7
#define SDL_GL_DOUBLEBUFFER          5
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18

#define SDL_PIXELFORMAT_RGB555   0x15130F02u
#define SDL_PIXELFORMAT_RGB565   0x15151002u
#define SDL_PIXELFORMAT_ARGB1555 0x15331002u
#define SDL_PIXELFORMAT_RGBA5551 0x15421002u
#define SDL_PIXELFORMAT_RGBA8888 0x16462004u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u

#define SDL_ALPHA_OPAQUE 255
#define SDL_BLENDMODE_NONE 0

#define SDL_QUERY   -1
#define SDL_IGNORE   0
#define SDL_DISABLE  0
#define SDL_ENABLE   1
#define SDL_PRESSED  1

#define SDL_BUTTON_LEFT   1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT  3

#define KMOD_NONE   0x0000
#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL  0x0040
#define KMOD_RCTRL  0x0080
#define KMOD_LALT   0x0100
#define KMOD_RALT   0x0200
#define KMOD_SHIFT  (KMOD_LSHIFT | KMOD_RSHIFT)
#define KMOD_CTRL   (KMOD_LCTRL | KMOD_RCTRL)
#define KMOD_ALT    (KMOD_LALT | KMOD_RALT)

#define SDL_MUSTLOCK(S) 0

typedef struct SDL_Window SDL_Window;
typedef void *SDL_GLContext;
typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_Thread SDL_Thread;
typedef int SDL_TimerID;
typedef Uint32 (*SDL_TimerCallback)(Uint32 interval, void *param);
typedef struct SDL_Joystick SDL_Joystick;
typedef Sint32 SDL_JoystickID;
typedef struct SDL_GameController SDL_GameController;
typedef Sint64 SDL_FingerID;

typedef struct { int value; } SDL_atomic_t;

typedef struct {
    int x, y, w, h;
} SDL_Rect;

typedef struct {
    Uint32 format;
    Uint8 BitsPerPixel;
    Uint8 BytesPerPixel;
} SDL_PixelFormat;

typedef struct {
    Uint32 flags;
    SDL_PixelFormat *format;
    int w;
    int h;
    int pitch;
    void *pixels;
    Uint32 colorkey;
    Uint8 has_colorkey;
    Uint8 is_linear;
} SDL_Surface;

typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B = 5, SDL_SCANCODE_C = 6, SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8, SDL_SCANCODE_F = 9, SDL_SCANCODE_G = 10, SDL_SCANCODE_H = 11,
    SDL_SCANCODE_I = 12, SDL_SCANCODE_J = 13, SDL_SCANCODE_K = 14, SDL_SCANCODE_L = 15,
    SDL_SCANCODE_M = 16, SDL_SCANCODE_N = 17, SDL_SCANCODE_O = 18, SDL_SCANCODE_P = 19,
    SDL_SCANCODE_Q = 20, SDL_SCANCODE_R = 21, SDL_SCANCODE_S = 22, SDL_SCANCODE_T = 23,
    SDL_SCANCODE_U = 24, SDL_SCANCODE_V = 25, SDL_SCANCODE_W = 26, SDL_SCANCODE_X = 27,
    SDL_SCANCODE_Y = 28, SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2 = 31, SDL_SCANCODE_3 = 32, SDL_SCANCODE_4 = 33,
    SDL_SCANCODE_5 = 34, SDL_SCANCODE_6 = 35, SDL_SCANCODE_7 = 36, SDL_SCANCODE_8 = 37,
    SDL_SCANCODE_9 = 38, SDL_SCANCODE_0 = 39,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43, SDL_SCANCODE_SPACE = 44, SDL_SCANCODE_MINUS = 45,
    SDL_SCANCODE_EQUALS = 46, SDL_SCANCODE_LEFTBRACKET = 47, SDL_SCANCODE_RIGHTBRACKET = 48,
    SDL_SCANCODE_BACKSLASH = 49, SDL_SCANCODE_NONUSHASH = 50, SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52, SDL_SCANCODE_GRAVE = 53, SDL_SCANCODE_COMMA = 54,
    SDL_SCANCODE_PERIOD = 55, SDL_SCANCODE_SLASH = 56, SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2 = 59, SDL_SCANCODE_F3 = 60, SDL_SCANCODE_F4 = 61,
    SDL_SCANCODE_F5 = 62, SDL_SCANCODE_F6 = 63, SDL_SCANCODE_F7 = 64, SDL_SCANCODE_F8 = 65,
    SDL_SCANCODE_F9 = 66, SDL_SCANCODE_F10 = 67, SDL_SCANCODE_F11 = 68, SDL_SCANCODE_F12 = 69,
    SDL_SCANCODE_PRINTSCREEN = 70, SDL_SCANCODE_SCROLLLOCK = 71, SDL_SCANCODE_PAUSE = 72,
    SDL_SCANCODE_INSERT = 73, SDL_SCANCODE_HOME = 74, SDL_SCANCODE_PAGEUP = 75,
    SDL_SCANCODE_DELETE = 76, SDL_SCANCODE_END = 77, SDL_SCANCODE_PAGEDOWN = 78,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_NUMLOCKCLEAR = 83,
    SDL_SCANCODE_KP_DIVIDE = 84, SDL_SCANCODE_KP_MULTIPLY = 85, SDL_SCANCODE_KP_MINUS = 86,
    SDL_SCANCODE_KP_PLUS = 87, SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89, SDL_SCANCODE_KP_2 = 90, SDL_SCANCODE_KP_3 = 91,
    SDL_SCANCODE_KP_4 = 92, SDL_SCANCODE_KP_5 = 93, SDL_SCANCODE_KP_6 = 94,
    SDL_SCANCODE_KP_7 = 95, SDL_SCANCODE_KP_8 = 96, SDL_SCANCODE_KP_9 = 97,
    SDL_SCANCODE_KP_0 = 98, SDL_SCANCODE_KP_PERIOD = 99,
    SDL_SCANCODE_NONUSBACKSLASH = 100, SDL_SCANCODE_APPLICATION = 101, SDL_SCANCODE_POWER = 102,
    SDL_SCANCODE_KP_EQUALS = 103,
    SDL_SCANCODE_F13 = 104, SDL_SCANCODE_F14 = 105, SDL_SCANCODE_F15 = 106, SDL_SCANCODE_F16 = 107,
    SDL_SCANCODE_F17 = 108, SDL_SCANCODE_F18 = 109, SDL_SCANCODE_F19 = 110, SDL_SCANCODE_F20 = 111,
    SDL_SCANCODE_F21 = 112, SDL_SCANCODE_F22 = 113, SDL_SCANCODE_F23 = 114, SDL_SCANCODE_F24 = 115,
    SDL_SCANCODE_EXECUTE = 116, SDL_SCANCODE_HELP = 117, SDL_SCANCODE_MENU = 118,
    SDL_SCANCODE_SELECT = 119, SDL_SCANCODE_STOP = 120, SDL_SCANCODE_AGAIN = 121,
    SDL_SCANCODE_UNDO = 122, SDL_SCANCODE_CUT = 123, SDL_SCANCODE_COPY = 124,
    SDL_SCANCODE_PASTE = 125, SDL_SCANCODE_FIND = 126, SDL_SCANCODE_MUTE = 127,
    SDL_SCANCODE_VOLUMEUP = 128, SDL_SCANCODE_VOLUMEDOWN = 129,
    SDL_SCANCODE_KP_COMMA = 133, SDL_SCANCODE_KP_EQUALSAS400 = 134,
    SDL_SCANCODE_INTERNATIONAL1 = 135, SDL_SCANCODE_INTERNATIONAL2 = 136,
    SDL_SCANCODE_INTERNATIONAL3 = 137, SDL_SCANCODE_INTERNATIONAL4 = 138,
    SDL_SCANCODE_INTERNATIONAL5 = 139, SDL_SCANCODE_INTERNATIONAL6 = 140,
    SDL_SCANCODE_INTERNATIONAL7 = 141, SDL_SCANCODE_INTERNATIONAL8 = 142,
    SDL_SCANCODE_INTERNATIONAL9 = 143,
    SDL_SCANCODE_LANG1 = 144, SDL_SCANCODE_LANG2 = 145, SDL_SCANCODE_LANG3 = 146,
    SDL_SCANCODE_LANG4 = 147, SDL_SCANCODE_LANG5 = 148, SDL_SCANCODE_LANG6 = 149,
    SDL_SCANCODE_LANG7 = 150, SDL_SCANCODE_LANG8 = 151, SDL_SCANCODE_LANG9 = 152,
    SDL_SCANCODE_ALTERASE = 153, SDL_SCANCODE_SYSREQ = 154, SDL_SCANCODE_CANCEL = 155,
    SDL_SCANCODE_CLEAR = 156, SDL_SCANCODE_PRIOR = 157, SDL_SCANCODE_RETURN2 = 158,
    SDL_SCANCODE_SEPARATOR = 159, SDL_SCANCODE_OUT = 160, SDL_SCANCODE_OPER = 161,
    SDL_SCANCODE_CLEARAGAIN = 162, SDL_SCANCODE_CRSEL = 163, SDL_SCANCODE_EXSEL = 164,
    SDL_SCANCODE_KP_00 = 176, SDL_SCANCODE_KP_000 = 177, SDL_SCANCODE_THOUSANDSSEPARATOR = 178,
    SDL_SCANCODE_DECIMALSEPARATOR = 179, SDL_SCANCODE_CURRENCYUNIT = 180,
    SDL_SCANCODE_CURRENCYSUBUNIT = 181, SDL_SCANCODE_KP_LEFTPAREN = 182,
    SDL_SCANCODE_KP_RIGHTPAREN = 183, SDL_SCANCODE_KP_LEFTBRACE = 184,
    SDL_SCANCODE_KP_RIGHTBRACE = 185, SDL_SCANCODE_KP_TAB = 186,
    SDL_SCANCODE_KP_BACKSPACE = 187, SDL_SCANCODE_KP_A = 188, SDL_SCANCODE_KP_B = 189,
    SDL_SCANCODE_KP_C = 190, SDL_SCANCODE_KP_D = 191, SDL_SCANCODE_KP_E = 192,
    SDL_SCANCODE_KP_F = 193, SDL_SCANCODE_KP_XOR = 194, SDL_SCANCODE_KP_POWER = 195,
    SDL_SCANCODE_KP_PERCENT = 196, SDL_SCANCODE_KP_LESS = 197, SDL_SCANCODE_KP_GREATER = 198,
    SDL_SCANCODE_KP_AMPERSAND = 199, SDL_SCANCODE_KP_DBLAMPERSAND = 200,
    SDL_SCANCODE_KP_VERTICALBAR = 201, SDL_SCANCODE_KP_DBLVERTICALBAR = 202,
    SDL_SCANCODE_KP_COLON = 203, SDL_SCANCODE_KP_HASH = 204, SDL_SCANCODE_KP_SPACE = 205,
    SDL_SCANCODE_KP_AT = 206, SDL_SCANCODE_KP_EXCLAM = 207, SDL_SCANCODE_KP_MEMSTORE = 208,
    SDL_SCANCODE_KP_MEMRECALL = 209, SDL_SCANCODE_KP_MEMCLEAR = 210,
    SDL_SCANCODE_KP_MEMADD = 211, SDL_SCANCODE_KP_MEMSUBTRACT = 212,
    SDL_SCANCODE_KP_MEMMULTIPLY = 213, SDL_SCANCODE_KP_MEMDIVIDE = 214,
    SDL_SCANCODE_KP_PLUSMINUS = 215, SDL_SCANCODE_KP_CLEAR = 216,
    SDL_SCANCODE_KP_CLEARENTRY = 217, SDL_SCANCODE_KP_BINARY = 218,
    SDL_SCANCODE_KP_OCTAL = 219, SDL_SCANCODE_KP_DECIMAL = 220,
    SDL_SCANCODE_KP_HEXADECIMAL = 221,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT = 225, SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_LGUI = 227, SDL_SCANCODE_RCTRL = 228, SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230, SDL_SCANCODE_RGUI = 231,
    SDL_SCANCODE_MODE = 257,
    SDL_SCANCODE_AUDIONEXT = 258, SDL_SCANCODE_AUDIOPREV = 259,
    SDL_SCANCODE_AUDIOSTOP = 260, SDL_SCANCODE_AUDIOPLAY = 261,
    SDL_SCANCODE_AUDIOMUTE = 262, SDL_SCANCODE_MEDIASELECT = 263,
    SDL_SCANCODE_WWW = 264, SDL_SCANCODE_MAIL = 265, SDL_SCANCODE_CALCULATOR = 266,
    SDL_SCANCODE_COMPUTER = 267, SDL_SCANCODE_AC_SEARCH = 268,
    SDL_SCANCODE_AC_HOME = 269, SDL_SCANCODE_AC_BACK = 270,
    SDL_SCANCODE_AC_FORWARD = 271, SDL_SCANCODE_AC_STOP = 272,
    SDL_SCANCODE_AC_REFRESH = 273, SDL_SCANCODE_AC_BOOKMARKS = 274,
    SDL_SCANCODE_BRIGHTNESSDOWN = 275, SDL_SCANCODE_BRIGHTNESSUP = 276,
    SDL_SCANCODE_DISPLAYSWITCH = 277, SDL_SCANCODE_KBDILLUMTOGGLE = 278,
    SDL_SCANCODE_KBDILLUMDOWN = 279, SDL_SCANCODE_KBDILLUMUP = 280,
    SDL_SCANCODE_EJECT = 281, SDL_SCANCODE_SLEEP = 282,
    SDL_SCANCODE_APP1 = 283, SDL_SCANCODE_APP2 = 284,
    SDL_NUM_SCANCODES = 512
} SDL_Scancode;

typedef enum {
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX
} SDL_GameControllerAxis;

typedef enum {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX
} SDL_GameControllerButton;

enum {
    SDL_QUIT = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP = 0x301,
    SDL_TEXTEDITING = 0x302,
    SDL_TEXTINPUT = 0x303,
    SDL_MOUSEMOTION = 0x400,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP = 0x402,
    SDL_MOUSEWHEEL = 0x403,
    SDL_FINGERDOWN = 0x700,
    SDL_FINGERUP = 0x701,
    SDL_FINGERMOTION = 0x702
};

enum {
    SDL_WINDOWEVENT_SHOWN = 1,
    SDL_WINDOWEVENT_MINIMIZED = 6,
    SDL_WINDOWEVENT_RESTORED = 9,
    SDL_WINDOWEVENT_ENTER = 10,
    SDL_WINDOWEVENT_LEAVE = 11,
    SDL_WINDOWEVENT_FOCUS_GAINED = 12,
    SDL_WINDOWEVENT_FOCUS_LOST = 13
};

typedef struct {
    SDL_Scancode scancode;
    Sint32 sym;
    Uint16 mod;
    Uint32 unused;
} SDL_Keysym;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 event;
    Sint32 data1;
    Sint32 data2;
} SDL_WindowEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 state;
    Uint8 repeat;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 which;
    Uint32 state;
    Sint32 x;
    Sint32 y;
    Sint32 xrel;
    Sint32 yrel;
} SDL_MouseMotionEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 which;
    Uint8 button;
    Uint8 state;
    Uint8 clicks;
    Sint32 x;
    Sint32 y;
} SDL_MouseButtonEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint32 which;
    Sint32 x;
    Sint32 y;
    Uint32 direction;
} SDL_MouseWheelEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Sint64 touchId;
    SDL_FingerID fingerId;
    float x;
    float y;
    float dx;
    float dy;
    float pressure;
} SDL_TouchFingerEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    char text[32];
    Sint32 start;
    Sint32 length;
} SDL_TextEditingEvent;

typedef struct {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    char text[32];
} SDL_TextInputEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextEditingEvent edit;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_TouchFingerEvent tfinger;
    Uint8 padding[56];
} SDL_Event;

int SDL_Init(Uint32 flags);
const char *SDL_GetError(void);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 ms);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_RaiseWindow(SDL_Window *window);
void SDL_SetWindowTitle(SDL_Window *window, const char *title);
void SDL_RestoreWindow(SDL_Window *window);
void SDL_MinimizeWindow(SDL_Window *window);
void SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
void SDL_SetWindowSize(SDL_Window *window, int w, int h);
void SDL_SetWindowPosition(SDL_Window *window, int x, int y);
void SDL_SetWindowGrab(SDL_Window *window, SDL_bool grabbed);
int SDL_UpdateWindowSurface(SDL_Window *window);
Uint32 SDL_GetWindowFlags(SDL_Window *window);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
SDL_Window *SDL_GetWindowFromID(Uint32 id);

int SDL_GL_SetAttribute(int attr, int value);
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window);
void SDL_GL_DeleteContext(SDL_GLContext ctx);
int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext ctx);
SDL_GLContext SDL_GL_GetCurrentContext(void);
SDL_Window *SDL_GL_GetCurrentWindow(void);
void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h);
int SDL_GL_SetSwapInterval(int interval);
void SDL_GL_SwapWindow(SDL_Window *window);

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask);
SDL_Surface *SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int w, int h, int depth, Uint32 format);
void SDL_FreeSurface(SDL_Surface *surface);
int SDL_LockSurface(SDL_Surface *surface);
void SDL_UnlockSurface(SDL_Surface *surface);
int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key);
int SDL_GetColorKey(SDL_Surface *surface, Uint32 *key);
int SDL_SetSurfaceBlendMode(SDL_Surface *surface, int blendMode);
Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b);
int SDL_BlitScaled(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);
int SDL_LockTexture(void *texture, const SDL_Rect *rect, void **pixels, int *pitch);
void SDL_UnlockTexture(void *texture);
int SDL_SetRenderDrawColor(void *r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca);
int SDL_RenderClear(void *r);
int SDL_RenderCopy(void *r, void *t, const SDL_Rect *s, const SDL_Rect *d);
void SDL_RenderPresent(void *r);
int SDL_RenderSetLogicalSize(void *r, int w, int h);

int SDL_PollEvent(SDL_Event *event);
void SDL_PumpEvents(void);
int SDL_PushEvent(SDL_Event *event);
Uint8 SDL_EventState(Uint32 type, int state);
Uint8 SDL_GetEventState(Uint32 type);
int SDL_GetModState(void);
Uint32 SDL_GetMouseState(int *x, int *y);
int SDL_SetRelativeMouseMode(SDL_bool enabled);
SDL_bool SDL_GetRelativeMouseMode(void);
int SDL_CaptureMouse(SDL_bool enabled);
int SDL_ShowCursor(int toggle);
void SDL_StartTextInput(void);
void SDL_StopTextInput(void);

int SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int idx);
SDL_GameController *SDL_GameControllerOpen(int idx);
void SDL_GameControllerClose(SDL_GameController *gc);
const char *SDL_GameControllerName(SDL_GameController *gc);
char *SDL_GameControllerMapping(SDL_GameController *gc);
Sint16 SDL_GameControllerGetAxis(SDL_GameController *gc, SDL_GameControllerAxis axis);
Uint8 SDL_GameControllerGetButton(SDL_GameController *gc, SDL_GameControllerButton button);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gc);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joy);

SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);
SDL_Thread *SDL_CreateThread(int (*fn)(void *), const char *name, void *data);
void SDL_DetachThread(SDL_Thread *thread);
SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_TimerCallback cb, void *param);

int SDL_AtomicGet(SDL_atomic_t *a);
int SDL_AtomicSet(SDL_atomic_t *a, int v);
SDL_bool SDL_AtomicCAS(SDL_atomic_t *a, int oldval, int newval);

#ifdef __cplusplus
}
#endif

#pragma pack(pop)

#endif
