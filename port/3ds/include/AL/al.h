#ifndef NOX_3DS_AL_H
#define NOX_3DS_AL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef char           ALboolean;
typedef char           ALchar;
typedef signed char    ALbyte;
typedef unsigned char  ALubyte;
typedef short          ALshort;
typedef unsigned short ALushort;
typedef int            ALint;
typedef unsigned int   ALuint;
typedef int            ALsizei;
typedef int            ALenum;
typedef float          ALfloat;
typedef double         ALdouble;
typedef void           ALvoid;

#define AL_FALSE 0
#define AL_TRUE  1
#define AL_NO_ERROR 0
#define AL_FORMAT_MONO16   0x1101
#define AL_FORMAT_STEREO16 0x1103
#define AL_GAIN            0x100A
#define AL_PITCH           0x1003
#define AL_POSITION        0x1004
#define AL_SOURCE_RELATIVE 0x0202
#define AL_SOURCE_STATE    0x1010
#define AL_PLAYING         0x1012
#define AL_PAUSED          0x1013
#define AL_STOPPED         0x1014
#define AL_BUFFERS_QUEUED  0x1015
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_VENDOR          0xB001
#define AL_VERSION         0xB002
#define AL_RENDERER        0xB003

void alGenSources(ALsizei n, ALuint *sources);
void alDeleteSources(ALsizei n, const ALuint *sources);
void alGenBuffers(ALsizei n, ALuint *buffers);
void alDeleteBuffers(ALsizei n, const ALuint *buffers);
void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
void alSourcePlay(ALuint source);
void alSourcePause(ALuint source);
void alSourceStop(ALuint source);
void alSourcef(ALuint source, ALenum param, ALfloat value);
void alSourcefv(ALuint source, ALenum param, const ALfloat *values);
void alSourceQueueBuffers(ALuint source, ALsizei nb, const ALuint *buffers);
void alSourceUnqueueBuffers(ALuint source, ALsizei nb, ALuint *buffers);
void alGetSourcei(ALuint source, ALenum param, ALint *value);
void alListenerf(ALenum param, ALfloat value);
void alListener3f(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
ALenum alGetError(void);
const ALchar *alGetString(ALenum param);

#ifdef __cplusplus
}
#endif

#endif
