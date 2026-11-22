#ifndef NOX_3DS_GL_H
#define NOX_3DS_GL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef double         GLdouble;
typedef char           GLchar;
typedef intptr_t       GLintptr;
typedef ptrdiff_t      GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE  1
#define GL_NO_ERROR 0

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES        0x0004
#define GL_TRIANGLE_STRIP   0x0005
#define GL_UNSIGNED_BYTE    0x1401
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#define GL_FLOAT            0x1406
#define GL_RGBA             0x1908
#define GL_BGRA             0x80E1
#define GL_RED              0x1903
#define GL_GREEN            0x1904
#define GL_BLUE             0x1905
#define GL_ALPHA            0x1906
#define GL_TEXTURE_2D       0x0DE1
#define GL_TEXTURE0         0x84C0
#define GL_ACTIVE_TEXTURE   0x84E0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S   0x2802
#define GL_TEXTURE_WRAP_T   0x2803
#define GL_TEXTURE_SWIZZLE_RGBA 0x8E46
#define GL_NEAREST          0x2600
#define GL_LINEAR           0x2601
#define GL_CLAMP_TO_EDGE    0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_VIEWPORT         0x0BA2
#define GL_FRAMEBUFFER      0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_ARRAY_BUFFER     0x8892
#define GL_STATIC_DRAW      0x88E4
#define GL_FRAGMENT_SHADER  0x8B30
#define GL_VERTEX_SHADER    0x8B31
#define GL_COMPILE_STATUS   0x8B81
#define GL_LINK_STATUS      0x8B82
#define GL_VERSION          0x1F02
#define GL_RENDERER         0x1F01
#define GL_VENDOR           0x1F00
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

void glActiveTexture(GLenum texture);
void glAttachShader(GLuint program, GLuint shader);
void glBindBuffer(GLenum target, GLuint buffer);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
void glBindTexture(GLenum target, GLuint texture);
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glClear(GLbitfield mask);
void glCompileShader(GLuint shader);
GLuint glCreateProgram(void);
GLuint glCreateShader(GLenum shaderType);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glEnableVertexAttribArray(GLuint index);
void glGenBuffers(GLsizei n, GLuint *buffers);
void glGenTextures(GLsizei n, GLuint *textures);
GLint glGetAttribLocation(GLuint program, const GLchar *name);
GLenum glGetError(void);
void glGetIntegerv(GLenum pname, GLint *data);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glGetShaderInfoLog(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
const GLubyte *glGetString(GLenum name);
GLint glGetUniformLocation(GLuint program, const GLchar *name);
GLboolean glIsProgram(GLuint program);
GLboolean glIsTexture(GLuint texture);
void glLinkProgram(GLuint program);
void glPixelStorei(GLenum pname, GLint param);
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
void glUniform1f(GLint location, GLfloat v0);
void glUniform1i(GLint location, GLint v0);
void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
void glUseProgram(GLuint program);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

#ifdef __cplusplus
}
#endif

#endif
