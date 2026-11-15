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
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm; layout(location=2) in vec2 aUV; layout(location=3) in vec4 aCol; layout(location=4) in vec4 aInst;
uniform mat4 uMVP; uniform mat4 uModel; uniform float uBias; uniform int uInst; uniform vec4 uSink;
out vec3 vW; out vec3 vN; out vec2 vUV; out vec4 vC;
void main(){
  vec3 lp=aPos, ln=aNrm; vec4 col=aCol;
  if(uInst==1){ // Instanzen (Baeume): zufaellige Drehung und leichte Farbvariation aus der Position
    float h=fract(sin(dot(aInst.xz,vec2(12.9898,78.233)))*43758.5453); float a=h*6.2832, c=cos(a), s=sin(a);
    lp=vec3(aPos.x*c+aPos.z*s, aPos.y, -aPos.x*s+aPos.z*c)*aInst.w+aInst.xyz;
    ln=vec3(aNrm.x*c+aNrm.z*s, aNrm.y, -aNrm.x*s+aNrm.z*c);
    col.rgb*=0.82+0.3*fract(h*13.7);
  }
  vec4 w=uModel*vec4(lp,1.0); mat3 m=mat3(uModel);
  // grobes Gelaende unter dem feinen absenken (rund um die Kamera), sonst ragt es ueber die Kamera und verdeckt den Himmel
  if(uSink.z>0.0){ float sd=distance(w.xz,uSink.xy); w.y-=uSink.w*(1.0-smoothstep(uSink.z*0.55,uSink.z,sd)); }
  vW=w.xyz; vN=m*(ln/vec3(dot(m[0],m[0]),dot(m[1],m[1]),dot(m[2],m[2]))); vUV=aUV; vC=col;
  gl_Position=uMVP*w; if(uBias>0.0){ float sw=uBias*3000.0; sw+=gl_Position.w*0.0004; gl_Position.z-=sw*0.4/max(gl_Position.w,0.1); }
}
)";

