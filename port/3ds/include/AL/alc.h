#ifndef NOX_3DS_ALC_H
#define NOX_3DS_ALC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;
typedef char ALCboolean;
typedef char ALCchar;
typedef int  ALCint;

ALCdevice *alcOpenDevice(const ALCchar *devicename);
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist);
ALCboolean alcMakeContextCurrent(ALCcontext *context);

#ifdef __cplusplus
}
#endif

#endif
