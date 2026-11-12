#include "gfx.h"
#include <SDL3/SDL.h>
#include <cstdarg>
#include <mutex>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define X(r, n, a) r(APIENTRY* gl##n) a = nullptr;
GLFUNCS(X)
#undef X

void logMsg(const char* fmt, ...) {
    static std::mutex m;
    static FILE* f = fopen("worldridr.log", "w");
    std::lock_guard<std::mutex> lk(m);
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (f) { fprintf(f, "%s\n", buf); fflush(f); }
}

static const char* VS = R"(#version 330 core
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm; layout(location=2) in vec2 aUV; layout(location=3) in vec4 aCol;
uniform mat4 uMVP; uniform mat4 uModel; uniform float uBias;
out vec3 vW; out vec3 vN; out vec2 vUV; out vec4 vC;
void main(){
  vec4 w=uModel*vec4(aPos,1.0); mat3 m=mat3(uModel);
  vW=w.xyz; vN=m*(aNrm/vec3(dot(m[0],m[0]),dot(m[1],m[1]),dot(m[2],m[2]))); vUV=aUV; vC=aCol;
  gl_Position=uMVP*w; gl_Position.z-=uBias*gl_Position.w;
}
)";

static const char* FS = R"(#version 330 core
in vec3 vW; in vec3 vN; in vec2 vUV; in vec4 vC;
uniform int uMode; uniform int uFlags; uniform sampler2D uTex; uniform vec3 uCam; uniform vec3 uSun; uniform mat4 uInvVP; uniform vec3 uTint; uniform float uTile;
out vec4 o;
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float vnoise(vec2 p){
  vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
  return mix(mix(hash(i),hash(i+vec2(1,0)),f.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x), f.y);
}
vec3 sky(vec3 d){
  float t=clamp(d.y,0.0,1.0);
  vec3 c=mix(vec3(0.78,0.86,0.94),vec3(0.22,0.45,0.82),pow(t,0.45));
  float s=max(dot(d,uSun),0.0);
  return c+vec3(1.0,0.9,0.7)*(pow(s,350.0)*1.5+pow(s,8.0)*0.12);
}
vec3 fog(vec3 c){
  vec3 v=vW-uCam; float d=length(v); float f=1.0-exp(-d*0.00012);
  vec3 dir=v/max(d,0.001);
  return mix(c, sky(normalize(vec3(dir.x,0.02,dir.z))), f);
}
void main(){
  if(uMode==6){ vec4 p=uInvVP*vec4(vW.xy,1.0,1.0); vec3 d=normalize(p.xyz/p.w-uCam); if(d.y<0.0) d=normalize(vec3(d.x,0.0,d.z)); o=vec4(sky(d),1.0); return; }
  if(uMode==5){ o=vec4(vC.rgb, vC.a*texture(uTex,vUV).r); return; }
  if(uMode==4){ o=vec4(texture(uTex,vUV).rgb*uTint,1.0); return; }
  if(uMode==8){ o=texture(uTex,vUV)*vec4(uTint,1.0); return; }
  vec3 nn=normalize(vN);
  float l=0.45+0.13*nn.y+0.52*max(dot(nn,uSun),0.0);
  if(uMode==7){ // Editor-Objekte: Textur als Wuerfelprojektion in Weltkoordinaten
    vec3 an=abs(nn);
    vec2 uv=(an.y>an.x && an.y>an.z) ? vW.xz : (an.x>an.z ? vW.zy : vW.xy);
    o=vec4(fog(texture(uTex,uv/uTile).rgb*uTint*l),1.0); return;
  }
  if(uMode==1){
    // Satellitenbild + feines Detailrauschen in der Naehe (das Luftbild hat nur ~0,4 m pro Pixel)
    vec3 c=texture(uTex,vUV).rgb;
    float d=length(vW-uCam);
    float n=vnoise(vW.xz*2.3)*0.5+vnoise(vW.xz*9.0)*0.35+vnoise(vW.xz*31.0)*0.15;
    c*=mix(1.0, 0.80+0.40*n, clamp(1.0-d/90.0,0.0,1.0));
    o=vec4(fog(c),1.0); return;
  }
  if(uMode==2){
    float lat=vUV.x, along=vUV.y; float hw=vC.a*25.5;
    int lanes=int(vC.r*255.0+0.5); int fl=int(vC.g*255.0+0.5); int klass=int(vC.b*255.0+0.5);
    float a=smoothstep(hw, hw-0.3, abs(lat));
    float nz=vnoise(vW.xz*14.0)*0.6+vnoise(vW.xz*1.7)*0.4;
    vec3 c=vec3(0.20+0.05*nz);
    if(klass>=7) c=vec3(0.42,0.38,0.30)+0.04*nz;
    float line=0.0;
    if((fl&1)!=0 && lanes>=2){
      float lw=2.0*hw/float(lanes); float x=(lat+hw)/lw; float k=floor(x+0.5);
      if(k>=1.0 && k<=float(lanes)-0.5 && abs(x-k)*lw<0.07){
        bool center=((fl&4)==0) && (abs(k*2.0-float(lanes))<0.1);
        if((center && klass<=2) || mod(along,9.0)<3.0) line=1.0;
      }
    }
    if((fl&2)!=0 && abs(abs(lat)-(hw-0.3))<0.06) line=1.0;
    line*=clamp(1.6-length(vW-uCam)/220.0,0.0,1.0);
    c=mix(c,vec3(0.86),line*0.9);
    o=vec4(fog(c),a); return;
  }
  if(uMode==3){
    vec3 c=vC.rgb;
    if(vC.a>0.5){
      vec2 g=vec2(fract(vUV.x/2.8), fract(vUV.y/3.1));
      float win=step(0.22,g.x)*step(g.x,0.78)*step(0.28,g.y)*step(g.y,0.80);
      float id=hash(floor(vec2(vUV.x/2.8, vUV.y/3.1))+floor(vW.xz*0.05));
      c=mix(c, mix(vec3(0.10,0.13,0.18), vec3(0.35,0.42,0.52), id*0.6), win);
    }
    o=vec4(fog(c*l),1.0); return;
  }
  vec3 c=vC.rgb*uTint;
  if((uFlags&1)!=0) c*=l;
  if((uFlags&4)!=0 && dot(nn,vW-uCam)>0.0){ c=vec3(0.13,0.13,0.14)*(vC.a<0.99?0.0:1.0); if(vC.a<0.99){ o=vec4(0.0); return; } }
  else if((uFlags&4)!=0){ // Lack/Glas: Himmelsspiegelung + Glanzlicht
    vec3 v=normalize(vW-uCam); vec3 r=reflect(v,nn);
    float fr=0.05+0.55*pow(1.0-max(dot(-v,nn),0.0),4.0);
    c+=sky(normalize(vec3(r.x,abs(r.y)+0.02,r.z)))*fr*0.55+vec3(1.0,0.95,0.85)*pow(max(dot(r,uSun),0.0),70.0)*0.7;
  }
  if((uFlags&2)!=0) c=fog(c);
  o=vec4(c,vC.a);
}
)";

