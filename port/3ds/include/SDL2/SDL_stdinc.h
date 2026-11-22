#ifndef NOX_3DS_SDL_STDINC_H
#define NOX_3DS_SDL_STDINC_H

#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;

typedef enum {
    SDL_FALSE = 0,
    SDL_TRUE  = 1
} SDL_bool;

#ifdef __cplusplus
extern "C" {
#endif
char *SDL_itoa(int value, char *str, int radix);
char *SDL_uitoa(unsigned int value, char *str, int radix);
#ifdef __cplusplus
}
#endif

#endif
