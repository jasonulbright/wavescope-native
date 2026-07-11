// GPU shader host: runs true per-pixel fragment-shader visuals on the GPU,
// the native counterpart of the web build's WebGPU engine (webgpu.ts in the
// web repo). Like the projectM host it lives in a child window with its own
// WGL/OpenGL 3.3 (compatibility) context layered over the Direct2D surface,
// with the shared D2D-rendered UI composited over it. Pulls in NO third-party
// code: all 17 shaders are GLSL 330 ports of the web WGSL, compiled at
// runtime. Pure GPU + OS.
//
// The WGSL->GLSL port is 1:1 on the math; only the boilerplate differs
// (uniform block -> individual uniforms, gl_VertexID fullscreen triangle,
// atan2 -> atan, textureLoad -> texelFetch). Besides the scalar audio
// uniforms, two 1-D textures update per frame — the 1024-bin FFT and the
// 2048-sample waveform — exposed through the same spec()/specLog()/wav()
// helpers the WGSL prelude defines. Keep the bodies in sync with webgpu.ts.

#include <GL/gl.h>
#include <string>

#pragma comment(lib, "opengl32.lib")

// WGL + GL entry points. Identical typedef/macro redefinitions across this and
// projectm_host.inl are legal in one TU, so no guards are needed.
typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int);
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#define GL_TEXTURE0        0x84C0
#define GL_R8              0x8229
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// GL 1.3+/2.0+/3.0 functions resolved at runtime (not in opengl32's 1.1 exports).
typedef GLuint(WINAPI* PFN_glCreateShader)(GLenum);
typedef void  (WINAPI* PFN_glShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void  (WINAPI* PFN_glCompileShader)(GLuint);
typedef void  (WINAPI* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void  (WINAPI* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
typedef void  (WINAPI* PFN_glDeleteShader)(GLuint);
typedef GLuint(WINAPI* PFN_glCreateProgram)(void);
typedef void  (WINAPI* PFN_glAttachShader)(GLuint, GLuint);
typedef void  (WINAPI* PFN_glLinkProgram)(GLuint);
typedef void  (WINAPI* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void  (WINAPI* PFN_glUseProgram2)(GLuint);
typedef GLint (WINAPI* PFN_glGetUniformLocation)(GLuint, const char*);
typedef void  (WINAPI* PFN_glUniform1f)(GLint, GLfloat);
typedef void  (WINAPI* PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void  (WINAPI* PFN_glUniform1i)(GLint, GLint);
typedef void  (WINAPI* PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void  (WINAPI* PFN_glBindVertexArray2)(GLuint);
typedef void  (WINAPI* PFN_glActiveTexture)(GLenum);

// Shared vertex shader: a single oversized triangle covering the viewport,
// positions synthesized from gl_VertexID (matches the WGSL vs()).
static const char* SV_VERT =
    "#version 330\n"
    "out vec2 vUv;\n"
    "void main(){\n"
    "  vec2 p = vec2((gl_VertexID==1)?3.0:-1.0, (gl_VertexID==2)?3.0:-1.0);\n"
    "  gl_Position = vec4(p,0.0,1.0);\n"
    "  vUv = p*0.5+0.5;\n"
    "}\n";

// Common fragment prelude: audio uniforms + the per-frame FFT/waveform
// textures with the same helpers as the WGSL prelude.
static const char* SV_FRAG_PRELUDE =
    "#version 330\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "uniform vec2 uRes;\n"
    "uniform float uTime,uLevel,uBass,uMid,uTreble,uBeat;\n"
    "uniform sampler1D uSpectrum;\n"   // 1024 texels, FFT magnitude 0..1
    "uniform sampler1D uWave;\n"       // 2048 texels, waveform biased 0..1
    "float spec(float x){ return texelFetch(uSpectrum, int(clamp(x,0.0,1.0)*1023.0), 0).r; }\n"
    "float specLog(float x){ float e = clamp(x,0.0,1.0); return spec(pow(e,2.6)*0.86); }\n"
    "float wav(float x){ return texelFetch(uWave, int(clamp(x,0.0,1.0)*2047.0), 0).r*2.0-1.0; }\n";

// The 17 fragment bodies, ported 1:1 from webgpu.ts GPU_MODES (same order).
static const char* SV_FRAG_BODY[] = {
    // 0: Plasma field
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float boost = 0.5 + uLevel*2.0;\n"
    "  float v = sin(p.x*6.0+t) + sin(p.y*6.0-t*1.2)\n"
    "          + sin((p.x+p.y)*5.0+t*0.7) + sin(length(p)*8.0-t*2.0+uBass*6.0);\n"
    "  v = v/4.0;\n"
    "  float hue = 0.6 + 0.15*v + uMid*0.3;\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)) + v*3.0);\n"
    "  float bright = clamp((0.4+0.6*abs(v))*boost + uBeat*0.4, 0.0, 1.0);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 1: Warp tunnel
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float r = length(p);\n"
    "  float a = atan(p.y,p.x);\n"
    "  float depth = 0.4/(r+0.05) + uTime*(0.5+uLevel*1.5);\n"
    "  float ang = a*3.0 + uTime*0.2;\n"
    "  float stripes = 0.5 + 0.5*sin(depth*12.566);\n"
    "  float spokes = 0.5 + 0.5*sin(ang*6.0 + uBass*10.0);\n"
    "  float m = stripes*spokes;\n"
    "  float fade = smoothstep(0.0, 0.15, r);\n"
    "  float hue = fract(depth*0.1 + uTreble*0.3);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = m*fade*(0.6 + uLevel + uBeat*0.5);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 2: Fractal fold
    "void main(){\n"
    "  vec2 p = (vUv*2.0-1.0)*1.5; p.x *= uRes.x/uRes.y;\n"
    "  vec2 z = p;\n"
    "  vec2 c = vec2(0.7+0.15*sin(uTime*0.3)+uBass*0.2,\n"
    "                0.5+0.15*cos(uTime*0.27)+uMid*0.2);\n"
    "  float orbit = 1e9;\n"
    "  for(int i=0;i<12;i++){ z = abs(z)/dot(z,z) - c; orbit = min(orbit, length(z)); }\n"
    "  float g = exp(-orbit*3.0);\n"
    "  float hue = fract(0.55 + orbit*0.3 + uTreble*0.4 + uTime*0.02);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(g*(1.0+uLevel*2.0) + uBeat*0.3, 0.0, 1.0);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 3: Kaleidoscope
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float r = length(p);\n"
    "  float a = atan(p.y,p.x);\n"
    "  float n = 6.0 + floor(uBass*6.0);\n"
    "  float seg = 6.28318/n;\n"
    "  a = a - seg*floor(a/seg);\n"
    "  a = abs(a - seg*0.5);\n"
    "  vec2 q = vec2(cos(a), sin(a))*r;\n"
    "  float v = sin(q.x*8.0+t) + sin(q.y*8.0-t*1.3) + sin(r*10.0-t*2.0+uMid*5.0);\n"
    "  float hue = fract(0.5 + 0.1*v + uTreble*0.3 + t*0.03);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(0.35 + 0.45*abs(v) + uLevel + uBeat*0.4, 0.0, 1.3);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 4: Hex tunnel
    "float hexDist(vec2 pp){\n"
    "  vec2 q = abs(pp);\n"
    "  return max(q.x*0.866025 + q.y*0.5, q.y);\n"
    "}\n"
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float r = length(p);\n"
    "  float a = atan(p.y,p.x);\n"
    "  float z = 1.0/(r+0.06) + uTime*(0.6+uLevel*1.5);\n"
    "  vec2 g = vec2(a*0.9549, z);\n"
    "  g = fract(g*vec2(3.0,2.0)) - 0.5;\n"
    "  float d = hexDist(g);\n"
    "  float cell = smoothstep(0.5, 0.44, d + 0.1*sin(z*2.0 + uBass*6.0));\n"
    "  float fade = smoothstep(0.0, 0.2, r);\n"
    "  float hue = fract(z*0.08 + uTreble*0.3);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = cell*fade*(0.5 + uLevel + uBeat*0.5);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 5: Voronoi cells
    "vec2 hash2(vec2 pp){\n"
    "  vec2 n = sin(vec2(dot(pp, vec2(127.1,311.7)), dot(pp, vec2(269.5,183.3))));\n"
    "  return fract(n*43758.5453);\n"
    "}\n"
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float scale = 4.0 + uMid*4.0;\n"
    "  vec2 g = p*scale;\n"
    "  vec2 cell = floor(g);\n"
    "  vec2 f = fract(g);\n"
    "  float md = 1e9;\n"
    "  vec2 id = vec2(0.0);\n"
    "  for(int j=-1;j<=1;j++){\n"
    "    for(int i=-1;i<=1;i++){\n"
    "      vec2 o = vec2(float(i), float(j));\n"
    "      vec2 pt = hash2(cell + o);\n"
    "      pt = 0.5 + 0.5*sin(t*(0.6+uLevel) + 6.28318*pt);\n"
    "      vec2 diff = o + pt - f;\n"
    "      float dd = dot(diff,diff);\n"
    "      if (dd < md){ md = dd; id = cell + o; }\n"
    "    }\n"
    "  }\n"
    "  float d = sqrt(md);\n"
    "  float idh = fract(sin(dot(id, vec2(12.9898,78.233)))*43758.5453);\n"
    "  float hue = fract(idh + uTreble*0.2 + t*0.02);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float pulse = 0.5 + 0.7*uBass + uBeat*0.5;\n"
    "  float edge = smoothstep(0.0, 0.06, d);\n"
    "  float bright = clamp((1.0-d)*pulse, 0.0, 1.2)*(0.35 + edge*0.65);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 6: Metaballs
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float field = 0.0;\n"
    "  for(int i=0;i<5;i++){\n"
    "    float fi = float(i);\n"
    "    vec2 c = 0.6*vec2(sin(t*(0.5+fi*0.13)+fi), cos(t*(0.43+fi*0.17)+fi*1.7));\n"
    "    float rr = 0.16 + 0.10*sin(t*0.7+fi) + uBass*0.12;\n"
    "    vec2 d = p - c;\n"
    "    field += rr*rr/(dot(d,d) + 0.0008);\n"
    "  }\n"
    "  float m = smoothstep(0.8, 1.4, field);\n"
    "  float hue = fract(0.55 + 0.10*field + uTreble*0.3 + t*0.02);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(m*(0.7+uLevel) + uBeat*0.3, 0.0, 1.2);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 7: Moire
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float a1 = t*0.1;\n"
    "  float a2 = -t*0.13 + uMid*1.5;\n"
    "  vec2 r1 = vec2(cos(a1)*p.x - sin(a1)*p.y, sin(a1)*p.x + cos(a1)*p.y);\n"
    "  float f = 22.0 + uBass*30.0;\n"
    "  float g1 = sin(r1.x*f)*sin(r1.y*f);\n"
    "  float g2 = sin(length(p)*(f*0.7) - t*2.0 + a2);\n"
    "  float v = g1*0.6 + g2*0.4;\n"
    "  float hue = fract(0.5 + 0.2*v + uTreble*0.3 + t*0.02);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(0.4 + 0.6*abs(v) + uLevel*0.6 + uBeat*0.3, 0.0, 1.3);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 8: Liquid noise
    "float hash(vec2 pp){\n"
    "  return fract(sin(dot(pp, vec2(127.1,311.7)))*43758.5453);\n"
    "}\n"
    "float vnoise(vec2 pp){\n"
    "  vec2 ip = floor(pp);\n"
    "  vec2 fp = fract(pp);\n"
    "  vec2 w = fp*fp*(3.0-2.0*fp);\n"
    "  float a = hash(ip);\n"
    "  float b = hash(ip + vec2(1.0,0.0));\n"
    "  float c = hash(ip + vec2(0.0,1.0));\n"
    "  float d = hash(ip + vec2(1.0,1.0));\n"
    "  return mix(mix(a,b,w.x), mix(c,d,w.x), w.y);\n"
    "}\n"
    "float fbm(vec2 pp){\n"
    "  float v = 0.0;\n"
    "  float amp = 0.5;\n"
    "  vec2 q = pp;\n"
    "  for(int i=0;i<5;i++){ v += amp*vnoise(q); q *= 2.0; amp *= 0.5; }\n"
    "  return v;\n"
    "}\n"
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime*0.15;\n"
    "  vec2 warp = vec2(fbm(p*2.0 + vec2(t,0.0)), fbm(p*2.0 + vec2(0.0,t) + 5.2));\n"
    "  float n = fbm(p*3.0 + warp*(2.0 + uBass*3.0) + t);\n"
    "  float hue = fract(0.05 + 0.12*n + uTreble*0.2 + uTime*0.01);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.25,0.5)));\n"
    "  float bright = clamp(0.2 + 1.1*n*(0.6+uLevel) + uBeat*0.3, 0.0, 1.3);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 9: Aurora
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime*0.3;\n"
    "  float glow = 0.0;\n"
    "  float hueShift = 0.0;\n"
    "  for(int i=0;i<3;i++){\n"
    "    float fi = float(i);\n"
    "    float sway = sin(p.y*(1.4+fi*0.6) + t*(1.0+fi*0.4) + fi*2.1)\n"
    "               + 0.5*sin(p.y*3.7 + t*2.0 + fi*4.0 + uMid*3.0);\n"
    "    float center = -0.7 + fi*0.7 + 0.35*sway;\n"
    "    float d = abs(p.x - center);\n"
    "    float band = exp(-d*d*(7.0 - uBass*4.0));\n"
    "    float vert = 0.55 + 0.45*smoothstep(-1.0, 1.0, p.y);\n"
    "    float shimmer = 0.8 + 0.2*sin(p.y*24.0 - uTime*5.0 + fi*3.0)*uTreble*3.0;\n"
    "    glow += band*vert*shimmer;\n"
    "    hueShift += band*fi;\n"
    "  }\n"
    "  float hue = 0.36 + 0.10*hueShift + 0.08*sin(t + p.y) + uTreble*0.15;\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(glow*(0.45 + uLevel*1.2) + uBeat*0.15, 0.0, 1.25);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 10: Supernova
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float r = length(p);\n"
    "  float a = atan(p.y,p.x);\n"
    "  float t = uTime;\n"
    "  float ringPhase = r*9.0 - t*(2.0 + uLevel*3.0);\n"
    "  float rings = pow(0.5 + 0.5*sin(ringPhase*3.14159), 6.0);\n"
    "  rings = rings*smoothstep(0.05, 0.35, r)*(0.3 + uBeat*1.2);\n"
    "  float spokes = pow(abs(sin(a*9.0 + t*0.4)), 8.0)*exp(-r*2.2)*(0.4 + uTreble*2.0);\n"
    "  float core = exp(-r*r*(14.0 - uBass*8.0))*(0.8 + uBass*1.5);\n"
    "  float v = rings + spokes + core;\n"
    "  float hue = fract(0.02 + r*0.12 - t*0.02 + uMid*0.15);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.25,0.55)));\n"
    "  float bright = clamp(v, 0.0, 1.35);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 11: Interference
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float freq = 14.0 - uBass*7.0;\n"
    "  float v = 0.0;\n"
    "  for(int i=0;i<3;i++){\n"
    "    float fi = float(i);\n"
    "    vec2 src = 0.7*vec2(sin(t*(0.31+fi*0.11) + fi*2.09), cos(t*(0.23+fi*0.13) + fi*4.19));\n"
    "    v += sin(length(p - src)*freq - t*2.5 + fi);\n"
    "  }\n"
    "  v = v/3.0;\n"
    "  float crest = pow(1.0 - abs(v), 3.0);\n"
    "  float hue = fract(0.52 + 0.12*v + uTreble*0.25 + t*0.015);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(0.15 + crest*(0.8 + uLevel*1.2) + uBeat*0.25, 0.0, 1.25);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 12: Phyllotaxis
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float spin = t*0.08;\n"
    "  vec2 rot = vec2(cos(spin), sin(spin));\n"
    "  p = vec2(p.x*rot.x - p.y*rot.y, p.x*rot.y + p.y*rot.x);\n"
    "  float GA = 2.39996323;\n"
    "  int count = 72;\n"
    "  float spread = 0.105 + 0.02*sin(t*0.5) + uBass*0.02;\n"
    "  float glow = 0.0;\n"
    "  float hueAcc = 0.0;\n"
    "  for(int i=1;i<=count;i++){\n"
    "    float fi = float(i);\n"
    "    float ang = fi*GA + t*0.15;\n"
    "    float rad = spread*sqrt(fi);\n"
    "    vec2 c = vec2(cos(ang), sin(ang))*rad;\n"
    "    vec2 d = p - c;\n"
    "    float ring = fi/float(count);\n"
    "    float energy = mix(uBass, mix(uMid, uTreble, clamp(ring*2.0-1.0, 0.0, 1.0)), clamp(ring*2.0, 0.0, 1.0));\n"
    "    float size = 3600.0 - energy*2400.0 - uBeat*600.0;\n"
    "    float g = exp(-dot(d,d)*size);\n"
    "    glow += g;\n"
    "    hueAcc += g*ring;\n"
    "  }\n"
    "  float hue = fract(0.08 + hueAcc/max(glow, 0.001)*0.5 + t*0.02);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(glow*(0.55 + uLevel*0.9), 0.0, 1.3);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 13: Neon horizon
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  vec3 col = vec3(0.0);\n"
    "  float horizon = -0.12;\n"
    "  if (p.y < horizon) {\n"
    "    float depth = horizon - p.y;\n"
    "    float z = 1.0/(depth + 0.001);\n"
    "    float gx = p.x*z*0.7;\n"
    "    float gz = z*0.5 + t*(1.2 + uLevel*2.5);\n"
    "    float lineX = pow(1.0 - abs(fract(gx)*2.0 - 1.0), 8.0);\n"
    "    float lineZ = pow(1.0 - abs(fract(gz)*2.0 - 1.0), 8.0);\n"
    "    float grid = clamp(lineX + lineZ, 0.0, 1.0)*exp(-depth*0.25)*smoothstep(0.0, 0.05, depth);\n"
    "    float gridHue = fract(0.78 + uMid*0.1);\n"
    "    col += grid*(0.6 + uBass*0.8)*(0.5 + 0.5*cos(6.28318*(gridHue+vec3(0.0,0.33,0.67))));\n"
    "  } else {\n"
    "    vec2 sunC = vec2(0.0, horizon + 0.34);\n"
    "    float sr = 0.26 + uBass*0.10 + uBeat*0.04;\n"
    "    float d = length(p - sunC);\n"
    "    float disc = smoothstep(sr, sr - 0.015, d);\n"
    "    float gaps = smoothstep(0.35, 0.65, 0.5 + 0.5*sin((p.y - horizon)*70.0 - t*3.0));\n"
    "    float sun = disc*mix(1.0, gaps, smoothstep(sunC.y + sr*0.1, sunC.y - sr, p.y));\n"
    "    vec3 sunCol = mix(vec3(1.0,0.25,0.55), vec3(1.0,0.85,0.3), clamp((p.y - sunC.y)/sr*0.5 + 0.5, 0.0, 1.0));\n"
    "    vec2 cell = floor(p*24.0);\n"
    "    float star = fract(sin(dot(cell, vec2(127.1,311.7)))*43758.5453);\n"
    "    float tw = step(0.985, star)*(0.5 + 0.5*sin(t*6.0 + star*40.0))*(0.3 + uTreble*2.5);\n"
    "    col += sun*sunCol*(0.9 + uLevel*0.6) + vec3(tw);\n"
    "    col += vec3(0.9,0.2,0.6)*exp(-abs(p.y - horizon)*9.0)*(0.35 + uBass*0.5);\n"
    "  }\n"
    "  fragColor = vec4(clamp(col, vec3(0.0), vec3(1.35)), 1.0);\n"
    "}\n",
    // 14: Spectrum bars (per-bin, from the FFT texture)
    "void main(){\n"
    "  float bars = 96.0;\n"
    "  float px = vUv.x;\n"
    "  float slot = floor(px*bars)/bars + 0.5/bars;\n"
    "  float m = specLog(slot);\n"
    "  float h = 0.04 + m*0.82;\n"
    "  float base = 0.14;\n"
    "  float inBarX = step(abs(fract(px*bars) - 0.5), 0.42);\n"
    "  float v = 0.0;\n"
    "  float yUp = (vUv.y - base)/max(h, 0.001);\n"
    "  if (yUp >= 0.0 && yUp <= 1.0) v = inBarX*(0.30 + 0.70*yUp);\n"
    "  float cap = exp(-abs(vUv.y - (base + h))*90.0)*inBarX*(0.6 + uBeat*0.8);\n"
    "  float yDn = (base - vUv.y)/max(h*0.35, 0.001);\n"
    "  if (yDn >= 0.0 && yDn <= 1.0) v += inBarX*0.16*(1.0 - yDn);\n"
    "  float hue = fract(0.66 - slot*0.55 + uTime*0.01);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(v*(0.6 + uLevel*0.7) + cap*m, 0.0, 1.3);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 15: Spectrum ring
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float r = length(p);\n"
    "  float a = atan(p.y,p.x);\n"
    "  float x = abs(fract(a/6.28318 + 0.75 + uTime*0.008)*2.0 - 1.0);\n"
    "  float m = specLog(x);\n"
    "  float ring = 0.40 + m*0.32 + uBass*0.04;\n"
    "  float d = abs(r - ring);\n"
    "  float line = exp(-d*d*900.0)*(0.45 + m*1.5);\n"
    "  float fill = step(r, ring)*exp(-(ring - r)*9.0)*0.22*(0.4 + uLevel);\n"
    "  float hue = fract(0.6 + x*0.45 + uTime*0.012);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(line + fill + uBeat*exp(-d*16.0)*0.35, 0.0, 1.3);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // 16: Scope lines (live waveform from the wave texture)
    "void main(){\n"
    "  float x = vUv.x;\n"
    "  vec3 col = vec3(0.0);\n"
    "  for(int i=0;i<3;i++){\n"
    "    float fi = float(i);\n"
    "    float s = wav(clamp(x + fi*0.004, 0.0, 1.0));\n"
    "    float yc = 0.5 + s*(0.26 + uLevel*0.35) + (fi - 1.0)*0.012*(1.0 + uTreble*4.0);\n"
    "    float d = abs(vUv.y - yc);\n"
    "    float g = exp(-d*d*5200.0) + exp(-d*26.0)*0.10;\n"
    "    vec3 tint = vec3(0.35,0.55,1.0);\n"
    "    if (i == 1) tint = vec3(0.40,1.00,0.75);\n"
    "    if (i == 2) tint = vec3(1.00,0.45,0.75);\n"
    "    col += tint*g*(0.7 + uLevel*0.8);\n"
    "  }\n"
    "  float gx = pow(1.0 - abs(fract(x*10.0)*2.0 - 1.0), 60.0);\n"
    "  float gy = pow(1.0 - abs(fract(vUv.y*6.0)*2.0 - 1.0), 60.0);\n"
    "  col += vec3(0.5,0.6,0.55)*(gx + gy)*0.05;\n"
    "  col += vec3(0.3,0.5,1.0)*uBeat*0.08;\n"
    "  fragColor = vec4(clamp(col, vec3(0.0), vec3(1.3)), 1.0);\n"
    "}\n",
};

static const wchar_t* SV_NAMES[] = {
    L"Plasma field", L"Warp tunnel", L"Fractal fold", L"Kaleidoscope",
    L"Hex tunnel", L"Voronoi cells", L"Metaballs", L"Moir\x00E9",
    L"Liquid noise", L"Aurora", L"Supernova", L"Interference",
    L"Phyllotaxis", L"Neon horizon", L"Spectrum bars", L"Spectrum ring",
    L"Scope lines",
};
static const int SV_COUNT = (int)(sizeof(SV_FRAG_BODY) / sizeof(SV_FRAG_BODY[0]));
static_assert(sizeof(SV_NAMES) / sizeof(SV_NAMES[0]) == sizeof(SV_FRAG_BODY) / sizeof(SV_FRAG_BODY[0]),
              "every GPU shader needs a name");

struct ShaderProg {
    GLuint prog = 0;
    GLint uRes = -1, uTime = -1, uLevel = -1, uBass = -1, uMid = -1, uTreble = -1, uBeat = -1;
};

struct ShaderHost {
    HWND child = nullptr;
    HDC dc = nullptr;
    HGLRC glrc = nullptr;
    GLuint vao = 0;
    ShaderProg progs[SV_COUNT] = {};
    bool ready = false;
    int lost = 0;             // consecutive wglMakeCurrent failures (GPU reset)
    GLuint overlayTex = 0;    // UI overlay uploaded from the D2D-rendered bitmap
    int overlayW = 0, overlayH = 0;
    GLuint specTex = 0, waveTex = 0;   // per-frame FFT/waveform textures
    int cw = 1, ch = 1;

    // GL function pointers.
    PFN_glCreateShader p_CreateShader = nullptr;
    PFN_glShaderSource p_ShaderSource = nullptr;
    PFN_glCompileShader p_CompileShader = nullptr;
    PFN_glGetShaderiv p_GetShaderiv = nullptr;
    PFN_glGetShaderInfoLog p_GetShaderInfoLog = nullptr;
    PFN_glDeleteShader p_DeleteShader = nullptr;
    PFN_glCreateProgram p_CreateProgram = nullptr;
    PFN_glAttachShader p_AttachShader = nullptr;
    PFN_glLinkProgram p_LinkProgram = nullptr;
    PFN_glGetProgramiv p_GetProgramiv = nullptr;
    PFN_glUseProgram2 p_UseProgram = nullptr;
    PFN_glGetUniformLocation p_GetUniformLocation = nullptr;
    PFN_glUniform1f p_Uniform1f = nullptr;
    PFN_glUniform2f p_Uniform2f = nullptr;
    PFN_glUniform1i p_Uniform1i = nullptr;
    PFN_glGenVertexArrays p_GenVertexArrays = nullptr;
    PFN_glBindVertexArray2 p_BindVertexArray = nullptr;
    PFN_glActiveTexture p_ActiveTexture = nullptr;
    PFNWGLSWAPINTERVALEXTPROC p_SwapInterval = nullptr;

    // Companion windows present unsynced so N windows don't stack N vblank
    // waits on the one render thread.
    void SetVsync(bool on) {
        if (!ready || !p_SwapInterval) return;
        wglMakeCurrent(dc, glrc);
        p_SwapInterval(on ? 1 : 0);
    }

    static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        // Forward mouse input to the parent so the shared control deck is
        // clickable over the shader engine too.
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONUP: case WM_LBUTTONDBLCLK: {
            HWND p = GetParent(h); if (p) PostMessageW(p, m, w, l); return 0;
        }
        }
        return DefWindowProc(h, m, w, l);
    }

    template <class T> void LoadGL(T& fn, const char* name) {
        fn = (T)wglGetProcAddress(name);
    }

    GLuint Compile(GLenum type, const char* src1, const char* src2) {
        GLuint s = p_CreateShader(type);
        const char* parts[2] = { src1, src2 ? src2 : "" };
        GLsizei n = src2 ? 2 : 1;
        p_ShaderSource(s, n, parts, nullptr);
        p_CompileShader(s);
        GLint ok = 0; p_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {}; p_GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
            OutputDebugStringA("WaveScope shader compile error:\n");
            OutputDebugStringA(log);
            p_DeleteShader(s);
            return 0;
        }
        return s;
    }

    bool BuildProgram(int idx) {
        GLuint vs = Compile(GL_VERTEX_SHADER, SV_VERT, nullptr);
        GLuint fs = Compile(GL_FRAGMENT_SHADER, SV_FRAG_PRELUDE, SV_FRAG_BODY[idx]);
        if (!vs || !fs) return false;
        GLuint prog = p_CreateProgram();
        p_AttachShader(prog, vs);
        p_AttachShader(prog, fs);
        p_LinkProgram(prog);
        p_DeleteShader(vs); p_DeleteShader(fs);
        GLint ok = 0; p_GetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) return false;
        ShaderProg& p = progs[idx];
        p.prog = prog;
        p.uRes = p_GetUniformLocation(prog, "uRes");
        p.uTime = p_GetUniformLocation(prog, "uTime");
        p.uLevel = p_GetUniformLocation(prog, "uLevel");
        p.uBass = p_GetUniformLocation(prog, "uBass");
        p.uMid = p_GetUniformLocation(prog, "uMid");
        p.uTreble = p_GetUniformLocation(prog, "uTreble");
        p.uBeat = p_GetUniformLocation(prog, "uBeat");
        // Sampler bindings are fixed: spectrum on unit 0, waveform on unit 1.
        p_UseProgram(prog);
        GLint us = p_GetUniformLocation(prog, "uSpectrum");
        GLint uw = p_GetUniformLocation(prog, "uWave");
        if (us >= 0) p_Uniform1i(us, 0);
        if (uw >= 0) p_Uniform1i(uw, 1);
        p_UseProgram(0);
        return true;
    }

    // One R8 1-D texture, allocated once and refreshed per frame.
    GLuint MakeAudioTex(int width) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_1D, t);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_R8, width, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        return t;
    }

    bool Init(HINSTANCE inst, HWND parent) {
        if (ready) return true;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"WaveScopeShader";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);   // else the frame's resize cursor shows through
        wc.style = CS_OWNDC;
        RegisterClassW(&wc);

        RECT rc; GetClientRect(parent, &rc);
        child = CreateWindowExW(0, L"WaveScopeShader", L"", WS_CHILD,
                                0, 0, rc.right, rc.bottom, parent, nullptr, inst, nullptr);
        if (!child) return false;
        dc = GetDC(child);

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
        int pf = ChoosePixelFormat(dc, &pfd);
        // Failures from here tear the partial state down (no leak per retry).
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) { Destroy(); return false; }

        HGLRC legacy = wglCreateContext(dc);
        if (!legacy) { Destroy(); return false; }
        wglMakeCurrent(dc, legacy);

        auto createAttribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        if (createAttribs) {
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
                0
            };
            HGLRC ctx = createAttribs(dc, nullptr, attribs);
            if (ctx) { wglMakeCurrent(dc, ctx); wglDeleteContext(legacy); glrc = ctx; }
            else glrc = legacy;
        } else {
            glrc = legacy;
        }

        p_SwapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (p_SwapInterval) p_SwapInterval(1);

        LoadGL(p_CreateShader, "glCreateShader");
        LoadGL(p_ShaderSource, "glShaderSource");
        LoadGL(p_CompileShader, "glCompileShader");
        LoadGL(p_GetShaderiv, "glGetShaderiv");
        LoadGL(p_GetShaderInfoLog, "glGetShaderInfoLog");
        LoadGL(p_DeleteShader, "glDeleteShader");
        LoadGL(p_CreateProgram, "glCreateProgram");
        LoadGL(p_AttachShader, "glAttachShader");
        LoadGL(p_LinkProgram, "glLinkProgram");
        LoadGL(p_GetProgramiv, "glGetProgramiv");
        LoadGL(p_UseProgram, "glUseProgram");
        LoadGL(p_GetUniformLocation, "glGetUniformLocation");
        LoadGL(p_Uniform1f, "glUniform1f");
        LoadGL(p_Uniform2f, "glUniform2f");
        LoadGL(p_Uniform1i, "glUniform1i");
        LoadGL(p_GenVertexArrays, "glGenVertexArrays");
        LoadGL(p_BindVertexArray, "glBindVertexArray");
        LoadGL(p_ActiveTexture, "glActiveTexture");
        if (!p_CreateShader || !p_CreateProgram || !p_UseProgram ||
            !p_GenVertexArrays || !p_Uniform1i || !p_ActiveTexture) {
            Destroy(); return false;
        }

        p_GenVertexArrays(1, &vao);

        specTex = MakeAudioTex(1024);
        waveTex = MakeAudioTex(2048);

        cw = rc.right > 0 ? rc.right : 1;
        ch = rc.bottom > 0 ? rc.bottom : 1;

        bool any = false;
        for (int i = 0; i < SV_COUNT; i++) any |= BuildProgram(i);
        if (!any) { Destroy(); return false; }

        ready = true;
        return true;
    }

    const wchar_t* ShaderName(int idx) const { return SV_NAMES[(idx % SV_COUNT + SV_COUNT) % SV_COUNT]; }
    int ShaderCount() const { return SV_COUNT; }
    // How many programs actually compiled+linked.
    int CompiledCount() const {
        int n = 0;
        for (const ShaderProg& p : progs) if (p.prog) n++;
        return n;
    }

    void Show(bool on) { if (child) ShowWindow(child, on ? SW_SHOW : SW_HIDE); }

    void Resize(int w, int h) {
        if (!ready || w <= 0 || h <= 0) return;
        SetWindowPos(child, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE);
        cw = w; ch = h;
    }

    // Render the shader frame; leaves the context current with no swap so the
    // shared UI overlay can be composited before Present.
    void RenderEngine(int idx, const AudioFrame& f) {
        if (!ready) return;
        idx = (idx % SV_COUNT + SV_COUNT) % SV_COUNT;
        ShaderProg& s = progs[idx];
        if (!s.prog) return;
        if (!wglMakeCurrent(dc, glrc)) { lost++; return; }  // GPU reset/TDR
        lost = 0;

        // Refresh the audio textures (r8: byte per texel, sampled 0..1).
        uint8_t buf[2048];
        for (int i = 0; i < 1024; i++)
            buf[i] = (uint8_t)(clampf(f.fft[i], 0.0f, 1.0f) * 255.0f);
        p_ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_1D, specTex);
        glTexSubImage1D(GL_TEXTURE_1D, 0, 0, 1024, GL_RED, GL_UNSIGNED_BYTE, buf);
        for (int i = 0; i < 2048; i++)
            buf[i] = (uint8_t)(clampf(f.wave[i] * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
        p_ActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_1D, waveTex);
        glTexSubImage1D(GL_TEXTURE_1D, 0, 0, 2048, GL_RED, GL_UNSIGNED_BYTE, buf);
        p_ActiveTexture(GL_TEXTURE0);

        glViewport(0, 0, cw, ch);
        glClearColor(0.043f, 0.051f, 0.047f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        p_UseProgram(s.prog);
        p_BindVertexArray(vao);
        p_Uniform2f(s.uRes, (float)cw, (float)ch);
        p_Uniform1f(s.uTime, f.t);
        p_Uniform1f(s.uLevel, f.level);
        p_Uniform1f(s.uBass, f.bass);
        p_Uniform1f(s.uMid, f.mid);
        p_Uniform1f(s.uTreble, f.treble);
        p_Uniform1f(s.uBeat, f.beat);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Reset modern-GL state and set a top-left ortho for the UIPainter.
    void BeginOverlay() {
        p_UseProgram(0);
        p_BindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, cw, ch, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    }

    // Composite the D2D-rendered UI overlay (premultiplied BGRA) as a
    // full-window textured quad. Re-uploads only when `dirty`.
    void DrawOverlayTex(const void* bgra, int tw, int th, bool dirty) {
        glEnable(GL_TEXTURE_2D);
        if (!overlayTex) glGenTextures(1, &overlayTex);
        glBindTexture(GL_TEXTURE_2D, overlayTex);
        if (dirty && bgra && tw > 0 && th > 0) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            if (tw != overlayW || th != overlayH) {   // (re)allocate on resize only
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
                overlayW = tw; overlayH = th;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
            }
        }
        if (overlayW <= 0) { glDisable(GL_TEXTURE_2D); return; }
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied alpha
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f((float)cw, 0);
        glTexCoord2f(1, 1); glVertex2f((float)cw, (float)ch);
        glTexCoord2f(0, 1); glVertex2f(0, (float)ch);
        glEnd();
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
    }

    void Present() {
        glDisable(GL_BLEND);
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        SwapBuffers(dc);
    }

    // Read the just-rendered (pre-present) backbuffer; bottom-up BGRA.
    bool ReadBack(std::vector<uint8_t>& out, int& w, int& h) {
        if (!ready) return false;
        wglMakeCurrent(dc, glrc);
        w = cw; h = ch;
        if (w <= 0 || h <= 0) return false;
        out.resize((size_t)w * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, out.data());
        return glGetError() == 0;
    }

    void Destroy() {
        if (glrc) wglMakeCurrent(dc, glrc);
        if (overlayTex) { glDeleteTextures(1, &overlayTex); overlayTex = 0; }
        if (specTex) { glDeleteTextures(1, &specTex); specTex = 0; }
        if (waveTex) { glDeleteTextures(1, &waveTex); waveTex = 0; }
        overlayW = overlayH = 0;
        lost = 0;
        for (ShaderProg& p : progs) p = ShaderProg{};
        if (glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glrc); glrc = nullptr; }
        if (child && dc) ReleaseDC(child, dc);
        if (child) DestroyWindow(child);
        child = nullptr; dc = nullptr; ready = false;
    }
};