static struct {
    GLuint prog = 0, dynVao = 0, dynVbo = 0;
    GLint uMVP, uMode, uFlags, uTex, uCam, uSun, uInvVP, uBias, uTint, uModel, uTile;
    float maxAniso = 1;
} G;

static void attribs() {
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, c));
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); logMsg("Shader-Fehler: %s", log); }
    return s;
}

bool gfxInit() {
    bool ok = true;
#define X(r, n, a) gl##n = (r(APIENTRY*) a)SDL_GL_GetProcAddress("gl" #n); if (!gl##n) { logMsg("GL-Funktion fehlt: gl" #n); ok = false; }
    GLFUNCS(X)
#undef X
    if (!ok) return false;
    G.prog = glCreateProgram();
    glAttachShader(G.prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(G.prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(G.prog);
    GLint linked = 0;
    glGetProgramiv(G.prog, GL_LINK_STATUS, &linked);
    if (!linked) { char log[2048]; glGetProgramInfoLog(G.prog, sizeof log, nullptr, log); logMsg("Link-Fehler: %s", log); return false; }
    glUseProgram(G.prog);
    G.uMVP = glGetUniformLocation(G.prog, "uMVP"); G.uMode = glGetUniformLocation(G.prog, "uMode");
    G.uFlags = glGetUniformLocation(G.prog, "uFlags"); G.uTex = glGetUniformLocation(G.prog, "uTex");
    G.uCam = glGetUniformLocation(G.prog, "uCam"); G.uSun = glGetUniformLocation(G.prog, "uSun");
    G.uInvVP = glGetUniformLocation(G.prog, "uInvVP"); G.uBias = glGetUniformLocation(G.prog, "uBias");
    G.uTint = glGetUniformLocation(G.prog, "uTint");
    G.uModel = glGetUniformLocation(G.prog, "uModel"); G.uTile = glGetUniformLocation(G.prog, "uTile");
    gfxModel(identity());
    glUniform1f(G.uTile, 2.0f);
    glUniform1i(G.uTex, 0);
    Vec3 sun = normalize({0.45f, 0.75f, 0.35f});
    glUniform3f(G.uSun, sun.x, sun.y, sun.z);
    glUniform3f(G.uTint, 1, 1, 1);
    glGenVertexArrays(1, &G.dynVao); glBindVertexArray(G.dynVao);
    glGenBuffers(1, &G.dynVbo); glBindBuffer(GL_ARRAY_BUFFER, G.dynVbo);
    attribs();
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &G.maxAniso);
    if (glGetError() != GL_NO_ERROR || G.maxAniso < 1) G.maxAniso = 1;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    return true;
}

void gfxMode(int mode, int flags) { glUniform1i(G.uMode, mode); glUniform1i(G.uFlags, flags); glUniform3f(G.uTint, 1, 1, 1); }
void gfxModel(const Mat4& m) { glUniformMatrix4fv(G.uModel, 1, GL_FALSE, m.m); }
void gfxTile(float metres) { glUniform1f(G.uTile, metres); }
GLuint gfxTextureFile(const std::string& path) {
    int w, h, c;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &c, 3);
    if (!px) return 0;
    GLuint t = gfxTexture(px, w, h, 3, true);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_image_free(px);
    return t;
}
void Batch::xform(size_t from, const Mat4& m) {
    for (size_t i = from; i < v.size(); i++) {
        Vec3 p = xformPoint(m, {v[i].p[0], v[i].p[1], v[i].p[2]}), n = normalize(xformDir(m, {v[i].n[0], v[i].n[1], v[i].n[2]}));
        v[i].p[0] = p.x; v[i].p[1] = p.y; v[i].p[2] = p.z;
        v[i].n[0] = n.x; v[i].n[1] = n.y; v[i].n[2] = n.z;
    }
}
void Batch::rrect(float x, float y, float w, float h, float r, Color col) {
    r = std::min(r, std::min(w, h) * 0.5f);
    rect(x + r, y, w - 2 * r, h, col);
    rect(x, y + r, r, h - 2 * r, col);
    rect(x + w - r, y + r, r, h - 2 * r, col);
    const float cxs[4] = {x + r, x + w - r, x + w - r, x + r}, cys[4] = {y + r, y + r, y + h - r, y + h - r};
    for (int q = 0; q < 4; q++)
        for (int i = 0; i < 5; i++) {
            float a0 = PI + q * PI / 2 + i * PI / 10, a1 = a0 + PI / 10;
            tri({cxs[q], cys[q], 0}, {cxs[q] + std::cos(a0) * r, cys[q] + std::sin(a0) * r, 0}, {cxs[q] + std::cos(a1) * r, cys[q] + std::sin(a1) * r, 0}, {0, 0, 1}, col);
        }
}
void gfxMVP(const Mat4& mvp) { glUniformMatrix4fv(G.uMVP, 1, GL_FALSE, mvp.m); }
void gfxCamera(Vec3 cam, const Mat4& inv) { glUniform3f(G.uCam, cam.x, cam.y, cam.z); glUniformMatrix4fv(G.uInvVP, 1, GL_FALSE, inv.m); }
void gfxBias(float b) { glUniform1f(G.uBias, b); }
void gfxTint(float r, float g, float b) { glUniform3f(G.uTint, r, g, b); }
void gfxBindTexture(GLuint t) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t); }