static const char* FS = R"(#version 330 core
in vec3 vW; in vec3 vN; in vec2 vUV; in vec4 vC;
uniform int uQ; uniform int uMode; uniform int uFlags; uniform sampler2D uTex; uniform vec3 uCam; uniform vec3 uSun; uniform mat4 uInvVP; uniform vec3 uTint; uniform float uTile;
uniform vec4 uEnv; uniform float uSnow; uniform vec3 uHP; uniform vec3 uHD; uniform float uHOn;
out vec4 o;
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float vnoise(vec2 p){
  vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
  return mix(mix(hash(i),hash(i+vec2(1,0)),f.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x), f.y);
}
vec3 sky(vec3 d){
  float day=uEnv.x, oc=uEnv.y;
  float t=clamp(d.y,0.0,1.0);
  vec3 zen=mix(vec3(0.006,0.012,0.04),vec3(0.22,0.45,0.82),day);
  vec3 hor=mix(vec3(0.02,0.03,0.07),vec3(0.78,0.86,0.94),day);
  float tw=1.0-smoothstep(0.0,0.30,abs(uSun.y+0.02));
  vec2 sa=normalize(uSun.xz+vec2(1e-4)), da=normalize(d.xz+vec2(1e-4));
  float az=pow(max(dot(da,sa),0.0),2.0);
  hor=mix(hor,vec3(1.0,0.50,0.22),tw*(0.30+0.55*az));
  zen=mix(zen,vec3(0.16,0.20,0.40),tw*0.5);
  vec3 c=mix(hor,zen,pow(t,0.45));
  vec3 grey=mix(vec3(0.015,0.017,0.025),vec3(0.62,0.65,0.69),day)*(1.0-0.30*uEnv.w);
  c=mix(c,grey,oc*0.9);
  float s=max(dot(d,uSun),0.0);
  c+=vec3(1.0,0.9,0.7)*(pow(s,350.0)*1.5+pow(s,8.0)*0.12)*(1.0-oc)*smoothstep(-0.12,0.04,uSun.y);
  float m=max(dot(d,-uSun),0.0);
  c+=vec3(0.85,0.9,1.0)*pow(m,1800.0)*2.2*(1.0-day)*(1.0-oc);
  vec3 sd=floor(d*240.0); float hs=hash(sd.xy+sd.z*7.3);
  c+=vec3(1.0)*step(0.9965,hs)*(1.0-day)*(1.0-oc)*smoothstep(0.02,0.25,d.y)*(0.45+0.55*hash(sd.yz));
  return c;
}
float headLight(vec3 w, vec3 n){
  if(uHOn<0.5) return 0.0;
  vec3 v=w-uHP; float d=length(v); if(d>115.0||d<0.3) return 0.0;
  vec3 dir=v/d; float cn=dot(dir,uHD);
  float cone=smoothstep(0.80,0.95,cn);
  float fall=1.0/(1.0+d*d*0.0035)*(1.0-smoothstep(75.0,115.0,d));
  float ndl=max(dot(n,-dir),0.0)*0.6+0.4;
  return cone*fall*ndl*2.4*uHOn;
}
float envLight(){ return mix(0.17,1.0,uEnv.x)*(1.0-0.20*uEnv.y); }
vec3 fog(vec3 c){
  vec3 v=vW-uCam; float d=length(v); float f=1.0-exp(-d*0.00026*uEnv.z); f=max(f, smoothstep(4000.0,12000.0,d));
  vec3 dir=v/max(d,0.001);
  return mix(c, sky(normalize(vec3(dir.x,0.02,dir.z))), f);
}
void main(){
  if(uMode==6){ vec4 p=uInvVP*vec4(vW.xy,1.0,1.0); vec3 d=p.xyz/(abs(p.w)>1e-6?p.w:1.0); if(!(dot(d,d)>1e-8)) d=vec3(0.0,0.3,1.0); d=normalize(d); if(d.y<0.0) d=normalize(vec3(d.x,0.0,d.z)+vec3(0.0,1e-4,0.0)); o=vec4(sky(d),1.0); return; }
  if(uMode==5){ o=vec4(vC.rgb, vC.a*texture(uTex,vUV).r); return; }
  if(uMode==4){ o=vec4(texture(uTex,vUV).rgb*uTint,1.0); return; }
  if(uMode==8){ o=texture(uTex,vUV)*vec4(uTint,1.0); return; }
  vec3 nn=normalize(vN);
  float hl=headLight(vW,nn);
  float l=mix(0.15,0.45,uEnv.x)*(1.0-0.18*uEnv.y)+0.13*nn.y*uEnv.x+0.52*uEnv.x*(1.0-0.65*uEnv.y)*max(dot(nn,uSun),0.0)+hl;
  if(uMode==7 || uMode==9){ // Editor-Objekte: Textur als Wuerfelprojektion in Weltkoordinaten (9: mal Flaechenfarbe)
    vec3 an=abs(nn);
    vec2 uv=(an.y>an.x && an.y>an.z) ? vW.xz : (an.x>an.z ? vW.zy : vW.xy);
    vec3 fc=(uMode==9) ? vC.rgb : vec3(1.0);
    o=vec4(fog(texture(uTex,uv/uTile).rgb*uTint*fc*l),1.0); return;
  }
  if(uMode==1){
    // Satellitenbild + feines Detailrauschen in der Naehe (das Luftbild hat nur ~0,4 m pro Pixel)
    vec3 c=texture(uTex,vUV).rgb;
    float d=length(vW-uCam);
    if(uQ>0){ float n=vnoise(vW.xz*2.3)*0.5+vnoise(vW.xz*9.0)*0.35+vnoise(vW.xz*31.0)*0.15;
    c*=mix(1.0, 0.80+0.40*n, clamp(1.0-d/90.0,0.0,1.0)); }
    // Hangschummerung aus dem Hoehenmodell: Sonnenseiten heller, Schattenhaenge dunkler (Gelaenderelief)
    c*=mix(1.0, (0.58+0.62*max(dot(nn,uSun),0.0))/1.03, 0.9);
    c=mix(c, vec3(0.86,0.88,0.92)*(0.55+0.45*nn.y), uSnow*0.62);
    c*=envLight()*mix(1.0,0.82,uEnv.w)+hl*0.9;
    o=vec4(fog(c),1.0); return;
  }
  if(uMode==2){
    float lat=vUV.x, along=vUV.y; float hw=vC.a*25.5;
    int lanes=int(vC.r*255.0+0.5); int fl=int(vC.g*255.0+0.5); int klass=int(vC.b*255.0+0.5);
    float lanesF=vN.x>0.5 ? vN.x : float(lanes);   // Spurzahl je Pixel (im Keil stetig)
    if(klass==8){ // Mittelinsel eines Kreisverkehrs: Rasen mit Bordstein
      float inside=-lat;
      float aa=smoothstep(-0.02,0.12,inside);
      vec3 grass=vec3(0.26,0.40,0.20)+0.06*vnoise(vW.xz*0.9)+0.03*vnoise(vW.xz*6.0);
      float cb=smoothstep(0.42,0.34,inside);
      o=vec4(fog(mix(grass,vec3(0.72,0.71,0.68),cb)),aa); return;
    }
    if(klass==9){ // Fahrbahnmarkierung (Pfeile): weiss, abgenutzt, in der Ferne ausgeblendet
      float dd=length(vW-uCam);
      float wr=uQ>0 ? 0.80+0.20*vnoise(vW.xz*6.0) : 0.9;
      o=vec4(fog(vec3(0.88,0.88,0.84)*wr), 0.92*clamp(1.6-dd/200.0,0.0,1.0)); return;
    }
    bool sw=(fl&8)!=0, swOnly=(fl&16)!=0;
    float al=abs(lat);
    float dist=length(vW-uCam);
    float nearF=uQ>0 ? clamp(1.0-dist/70.0,0.0,1.0) : 0.0;
    // Ausdehnung des Bandes: Gehsteigband bis hw+1.9, Asphaltband bis hw (+0.15 Randstein)
    float outer=swOnly ? hw+1.9 : (sw ? hw+0.15 : hw);
    float a=smoothstep(outer, outer-0.22, al);
    if(swOnly) a*=smoothstep(hw-0.03, hw+0.03, al);
    float nz=uQ>0 ? vnoise(vW.xz*14.0)*0.6+vnoise(vW.xz*1.7)*0.4 : 0.5;
    vec3 c;
    if(al>hw){
      // Gehsteig: helle Platten mit Fugen, Randstein-Kante
      float slab=uQ>0 ? vnoise(floor(vec2(along,al)*1.25)*0.7)*0.06 : 0.0;
      c=vec3(0.60,0.58,0.55)+slab+0.03*nz;
      float j=max(step(0.965,fract(along*1.25)), step(0.965,fract(al*1.25)));
      c=mix(c, c*0.78, j*nearF*0.8);
      float kerb=smoothstep(hw+0.15, hw+0.05, al);
      c=mix(c, vec3(0.74,0.73,0.71), kerb*step(hw,al));
    } else {
      float fine=(vnoise(vW.xz*38.0)-0.5)*0.05*nearF;
      c=vec3(0.19+0.05*nz+fine);
      if(klass>=7) c=vec3(0.42,0.38,0.30)+0.05*nz+fine;
      // Spurrillen und Fahrbahnflecken
      float lw0=2.0*hw/max(lanesF,1.0); float xl=(lat+hw)/lw0; float dc=(fract(xl)-0.5)*lw0;
      float track=smoothstep(0.38,0.0,abs(abs(dc)-0.85))*step(2.0,lw0)*(klass<=5?1.0:0.0);
      c*=1.0-0.12*track*nearF;
      if(uQ>0) c*=0.92+0.10*vnoise(vW.xz*0.35);
      // Markierungen (weich, abgenutzt)
      float line=0.0;
      if((fl&1)!=0 && lanesF>=1.5){
        float lw=lw0; float x=(lat+hw)/lw; float k=floor(x+0.5);
        float d=abs(x-k)*lw;
        if(k>=1.0 && k<=lanesF-0.5){
          int cb=(fl>>5)&7;                                  // Spuren der Gegenrichtung (links), 0 = Mitte
          float cpos=cb>0 ? float(cb) : lanesF*0.5;
          bool center=((fl&4)==0) && (abs(k-cpos)<0.1);
          float dash=(center && klass<=2) ? 1.0 : smoothstep(0.0,0.15,3.0-mod(along,9.0))*smoothstep(0.0,0.15,mod(along,9.0));
          line=max(line, smoothstep(0.085,0.05,d)*dash);
        }
      }
      if((fl&2)!=0) line=max(line, smoothstep(0.085,0.05,abs(al-(hw-0.3))));
      float wear=uQ>0 ? 0.78+0.22*vnoise(vW.xz*6.0) : 0.9;
      line*=wear*clamp(1.6-dist/220.0,0.0,1.0);
      c=mix(c,vec3(0.88,0.88,0.84),line*0.92);
      c*=1.0-0.30*uEnv.w;
      c=mix(c, vec3(0.84,0.86,0.90), uSnow*0.55*(1.0-0.7*line));
    }
    c*=envLight()+hl*0.95;
    if(a<0.02) discard;
    o=vec4(fog(c),a); return;
  }
  if(uMode==3){
    vec3 c=vC.rgb; vec3 em=vec3(0.0);
    if(vC.a>0.5){
      bool gl=vC.a<0.85;
      vec2 g=vec2(fract(vUV.x/2.8), fract(vUV.y/3.1));
      float win=gl ? step(0.04,g.x)*step(g.x,0.96)*step(0.10,g.y)*step(g.y,0.90)
                   : step(0.22,g.x)*step(g.x,0.78)*step(0.28,g.y)*step(g.y,0.80);
      float id=hash(floor(vec2(vUV.x/2.8, vUV.y/3.1))+floor(vW.xz*0.05));
      vec3 vv=normalize(vW-uCam);
      float fr=0.25+0.5*pow(1.0-abs(dot(vv,normalize(vN))),2.0);
      vec3 glassC=gl ? mix(vec3(0.20,0.30,0.38), sky(normalize(vec3(vv.x,abs(vv.y)+0.15,vv.z))), fr) : mix(vec3(0.10,0.13,0.18), vec3(0.35,0.42,0.52), id*0.6);
      c=mix(c, glassC, win);
      em=vec3(1.0,0.82,0.52)*win*step(0.62,id)*(1.0-uEnv.x)*0.9;
    }
    o=vec4(fog(c*l+em),1.0); return;
  }
  vec3 c=vC.rgb*uTint;
  if((uFlags&1)!=0) c*=l;
  if((uFlags&4)!=0 && dot(nn,normalize(vW-uCam))>0.28){ c=vec3(0.13,0.13,0.14)*(vC.a<0.99?0.0:1.0); if(vC.a<0.99){ o=vec4(0.0); return; } }
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
    GLint uMVP, uMode, uFlags, uTex, uCam, uSun, uInvVP, uBias, uTint, uModel, uTile, uQ, uInst, uEnv, uSnow, uHP, uHD, uHOn, uSink;
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
    G.uTint = glGetUniformLocation(G.prog, "uTint"); G.uQ = glGetUniformLocation(G.prog, "uQ");
    G.uEnv = glGetUniformLocation(G.prog, "uEnv"); G.uSnow = glGetUniformLocation(G.prog, "uSnow");
    G.uSink = glGetUniformLocation(G.prog, "uSink"); glUniform4f(G.uSink, 0, 0, 0, 0);
    G.uHP = glGetUniformLocation(G.prog, "uHP"); G.uHD = glGetUniformLocation(G.prog, "uHD"); G.uHOn = glGetUniformLocation(G.prog, "uHOn");
    glUniform1i(G.uQ, 1);
    G.uModel = glGetUniformLocation(G.prog, "uModel"); G.uTile = glGetUniformLocation(G.prog, "uTile"); G.uInst = glGetUniformLocation(G.prog, "uInst");
    gfxModel(identity());
    glUniform1f(G.uTile, 2.0f);
    glUniform1i(G.uTex, 0);
    Vec3 sun = normalize({0.45f, 0.75f, 0.35f});
    glUniform3f(G.uSun, sun.x, sun.y, sun.z);
    glUniform3f(G.uTint, 1, 1, 1);
    glUniform4f(G.uEnv, 1, 0, 1, 0); glUniform1f(G.uSnow, 0); glUniform1f(G.uHOn, 0);
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
void gfxInstanced(bool on) { glUniform1i(G.uInst, on ? 1 : 0); }
void gfxQuality(int q) { glUniform1i(G.uQ, q); }
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
void gfxSink(float x, float z, float radius, float depth) { glUniform4f(G.uSink, x, z, radius, depth); }
void gfxEnv(float day, float overcast, float fogK, float wet, float snow, Vec3 sun) {
    glUniform4f(G.uEnv, day, overcast, fogK, wet); glUniform1f(G.uSnow, snow); glUniform3f(G.uSun, sun.x, sun.y, sun.z);
}
void gfxHead(Vec3 pos, Vec3 dir, float on) { glUniform3f(G.uHP, pos.x, pos.y, pos.z); glUniform3f(G.uHD, dir.x, dir.y, dir.z); glUniform1f(G.uHOn, on); }
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
void Mesh::drawInstanced(GLuint ivbo, int n) const {
    if (!count || !ivbo || n <= 0) return;
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, ivbo);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, 16, (void*)0); glVertexAttribDivisor(4, 1);
    glDrawArraysInstanced(GL_TRIANGLES, 0, count, n);
    glVertexAttribDivisor(4, 0); glDisableVertexAttribArray(4);
}
GLuint gfxInstBuffer(GLuint vbo, const float* v4, int n) {
    if (!vbo) glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)std::max(n, 1) * 16, v4, GL_STREAM_DRAW);
    return vbo;
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
        Color cc = (i == 0) ? Color{170, 170, 176, 255} : cap; // eine helle Markierung zeigt die Drehung
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

void Target::destroy() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (depth) glDeleteRenderbuffers(1, &depth);
    if (tex) glDeleteTextures(1, &tex);
    fbo = depth = tex = 0; w = h = 0;
}
void Target::create(int w_, int h_) {
    destroy();
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
