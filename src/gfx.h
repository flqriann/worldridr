#pragma once
#include "common.h"
#include <windows.h>
#include <GL/gl.h>
#include <cstddef>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGBA8 0x8058
#define GL_RGB8 0x8051
#define GL_R8 0x8229
#define GL_MULTISAMPLE 0x809D

#define GLFUNCS(X) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
    X(void, GenVertexArrays, (GLsizei, GLuint*)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*)) \
    X(void, GenBuffers, (GLsizei, GLuint*)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint*)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, VertexAttribDivisor, (GLuint, GLuint)) \
    X(void, DrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar*)) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, GenerateMipmap, (GLenum)) \
    X(void, GenFramebuffers, (GLsizei, GLuint*)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint*)) \
    X(void, BindRenderbuffer, (GLenum, GLuint)) \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint*)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))

#define X(r, n, a) extern r(APIENTRY* gl##n) a;
GLFUNCS(X)
#undef X

struct Color { uint8_t r = 255, g = 255, b = 255, a = 255; };
struct Vertex { float p[3]; float n[3]; float uv[2]; Color c; };

struct Mesh {
    GLuint vao = 0, vbo = 0;
    int count = 0;
    void create(const Vertex* v, int n);
    void destroy();
    void draw() const;
    void drawInstanced(GLuint instVbo, int n) const; // pro Instanz vec4 (x, y, z, Skalierung); gfxInstanced(true) vorher
};
GLuint gfxInstBuffer(GLuint vbo, const float* v4, int n); // Instanzdaten hochladen, liefert ggf. neu erzeugten Puffer
void gfxInstanced(bool on);

// Dynamischer Dreiecks-Puffer fuer Auto, Schilder, HUD
struct Batch {
    std::vector<Vertex> v;
    void clear() { v.clear(); }
    void vert(Vec3 p, Vec3 n, float u, float w, Color c);
    void tri(Vec3 a, Vec3 b, Vec3 c, Vec3 n, Color col);
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, Color col); // a,b,c,d umlaufend; uv 0..1
    void box(const Mat4& m, Vec3 c, Vec3 h, Color col);
    void beam(const Mat4& m, Vec3 a, Vec3 b, float t, Color col);
    void cylinderX(const Mat4& m, Vec3 c, float radius, float halfLen, float rot, Color side, Color cap);
    void rect(float x, float y, float w, float h, Color col);
    void rrect(float x, float y, float w, float h, float r, Color col);
    void xform(size_t from, const Mat4& m); // Vertices ab Index transformieren
    void disc(float cx, float cy, float r, Color col);
    void draw() const;
};

struct Font {
    GLuint tex = 0;
    bool load();
    float width(const std::string& s, float px) const;
    void draw(Batch& b, float x, float y, float px, Color c, const std::string& s) const;
};

struct Target { // Render-Ziel fuer Spiegel
    GLuint fbo = 0, tex = 0, depth = 0;
    int w = 0, h = 0;
    void create(int w, int h);
    void destroy();
};

// Modi: 0 Farbe, 1 Gelaende (Satellitenbild), 2 Strasse, 3 Gebaeude, 4 Textur, 5 Text, 6 Himmel
// 7 = Editor-Objekt (Textur in Weltkoordinaten, Farbe ueber gfxTint)
enum { FLAG_LIT = 1, FLAG_FOG = 2, FLAG_GLOSS = 4 };
bool gfxInit();
void gfxModel(const Mat4& m);
void gfxTile(float metres);
GLuint gfxTextureFile(const std::string& path);
void gfxMode(int mode, int flags = 0);
void gfxMVP(const Mat4& mvp);
void gfxCamera(Vec3 cam, const Mat4& invViewProj);
void gfxBias(float b);
void gfxEnv(float day, float overcast, float fogK, float wet, float snow, Vec3 sun);   // Tageslicht, Bewoelkung, Nebeldichte, Naesse, Schnee
void gfxHead(Vec3 pos, Vec3 dir, float on);                                              // Scheinwerferkegel des eigenen Autos
void gfxTint(float r, float g, float b);
void gfxQuality(int q);   // 0 = einfache Shader (kein Detailrauschen), 1+ = volle Details
GLuint gfxTexture(const uint8_t* px, int w, int h, int comp, bool mips);
void gfxBindTexture(GLuint t);