GLuint gfxTexture(const uint8_t* px, int w, int h, int comp, bool mips) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    GLenum fmt = comp == 1 ? GL_RED : (comp == 3 ? GL_RGB : GL_RGBA);
    GLenum ifmt = comp == 1 ? GL_R8 : (comp == 3 ? GL_RGB8 : GL_RGBA8);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (mips) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, std::min(G.maxAniso, 8.0f));
    } else
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    return t;
}

void Mesh::create(const Vertex* v, int n) {
    destroy();
    if (n <= 0) return;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof(Vertex), v, GL_STATIC_DRAW);
    attribs();
    count = n;
}
void Mesh::destroy() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    vao = vbo = 0; count = 0;
}
void Mesh::draw() const {
    if (!count) return;
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, count);
}

void Batch::vert(Vec3 p, Vec3 n, float u, float w, Color c) { v.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {u, w}, c}); }
void Batch::tri(Vec3 a, Vec3 b, Vec3 c, Vec3 n, Color col) { vert(a, n, 0, 0, col); vert(b, n, 1, 0, col); vert(c, n, 1, 1, col); }
void Batch::quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, Color col) {
    vert(a, n, 0, 0, col); vert(b, n, 1, 0, col); vert(c, n, 1, 1, col);
    vert(a, n, 0, 0, col); vert(c, n, 1, 1, col); vert(d, n, 0, 1, col);
}
void Batch::box(const Mat4& m, Vec3 c, Vec3 h, Color col) {
    Vec3 p[8];
    for (int i = 0; i < 8; i++) p[i] = xformPoint(m, {c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z)});
    Vec3 nx = normalize(xformDir(m, {1, 0, 0})), ny = normalize(xformDir(m, {0, 1, 0})), nz = normalize(xformDir(m, {0, 0, 1}));
    quad(p[1], p[3], p[7], p[5], nx, col); quad(p[0], p[4], p[6], p[2], -nx, col);
    quad(p[2], p[6], p[7], p[3], ny, col); quad(p[0], p[1], p[5], p[4], -ny, col);
    quad(p[4], p[5], p[7], p[6], nz, col); quad(p[0], p[2], p[3], p[1], -nz, col);
}
void Batch::beam(const Mat4& m, Vec3 a, Vec3 b, float t, Color col) {
    Vec3 d = normalize(b - a);
    Vec3 ref = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 s = normalize(cross(d, ref)) * t, u = normalize(cross(s, d)) * t;
    Vec3 A[4] = {a - s - u, a + s - u, a + s + u, a - s + u}, B[4] = {b - s - u, b + s - u, b + s + u, b - s + u};
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        Vec3 n = normalize(xformDir(m, (A[i] + A[j]) * 0.5f - a));
        quad(xformPoint(m, A[i]), xformPoint(m, A[j]), xformPoint(m, B[j]), xformPoint(m, B[i]), n, col);
    }
}
void Batch::cylinderX(const Mat4& m, Vec3 c, float radius, float halfLen, float rot, Color side, Color cap) {
    const int N = 16;
    Vec3 nL = normalize(xformDir(m, {-1, 0, 0}));
    for (int i = 0; i < N; i++) {
        float a0 = rot + i * 2 * PI / N, a1 = rot + (i + 1) * 2 * PI / N;
        Vec3 o0{0, std::cos(a0) * radius, std::sin(a0) * radius}, o1{0, std::cos(a1) * radius, std::sin(a1) * radius};
        Vec3 l0 = xformPoint(m, c + o0 + Vec3{-halfLen, 0, 0}), l1 = xformPoint(m, c + o1 + Vec3{-halfLen, 0, 0});
        Vec3 r0 = xformPoint(m, c + o0 + Vec3{halfLen, 0, 0}), r1 = xformPoint(m, c + o1 + Vec3{halfLen, 0, 0});
        quad(l0, r0, r1, l1, normalize(xformDir(m, o0 + o1)), side);
        Color cc = (i % 4 == 0) ? Color{150, 150, 155, 255} : cap; // Speichen-Andeutung, damit man die Drehung sieht
        tri(xformPoint(m, c + Vec3{-halfLen, 0, 0}), l0, l1, nL, cc);
        tri(xformPoint(m, c + Vec3{halfLen, 0, 0}), r1, r0, -nL, cc);
    }
}
void Batch::rect(float x, float y, float w, float h, Color col) {
    quad({x, y, 0}, {x + w, y, 0}, {x + w, y + h, 0}, {x, y + h, 0}, {0, 0, 1}, col);
}
void Batch::disc(float cx, float cy, float r, Color col) {
    const int N = 28;
    for (int i = 0; i < N; i++) {
        float a0 = i * 2 * PI / N, a1 = (i + 1) * 2 * PI / N;
        tri({cx, cy, 0}, {cx + std::cos(a0) * r, cy + std::sin(a0) * r, 0}, {cx + std::cos(a1) * r, cy + std::sin(a1) * r, 0}, {0, 0, 1}, col);
    }
}
void Batch::draw() const {
    if (v.empty()) return;
    glBindVertexArray(G.dynVao);
    glBindBuffer(GL_ARRAY_BUFFER, G.dynVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)v.size() * sizeof(Vertex), v.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v.size());
}

