// Minimal self-contained OpenGL 3.3 core loader (no system GL headers required).
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define HD_APIENTRY __stdcall
#else
#define HD_APIENTRY
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef std::ptrdiff_t GLsizeiptr;
typedef std::ptrdiff_t GLintptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_MULTISAMPLE 0x809D
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_SAMPLES 0x8D57
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2

#define HD_GL_FUNCTIONS(X) \
    X(const GLubyte*, glGetString, (GLenum name)) \
    X(GLenum, glGetError, (void)) \
    X(void, glGetIntegerv, (GLenum pname, GLint* data)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h)) \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glBlendFunc, (GLenum s, GLenum d)) \
    X(void, glDepthFunc, (GLenum f)) \
    X(void, glDepthMask, (GLboolean f)) \
    X(void, glPolygonOffset, (GLfloat factor, GLfloat units)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* data)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glGenBuffers, (GLsizei n, GLuint* b)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* b)) \
    X(void, glBindBuffer, (GLenum target, GLuint b)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* a)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* a)) \
    X(void, glBindVertexArray, (GLuint a)) \
    X(void, glEnableVertexAttribArray, (GLuint i)) \
    X(void, glDisableVertexAttribArray, (GLuint i)) \
    X(void, glVertexAttribPointer, (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p)) \
    X(void, glVertexAttrib3f, (GLuint i, GLfloat x, GLfloat y, GLfloat z)) \
    X(void, glVertexAttrib1f, (GLuint i, GLfloat x)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glShaderSource, (GLuint s, GLsizei n, const GLchar* const* src, const GLint* len)) \
    X(void, glCompileShader, (GLuint s)) \
    X(void, glGetShaderiv, (GLuint s, GLenum p, GLint* v)) \
    X(void, glGetShaderInfoLog, (GLuint s, GLsizei max, GLsizei* len, GLchar* log)) \
    X(void, glDeleteShader, (GLuint s)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint p, GLuint s)) \
    X(void, glBindAttribLocation, (GLuint p, GLuint i, const GLchar* name)) \
    X(void, glLinkProgram, (GLuint p)) \
    X(void, glGetProgramiv, (GLuint p, GLenum pn, GLint* v)) \
    X(void, glGetProgramInfoLog, (GLuint p, GLsizei max, GLsizei* len, GLchar* log)) \
    X(void, glDeleteProgram, (GLuint p)) \
    X(void, glUseProgram, (GLuint p)) \
    X(GLint, glGetUniformLocation, (GLuint p, const GLchar* name)) \
    X(void, glUniform1f, (GLint l, GLfloat v)) \
    X(void, glUniform1i, (GLint l, GLint v)) \
    X(void, glUniform3f, (GLint l, GLfloat x, GLfloat y, GLfloat z)) \
    X(void, glUniform4f, (GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w)) \
    X(void, glUniformMatrix4fv, (GLint l, GLsizei n, GLboolean t, const GLfloat* v)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* f)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* f)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint f)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum att, GLenum textarget, GLuint tex, GLint level)) \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum att, GLenum rbtarget, GLuint rb)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint* r)) \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint* r)) \
    X(void, glBindRenderbuffer, (GLenum target, GLuint r)) \
    X(void, glRenderbufferStorageMultisample, (GLenum target, GLsizei samples, GLenum fmt, GLsizei w, GLsizei h)) \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
    X(void, glGenTextures, (GLsizei n, GLuint* t)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* t)) \
    X(void, glBindTexture, (GLenum target, GLuint t)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* data)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glActiveTexture, (GLenum unit))

#define HD_GL_DECLARE(ret, name, args) typedef ret(HD_APIENTRY* PFN_hd_##name) args; extern PFN_hd_##name hd_##name;
HD_GL_FUNCTIONS(HD_GL_DECLARE)
#undef HD_GL_DECLARE

// Route standard names to our pointers.
#define glGetString hd_glGetString
#define glGetError hd_glGetError
#define glGetIntegerv hd_glGetIntegerv
#define glViewport hd_glViewport
#define glClearColor hd_glClearColor
#define glClear hd_glClear
#define glEnable hd_glEnable
#define glDisable hd_glDisable
#define glBlendFunc hd_glBlendFunc
#define glDepthFunc hd_glDepthFunc
#define glDepthMask hd_glDepthMask
#define glPolygonOffset hd_glPolygonOffset
#define glDrawArrays hd_glDrawArrays
#define glReadPixels hd_glReadPixels
#define glPixelStorei hd_glPixelStorei
#define glGenBuffers hd_glGenBuffers
#define glDeleteBuffers hd_glDeleteBuffers
#define glBindBuffer hd_glBindBuffer
#define glBufferData hd_glBufferData
#define glGenVertexArrays hd_glGenVertexArrays
#define glDeleteVertexArrays hd_glDeleteVertexArrays
#define glBindVertexArray hd_glBindVertexArray
#define glEnableVertexAttribArray hd_glEnableVertexAttribArray
#define glDisableVertexAttribArray hd_glDisableVertexAttribArray
#define glVertexAttribPointer hd_glVertexAttribPointer
#define glVertexAttrib3f hd_glVertexAttrib3f
#define glVertexAttrib1f hd_glVertexAttrib1f
#define glCreateShader hd_glCreateShader
#define glShaderSource hd_glShaderSource
#define glCompileShader hd_glCompileShader
#define glGetShaderiv hd_glGetShaderiv
#define glGetShaderInfoLog hd_glGetShaderInfoLog
#define glDeleteShader hd_glDeleteShader
#define glCreateProgram hd_glCreateProgram
#define glAttachShader hd_glAttachShader
#define glBindAttribLocation hd_glBindAttribLocation
#define glLinkProgram hd_glLinkProgram
#define glGetProgramiv hd_glGetProgramiv
#define glGetProgramInfoLog hd_glGetProgramInfoLog
#define glDeleteProgram hd_glDeleteProgram
#define glUseProgram hd_glUseProgram
#define glGetUniformLocation hd_glGetUniformLocation
#define glUniform1f hd_glUniform1f
#define glUniform1i hd_glUniform1i
#define glUniform3f hd_glUniform3f
#define glUniform4f hd_glUniform4f
#define glUniformMatrix4fv hd_glUniformMatrix4fv
#define glGenFramebuffers hd_glGenFramebuffers
#define glDeleteFramebuffers hd_glDeleteFramebuffers
#define glBindFramebuffer hd_glBindFramebuffer
#define glFramebufferTexture2D hd_glFramebufferTexture2D
#define glFramebufferRenderbuffer hd_glFramebufferRenderbuffer
#define glCheckFramebufferStatus hd_glCheckFramebufferStatus
#define glGenRenderbuffers hd_glGenRenderbuffers
#define glDeleteRenderbuffers hd_glDeleteRenderbuffers
#define glBindRenderbuffer hd_glBindRenderbuffer
#define glRenderbufferStorageMultisample hd_glRenderbufferStorageMultisample
#define glBlitFramebuffer hd_glBlitFramebuffer
#define glGenTextures hd_glGenTextures
#define glDeleteTextures hd_glDeleteTextures
#define glBindTexture hd_glBindTexture
#define glTexImage2D hd_glTexImage2D
#define glTexParameteri hd_glTexParameteri
#define glActiveTexture hd_glActiveTexture

namespace hd {
using GLGetProc = void* (*)(const char*);
// Loads all functions; returns false (and the first missing name) on failure.
bool loadGL(GLGetProc getProc, const char** missing = nullptr);
} // namespace hd