static stbtt_bakedchar g_chars[224];
static const float FONT_PX = 44.0f;

bool Font::load() {
    const char* paths[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\tahoma.ttf"};
    std::vector<uint8_t> data;
    for (const char* p : paths) {
        FILE* f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        data.resize(n);
        fread(data.data(), 1, n, f);
        fclose(f);
        break;
    }
    if (data.empty()) { logMsg("Keine Schriftart gefunden"); return false; }
    std::vector<uint8_t> bmp(1024 * 1024);
    stbtt_BakeFontBitmap(data.data(), 0, FONT_PX, bmp.data(), 1024, 1024, 32, 224, g_chars);
    tex = gfxTexture(bmp.data(), 1024, 1024, 1, false);
    return true;
}

// UTF-8 -> Latin-1 Codepunkte (alles andere wird '?')
static int nextCp(const std::string& s, size_t& i) {
    unsigned char c = s[i++];
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
    int cp = c & (0x3F >> n);
    while (n-- > 0 && i < s.size()) cp = (cp << 6) | (s[i++] & 0x3F);
    return (cp >= 32 && cp < 256) ? cp : '?';
}
float Font::width(const std::string& s, float px) const {
    float w = 0;
    for (size_t i = 0; i < s.size();) { int cp = nextCp(s, i); if (cp >= 32) w += g_chars[cp - 32].xadvance; }
    return w * px / FONT_PX;
}
void Font::draw(Batch& b, float x, float y, float px, Color c, const std::string& s) const {
    float k = px / FONT_PX;
    for (size_t i = 0; i < s.size();) {
        int cp = nextCp(s, i);
        if (cp < 32) continue;
        const stbtt_bakedchar& g = g_chars[cp - 32];
        float x0 = x + g.xoff * k, y0 = y + g.yoff * k, x1 = x0 + (g.x1 - g.x0) * k, y1 = y0 + (g.y1 - g.y0) * k;
        float u0 = g.x0 / 1024.0f, v0 = g.y0 / 1024.0f, u1 = g.x1 / 1024.0f, v1 = g.y1 / 1024.0f;
        Vec3 n{0, 0, 1};
        b.vert({x0, y0, 0}, n, u0, v0, c); b.vert({x1, y0, 0}, n, u1, v0, c); b.vert({x1, y1, 0}, n, u1, v1, c);
        b.vert({x0, y0, 0}, n, u0, v0, c); b.vert({x1, y1, 0}, n, u1, v1, c); b.vert({x0, y1, 0}, n, u0, v1, c);
        x += g.xadvance * k;
    }
}

void Target::create(int w_, int h_) {
    w = w_; h = h_;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
