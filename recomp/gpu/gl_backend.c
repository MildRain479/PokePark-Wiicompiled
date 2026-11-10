/* OpenGL device for the GX decoder (gx_device.h).
 *
 * The EFB (the GPU's embedded framebuffer, 640x528) is an offscreen
 * framebuffer rendered at a multiple of the native resolution (the
 * settings' internal resolution, by default whatever fits the window),
 * wider again in 16:9, with multisample anti-aliasing if the
 * settings ask for it. TEV, the GPU's programmable color combiner, runs
 * in one "uber" fragment shader driven by uniforms, so any stage setup
 * draws without compiling a new shader. Copies of the EFB to the XFB are
 * presented in a window, or borderless fullscreen, at the shape they have
 * on a TV, with the settings menu over them while it is open;
 * RECOMP_GX_DUMP=<dir> also writes them as BMPs. gx_frames.c decides
 * what is drawn when; the window is window_win32.c's.
 *
 * Environment:
 *   RECOMP_GX_SCALE=n          internal resolution multiplier, over the settings
 *   RECOMP_GX_DUMP=<dir>       write presented frames as BMP files
 *   RECOMP_GX_DUMP_EVERY=n     ...every nth frame (default 30)
 *   RECOMP_GX_DUMP_FROM=n      ...from the nth on (default 0)
 *   RECOMP_GL_PRESENT=swapbuffers  present with SwapBuffers, not DXGI (see "Presenting")
 *   RECOMP_GL_DEBUG=1          debug context, printing the driver's error messages */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_5.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")


#include "gx_device.h"
#include "host_settings.h"

/* ------------------------------------------------------------------------ */
/* GL 2+ entry points */

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_LOWER_LEFT 0x8CA1
#define GL_ZERO_TO_ONE 0x935F
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_MAX_SAMPLES 0x8D57
#define GL_BGRA 0x80E1

#define GL_FUNCS(X) \
    X(void, glGenBuffers, (GLsizei, GLuint*)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*)) \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar*)) \
    X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint*)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1iv, (GLint, GLsizei, const GLint*)) \
    X(void, glUniform4iv, (GLint, GLsizei, const GLint*)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint*)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint*)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint*)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glBlendEquation, (GLenum)) \
    X(void, glClipControl, (GLenum, GLenum)) \
    X(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint))

#define DECLARE(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; static PFN_##name p_##name;
GL_FUNCS(DECLARE)
#undef DECLARE

static void gl_forget(void);
/* wglSwapIntervalEXT. Set to 0: presenting never waits for the display.
 * The game paces itself by its emulated retrace (and the frames between
 * by gx_frames.c), and in a window or borderless window the compositor
 * shows only whole frames anyway. Waiting held up the game thread: with
 * a display whose refresh stalls (a virtual display with nobody
 * watching), every present took 110 ms and the game ran at 7 fps. */
static BOOL(WINAPI* pSwapInterval)(int);

static void load_gl(void) {
#define LOAD(ret, name, args) \
    p_##name = (PFN_##name)wglGetProcAddress(#name); \
    if (!p_##name) { fprintf(stderr, "gl: missing %s\n", #name); exit(1); }
    GL_FUNCS(LOAD)
#undef LOAD
}

/* ------------------------------------------------------------------------ */
/* Shaders */

#define STR2(x) #x
#define STR(x) STR2(x)
#define GX_XF_WORDS_STR STR(GX_XF_WORDS)
#define GX_XF_MEM_WORDS_STR STR(GX_XF_MEM_WORDS)

/* The GPU's transform unit (XF), per vertex: position and normal
 * transform by the vertex's matrices, projection, the two color channels
 * with lighting, and texture coordinate generation. All state comes from
 * the XF memory and registers (gx.h), in a uniform buffer.
 *
 * Lighting follows the hardware model as Dolphin documents it: a lit
 * channel is material * clamp(ambient + sum of its lights), where each
 * light contributes color * attenuation * diffuse term; attenuation is
 * none, specular or spot, the diffuse term none, signed or clamped
 * N.L. */
static const char* kVertexShader =
    "#version 330 compatibility\n"
    "layout(std140) uniform XF { uvec4 xfm[" GX_XF_WORDS_STR " / 4]; };\n"
    "in vec4 a_mtx0, a_mtx1, a_mtx2; in vec3 a_pos, a_nrm; in vec4 a_col0, a_col1;\n"
    "in vec4 a_tex01, a_tex23, a_tex45, a_tex67;\n"
    "uniform vec4 u_canvas; /* what clip space's x and y are multiplied by (gx_canvas_draw) */\n"
    "out vec4 v_col0; out vec4 v_col1; out vec3 v_tex[8];\n"
    "uint xw(int a) { int i = a < 0x1000 ? a : a - 0x1000 + " GX_XF_MEM_WORDS_STR "; return xfm[i >> 2][i & 3]; }\n"
    "float xfl(int a) { return uintBitsToFloat(xw(a)); }\n"
    "vec4 row(int a) { return uintBitsToFloat(xfm[a >> 2]); }\n" /* rows of 4 start on multiples of 4 */
    "vec3 vec3at(int a) { return vec3(xfl(a), xfl(a + 1), xfl(a + 2)); }\n"
    "vec4 unpack(uint c) { return vec4(float((c >> 24) & 255u), float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0; }\n"
    "vec4 light(int j, uint ctrl, vec3 pos, vec3 n) {\n"
    "  int b = 0x600 + j * 16;\n"
    "  vec3 cosatt = vec3at(b + 4), distatt = vec3at(b + 7), lpos = vec3at(b + 10), ldirv = vec3at(b + 13);\n"
    "  uint dfn = (ctrl >> 7) & 3u, afn = (ctrl >> 9) & 3u;\n"
    "  vec3 ldir; float attn;\n"
    "  if (afn == 1u) {\n" /* specular */
    "    ldir = normalize(lpos - pos);\n"
    "    attn = dot(n, ldir) >= 0.0 ? max(0.0, dot(n, ldirv)) : 0.0;\n"
    "    vec3 da = dfn == 0u ? distatt : normalize(distatt);\n"
    "    attn = max(0.0, dot(cosatt, vec3(1.0, attn, attn * attn))) / dot(da, vec3(1.0, attn, attn * attn));\n"
    "  } else if (afn == 3u) {\n" /* spot */
    "    vec3 d = lpos - pos; float dist2 = dot(d, d), dist = sqrt(dist2); ldir = d / dist;\n"
    "    attn = max(0.0, dot(ldir, ldirv));\n"
    "    attn = max(0.0, cosatt.x + cosatt.y * attn + cosatt.z * attn * attn) / dot(distatt, vec3(1.0, dist, dist2));\n"
    "  } else {\n" /* none, directional */
    "    vec3 d = lpos - pos; ldir = length(d) == 0.0 ? n : normalize(d); attn = 1.0;\n"
    "  }\n"
    "  float diff = dfn == 0u ? 1.0 : (dfn == 1u ? dot(ldir, n) : max(0.0, dot(ldir, n)));\n"
    "  return attn * diff * unpack(xw(b + 3));\n"
    "}\n"
    "vec4 lights(uint ctrl, vec3 pos, vec3 n) {\n"
    "  uint mask = ((ctrl >> 2) & 15u) | (((ctrl >> 11) & 15u) << 4);\n"
    "  vec4 sum = vec4(0.0);\n"
    "  for (int j = 0; j < 8; j++) if ((mask & (1u << uint(j))) != 0u) sum += light(j, ctrl, pos, n);\n"
    "  return sum;\n"
    "}\n"
    "vec4 channel(int i, vec4 vcol, vec3 pos, vec3 n) {\n"
    "  uint cc = xw(0x100E + i), ac = xw(0x1010 + i);\n"
    "  vec4 matr = unpack(xw(0x100C + i)), ambr = unpack(xw(0x100A + i)), r;\n"
    "  vec3 mat = (cc & 1u) != 0u ? vcol.rgb : matr.rgb;\n"
    "  if ((cc & 2u) != 0u) {\n"
    "    vec3 amb = (cc & 64u) != 0u ? vcol.rgb : ambr.rgb;\n"
    "    r.rgb = mat * clamp(amb + lights(cc, pos, n).rgb, 0.0, 1.0);\n"
    "  } else r.rgb = mat;\n"
    "  float mata = (ac & 1u) != 0u ? vcol.a : matr.a;\n"
    "  if ((ac & 2u) != 0u) {\n"
    "    float amba = (ac & 64u) != 0u ? vcol.a : ambr.a;\n"
    "    r.a = mata * clamp(amba + lights(ac, pos, n).a, 0.0, 1.0);\n"
    "  } else r.a = mata;\n"
    "  return clamp(r, 0.0, 1.0);\n"
    "}\n"
    "int texmtx(int i) { return int(i < 3 ? a_mtx0[i + 1] : i < 7 ? a_mtx1[i - 3] : a_mtx2.x); }\n"
    "vec2 texin(int i) { vec4 p = i < 2 ? a_tex01 : i < 4 ? a_tex23 : i < 6 ? a_tex45 : a_tex67; return (i & 1) == 0 ? p.xy : p.zw; }\n"
    "void main() {\n"
    "  int pn = int(a_mtx0.x);\n"
    "  vec4 in4 = vec4(a_pos, 1.0);\n"
    "  vec3 world = vec3(dot(row(pn * 4), in4), dot(row(pn * 4 + 4), in4), dot(row(pn * 4 + 8), in4));\n"
    "  int nb = 0x400 + (pn & 31) * 3;\n"
    "  vec3 n = vec3(dot(vec3at(nb), a_nrm), dot(vec3at(nb + 3), a_nrm), dot(vec3at(nb + 6), a_nrm));\n"
    "  if (dot(n, n) > 0.0) n = normalize(n);\n"
    "  float p0 = xfl(0x1020), p1 = xfl(0x1021), p2 = xfl(0x1022), p3 = xfl(0x1023), p4 = xfl(0x1024), p5 = xfl(0x1025);\n"
    "  vec4 clip = xw(0x1026) == 0u\n"
    "    ? vec4(p0 * world.x + p1 * world.z, p2 * world.y + p3 * world.z, p4 * world.z + p5, -world.z)\n"
    "    : vec4(p0 * world.x + p1, p2 * world.y + p3, p4 * world.z + p5, 1.0);\n"
    /* GX depth runs from -w (near) to 0 (far). The EFB depth is stored
     * reversed (see gxd_init), so the far plane, where GX scenes keep
     * nearly everything, gets the float precision: 0..w, far to near. */
    "  gl_Position = vec4(clip.x * u_canvas.x, clip.y * u_canvas.y, -clip.z, clip.w);\n"
    "  v_col0 = channel(0, a_col0, world, n);\n"
    "  v_col1 = channel(1, a_col1, world, n);\n"
    "  int ntex = int(xw(0x103F) & 15u);\n"
    "  bool dual = (xw(0x1012) & 1u) != 0u;\n"
    "  for (int i = 0; i < 8; i++) {\n"
    "    if (i >= ntex) { v_tex[i] = vec3(0.0, 0.0, 1.0); continue; }\n"
    "    uint info = xw(0x1040 + i);\n"
    "    int src_row = int((info >> 7) & 31u), type = int((info >> 4) & 7u);\n"
    "    vec4 src = vec4(0.0, 0.0, 1.0, 1.0);\n"
    "    if (src_row == 0) src = in4;\n"
    "    else if (src_row == 1) src.xyz = a_nrm;\n"
    "    else if (src_row >= 5 && src_row <= 12) src.xy = texin(src_row - 5);\n"
    "    vec3 stq;\n"
    "    if (type == 0) {\n"
    "      int m = texmtx(i) * 4;\n"
    "      stq = vec3(dot(row(m), src), dot(row(m + 4), src), ((info >> 1) & 1u) != 0u ? dot(row(m + 8), src) : 1.0);\n"
    "    } else stq = vec3(src.xy, 1.0);\n"
    "    if (dual) {\n" /* post-transform */
    "      uint pt = xw(0x1050 + i);\n"
    "      vec4 v = vec4(stq, 1.0);\n"
    "      if (((pt >> 8) & 1u) != 0u && dot(v.xyz, v.xyz) > 0.0) v.xyz = normalize(v.xyz);\n"
    "      int m = 0x500 + int(pt & 63u) * 4;\n"
    "      stq = vec3(dot(row(m), v), dot(row(m + 4), v), dot(row(m + 8), v));\n"
    "    }\n"
    "    v_tex[i] = stq;\n"
    "  }\n"
    "}\n";

/* TEV: each stage computes, per color channel and for alpha,
 *   d + lerp(a, b, c) (or d - lerp(...)), plus a bias, times a scale,
 * optionally clamped, into one of four registers; or, with bias 3, a
 * comparison of a and b selecting c or 0. Inputs are registers, the
 * texture, the rasterized color, constants and "konst" colors. */
static const char* kFragmentShader =
    "#version 330 compatibility\n"
    "in vec4 v_col0; in vec4 v_col1; in vec3 v_tex[8];\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex0, u_tex1, u_tex2, u_tex3, u_tex4, u_tex5, u_tex6, u_tex7;\n"
    "uniform int u_num_stages;\n"
    "uniform ivec4 u_cin[16], u_ain[16], u_cop[16], u_aop[16], u_map[16], u_sel[16];\n"
    "uniform vec4 u_reg[4], u_konst[4];\n"
    "uniform ivec4 u_swap[4];\n"
    "uniform ivec4 u_alpha;\n"
    "uniform vec4 u_alpha_ref;\n"
    "uniform ivec4 u_fog; /* kind, orthographic, b's magnitude and shift (gx.h) */\n"
    "uniform vec4 u_fogf; /* a, c */\n"
    "uniform vec4 u_fog_color;\n"
    "uniform vec4 u_fog_range[4]; /* the offset from the center as x a + b, on; ten cosines */\n"
    "/* Indirect texturing (gx.h): the indirect stages and whether any stage is\n"
    " * indirect; each stage's command; the indirect stages' maps, coordinates\n"
    " * and scales; the matrices' rows, each with its shift; the coordinates'\n"
    " * sizes, two to a vector. */\n"
    "uniform ivec4 u_ind, u_ind_cmd[4], u_ind_stage[4], u_ind_mtx[6], u_tc_size[4];\n"
    "vec4 sample_map(int m, vec2 uv) {\n"
    "  if (m == 0) return texture(u_tex0, uv); if (m == 1) return texture(u_tex1, uv);\n"
    "  if (m == 2) return texture(u_tex2, uv); if (m == 3) return texture(u_tex3, uv);\n"
    "  if (m == 4) return texture(u_tex4, uv); if (m == 5) return texture(u_tex5, uv);\n"
    "  if (m == 6) return texture(u_tex6, uv); return texture(u_tex7, uv);\n"
    "}\n"
    "ivec2 map_size(int m) {\n"
    "  if (m == 0) return textureSize(u_tex0, 0); if (m == 1) return textureSize(u_tex1, 0);\n"
    "  if (m == 2) return textureSize(u_tex2, 0); if (m == 3) return textureSize(u_tex3, 0);\n"
    "  if (m == 4) return textureSize(u_tex4, 0); if (m == 5) return textureSize(u_tex5, 0);\n"
    "  if (m == 6) return textureSize(u_tex6, 0); return textureSize(u_tex7, 0);\n"
    "}\n"
    "/* A map sampled at a coordinate in texels with 7 fractional bits. */\n"
    "vec4 sample_at(int m, ivec2 c) { return sample_map(m, vec2(c) / (vec2(map_size(m)) * 128.0)); }\n"
    "ivec2 fixpoint_uv(int n) {\n"
    "  vec3 tc = v_tex[n]; ivec4 sz = u_tc_size[n >> 1];\n"
    "  return ivec2(tc.xy / (tc.z == 0.0 ? 1.0 : tc.z) * vec2((n & 1) != 0 ? sz.zw : sz.xy) * 128.0);\n"
    "}\n"
    "int wrap(int v, int w) { return w == 0 ? v : (w == 6 ? 0 : v & ((256 >> (w - 1)) * 128 - 1)); }\n"
    "vec4 swizzle(vec4 c, int t) { ivec4 s = u_swap[t]; return vec4(c[s.x], c[s.y], c[s.z], c[s.w]); }\n"
    "float kfrac(int s) { return s < 8 ? float(8 - s) / 8.0 : 0.0; }\n"
    "vec3 kcolor(int s) {\n"
    "  if (s < 8) return vec3(kfrac(s));\n"
    "  if (s >= 12 && s < 16) return u_konst[s - 12].rgb;\n"
    "  if (s >= 16) { int k = (s - 16) & 3; int c = (s - 16) >> 2; return vec3(u_konst[k][c]); }\n"
    "  return vec3(0.0);\n"
    "}\n"
    "float kalpha(int s) {\n"
    "  if (s < 8) return kfrac(s);\n"
    "  if (s >= 16) { int k = (s - 16) & 3; int c = (s - 16) >> 2; return u_konst[k][c]; }\n"
    "  return 0.0;\n"
    "}\n"
    "bool acmp(int f, float a, float r) {\n"
    "  if (f == 0) return false; if (f == 1) return a < r; if (f == 2) return a == r;\n"
    "  if (f == 3) return a <= r; if (f == 4) return a > r; if (f == 5) return a != r;\n"
    "  if (f == 6) return a >= r; return true;\n"
    "}\n"
    "float q8(float x) { return floor(x * 255.0 + 0.5) / 255.0; }\n"
    "vec3 q8(vec3 x) { return floor(x * 255.0 + 0.5) / 255.0; }\n"
    "void main() {\n"
    "  vec4 reg[4]; reg[0] = u_reg[0]; reg[1] = u_reg[1]; reg[2] = u_reg[2]; reg[3] = u_reg[3];\n"
    "  vec4 last = reg[0];\n"
    "  ivec3 ind_tex[4]; ivec2 ind_coord = ivec2(0);\n"
    "  if (u_ind.y != 0) {\n"
    "    for (int k = 0; k < u_ind.x && k < 4; k++) {\n"
    "      ivec4 is = u_ind_stage[k];\n"
    "      ind_tex[k] = ivec3(floor(sample_at(is.x, fixpoint_uv(is.y) >> is.zw).abg * 255.0 + 0.5));\n"
    "    }\n"
    "  }\n"
    "  for (int i = 0; i < u_num_stages; i++) {\n"
    "    ivec4 map = u_map[i]; ivec4 sel = u_sel[i];\n"
    "    int cmd = u_ind.y != 0 ? u_ind_cmd[i >> 2][i & 3] : 0, bump = 0;\n"
    "    if (cmd != 0) {\n"
    "      /* As gx.frag's indirect: the indirect stage's texel offsets the coordinate. */\n"
    "      int bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;\n"
    "      ivec2 uv = fixpoint_uv(map.y), trans = ivec2(0);\n"
    "      if (bt < u_ind.x && (bs != 0 || mid != 0)) {\n"
    "        ivec3 crd = ind_tex[bt];\n"
    "        if (bs != 0) bump = crd[bs - 1] & (fmt == 1 ? 0xE0 : (fmt == 2 ? 0xF0 : 0xF8));\n"
    "        crd &= ivec3(fmt == 0 ? 255 : (fmt == 1 ? 31 : (fmt == 2 ? 15 : 7)));\n"
    "        int add = fmt == 0 ? -128 : 1;\n"
    "        crd += ivec3((bias & 1) != 0 ? add : 0, (bias & 2) != 0 ? add : 0, (bias & 4) != 0 ? add : 0);\n"
    "        int m = (mid & 3) - 1;\n"
    "        if (m >= 0) {\n"
    "          ivec4 a = u_ind_mtx[2 * m], b = u_ind_mtx[2 * m + 1];\n"
    "          if ((mid >> 2) == 0) trans = ivec2(a.x * crd.x + a.y * crd.y + a.z * crd.z, b.x * crd.x + b.y * crd.y + b.z * crd.z) >> 3;\n"
    "          else if ((mid >> 2) == 1) trans = (uv * crd.x) >> 8;\n"
    "          else trans = (uv * crd.y) >> 8;\n"
    "          trans = a.w >= 0 ? trans >> a.w : trans << -a.w;\n"
    "        }\n"
    "      }\n"
    "      ivec2 c = ivec2(wrap(uv.x, (cmd >> 13) & 7), wrap(uv.y, (cmd >> 16) & 7)) + trans;\n"
    "      ind_coord = ((cmd >> 20) & 1) != 0 ? ind_coord + c : c;\n"
    "      ind_coord = (ind_coord << 8) >> 8;\n"
    "    }\n"
    "    vec4 tex = vec4(1.0);\n"
    "    if (map.z != 0) {\n"
    "      if (cmd != 0) tex = sample_at(map.x, ind_coord);\n"
    "      else { vec3 tc = v_tex[map.y]; tex = sample_map(map.x, tc.xy / (tc.z == 0.0 ? 1.0 : tc.z)); }\n"
    "    }\n"
    "    tex = swizzle(tex, sel.w & 3);\n"
    "    vec4 ras = map.w == 0 ? v_col0 : (map.w == 1 ? v_col1 : vec4(0.0));\n"
    "    if (map.w == 5) ras = vec4(0.0, 0.0, 0.0, float(bump) / 255.0);\n"
    "    else if (map.w == 6) ras = vec4(0.0, 0.0, 0.0, float(bump | (bump >> 5)) / 255.0);\n"
    "    ras = swizzle(ras, sel.w >> 2);\n"
    "    vec3 kc = kcolor(sel.z & 31); float ka = kalpha(sel.z >> 5);\n"
    "    vec3 cin[16];\n"
    "    cin[0] = reg[0].rgb; cin[1] = vec3(reg[0].a); cin[2] = reg[1].rgb; cin[3] = vec3(reg[1].a);\n"
    "    cin[4] = reg[2].rgb; cin[5] = vec3(reg[2].a); cin[6] = reg[3].rgb; cin[7] = vec3(reg[3].a);\n"
    "    cin[8] = tex.rgb; cin[9] = vec3(tex.a); cin[10] = ras.rgb; cin[11] = vec3(ras.a);\n"
    "    cin[12] = vec3(1.0); cin[13] = vec3(0.5); cin[14] = kc; cin[15] = vec3(0.0);\n"
    "    float ain[8];\n"
    "    ain[0] = reg[0].a; ain[1] = reg[1].a; ain[2] = reg[2].a; ain[3] = reg[3].a;\n"
    "    ain[4] = tex.a; ain[5] = ras.a; ain[6] = ka; ain[7] = 0.0;\n"
    "    ivec4 ci = u_cin[i]; ivec4 co = u_cop[i];\n"
    "    vec3 a = clamp(cin[ci.x], 0.0, 1.0), b = clamp(cin[ci.y], 0.0, 1.0), c = clamp(cin[ci.z], 0.0, 1.0), d = cin[ci.w];\n"
    "    vec3 rc;\n"
    "    if (co.y == 3) {\n"
    "      vec3 r;\n"
    "      if (co.z == 3) r = vec3(co.x == 0 ? greaterThan(q8(a), q8(b)) : equal(q8(a), q8(b)));\n"
    "      else { float av = dot(q8(a), vec3(65536.0, 256.0, 1.0)), bv = dot(q8(b), vec3(65536.0, 256.0, 1.0));\n"
    "        if (co.z == 0) { av = q8(a.r); bv = q8(b.r); }\n"
    "        else if (co.z == 1) { av = q8(a.r) + q8(a.g) * 256.0; bv = q8(b.r) + q8(b.g) * 256.0; }\n"
    "        else { av = q8(a.r) + q8(a.g) * 256.0 + q8(a.b) * 65536.0; bv = q8(b.r) + q8(b.g) * 256.0 + q8(b.b) * 65536.0; }\n"
    "        r = vec3(co.x == 0 ? float(av > bv) : float(av == bv)); }\n"
    "      rc = d + r * c;\n"
    "    } else {\n"
    "      vec3 l = mix(a, b, c);\n"
    "      rc = co.x == 0 ? d + l : d - l;\n"
    "      rc += co.y == 1 ? 0.5 : (co.y == 2 ? -0.5 : 0.0);\n"
    "      rc *= co.z == 1 ? 2.0 : (co.z == 2 ? 4.0 : (co.z == 3 ? 0.5 : 1.0));\n"
    "    }\n"
    "    rc = co.w != 0 ? clamp(rc, 0.0, 1.0) : clamp(rc, -1024.0 / 255.0, 1023.0 / 255.0);\n"
    "    ivec4 ai = u_ain[i]; ivec4 ao = u_aop[i];\n"
    "    float aa = clamp(ain[ai.x], 0.0, 1.0), ab = clamp(ain[ai.y], 0.0, 1.0), ac = clamp(ain[ai.z], 0.0, 1.0), ad = ain[ai.w];\n"
    "    float ra;\n"
    "    if (ao.y == 3) {\n"
    "      float r = ao.x == 0 ? float(q8(aa) > q8(ab)) : float(q8(aa) == q8(ab));\n"
    "      ra = ad + r * ac;\n"
    "    } else {\n"
    "      float l = mix(aa, ab, ac);\n"
    "      ra = ao.x == 0 ? ad + l : ad - l;\n"
    "      ra += ao.y == 1 ? 0.5 : (ao.y == 2 ? -0.5 : 0.0);\n"
    "      ra *= ao.z == 1 ? 2.0 : (ao.z == 2 ? 4.0 : (ao.z == 3 ? 0.5 : 1.0));\n"
    "    }\n"
    "    ra = ao.w != 0 ? clamp(ra, 0.0, 1.0) : clamp(ra, -1024.0 / 255.0, 1023.0 / 255.0);\n"
    "    reg[sel.x].rgb = rc; reg[sel.y].a = ra;\n"
    "    last = vec4(rc, ra);\n"
    "  }\n"
    "  last = clamp(last, 0.0, 1.0);\n"
    "  float a8 = floor(last.a * 255.0 + 0.5);\n"
    "  bool p0 = acmp(u_alpha.x, a8, u_alpha_ref.x), p1 = acmp(u_alpha.y, a8, u_alpha_ref.y);\n"
    "  bool pass = u_alpha.z == 0 ? (p0 && p1) : (u_alpha.z == 1 ? (p0 || p1) : (u_alpha.z == 2 ? (p0 != p1) : (p0 == p1)));\n"
    "  if (!pass) discard;\n"
    "  if (u_fog.x != 0) {\n"
    "    int z = int(clamp((1.0 - gl_FragCoord.z) * 16777216.0, 0.0, 16777215.0));\n"
    "    float ze = u_fog.y == 0 ? u_fogf.x * 16777216.0 / float(u_fog.z - (z >> u_fog.w)) : u_fogf.x * float(z) / 16777216.0;\n"
    "    if (u_fog_range[0].z != 0.0) {\n"
    "      float t = clamp(abs(gl_FragCoord.x * u_fog_range[0].x + u_fog_range[0].y) / 32.0 - 1.0, -1.0, 9.0);\n"
    "      int lo = int(floor(t)), hi = min(lo + 1, 9);\n"
    "      float kl = lo < 0 ? 1.0 : u_fog_range[(lo + 4) >> 2][lo & 3];\n"
    "      float kh = u_fog_range[(hi + 4) >> 2][hi & 3];\n"
    "      ze /= max(mix(kl, kh, t - float(lo)), 1.0 / 256.0);\n"
    "    }\n"
    "    float f = clamp(ze - u_fogf.y, 0.0, 1.0);\n"
    "    if (u_fog.x == 4) f = 1.0 - exp2(-8.0 * f);\n"
    "    else if (u_fog.x == 5) f = 1.0 - exp2(-8.0 * f * f);\n"
    "    else if (u_fog.x == 6) f = exp2(-8.0 * (1.0 - f));\n"
    "    else if (u_fog.x == 7) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"
    "    last.rgb = mix(last.rgb, u_fog_color.rgb, floor(f * 256.0 + 0.5) / 256.0);\n"
    "  }\n"
    "  o_color = last;\n"
    "}\n";

static GLuint sProgram, sVao, sVbo, sIbo, sXfUbo;
static GLint uNumStages, uCin, uAin, uCop, uAop, uMap, uSel, uReg, uKonst, uSwap, uAlpha, uAlphaRef, uFog, uFogF, uFogColor;
static GLint uInd, uIndCmd, uIndStage, uIndMtx, uTcSize, uFogRange, uCanvas;

static GLuint compile(GLenum type, const char* src) {
    GLuint s = p_glCreateShader(type);
    GLint ok;
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "gl: shader error:\n%s\n", log);
        exit(1);
    }
    return s;
}

static void build_program(void) {
    static const char* attribs[] = {"a_mtx0", "a_mtx1", "a_mtx2", "a_pos", "a_nrm", "a_col0",
                                    "a_col1", "a_tex01", "a_tex23", "a_tex45", "a_tex67"};
    GLint ok;
    int i;
    sProgram = p_glCreateProgram();
    p_glAttachShader(sProgram, compile(GL_VERTEX_SHADER, kVertexShader));
    p_glAttachShader(sProgram, compile(GL_FRAGMENT_SHADER, kFragmentShader));
    for (i = 0; i < 11; i++) p_glBindAttribLocation(sProgram, (GLuint)i, attribs[i]);
    p_glLinkProgram(sProgram);
    p_glGetProgramiv(sProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(sProgram, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        exit(1);
    }
    p_glUseProgram(sProgram);
    for (i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof(name), "u_tex%d", i);
        p_glUniform1i(p_glGetUniformLocation(sProgram, name), i);
    }
    uNumStages = p_glGetUniformLocation(sProgram, "u_num_stages");
    uCin = p_glGetUniformLocation(sProgram, "u_cin");
    uAin = p_glGetUniformLocation(sProgram, "u_ain");
    uCop = p_glGetUniformLocation(sProgram, "u_cop");
    uAop = p_glGetUniformLocation(sProgram, "u_aop");
    uMap = p_glGetUniformLocation(sProgram, "u_map");
    uSel = p_glGetUniformLocation(sProgram, "u_sel");
    uReg = p_glGetUniformLocation(sProgram, "u_reg");
    uKonst = p_glGetUniformLocation(sProgram, "u_konst");
    uSwap = p_glGetUniformLocation(sProgram, "u_swap");
    uAlpha = p_glGetUniformLocation(sProgram, "u_alpha");
    uAlphaRef = p_glGetUniformLocation(sProgram, "u_alpha_ref");
    uFog = p_glGetUniformLocation(sProgram, "u_fog");
    uFogF = p_glGetUniformLocation(sProgram, "u_fogf");
    uFogColor = p_glGetUniformLocation(sProgram, "u_fog_color");
    uInd = p_glGetUniformLocation(sProgram, "u_ind");
    uIndCmd = p_glGetUniformLocation(sProgram, "u_ind_cmd");
    uIndStage = p_glGetUniformLocation(sProgram, "u_ind_stage");
    uIndMtx = p_glGetUniformLocation(sProgram, "u_ind_mtx");
    uTcSize = p_glGetUniformLocation(sProgram, "u_tc_size");
    uFogRange = p_glGetUniformLocation(sProgram, "u_fog_range");
    uCanvas = p_glGetUniformLocation(sProgram, "u_canvas");

    p_glGenVertexArrays(1, &sVao);
    p_glBindVertexArray(sVao);
    p_glGenBuffers(1, &sVbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    p_glGenBuffers(1, &sIbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sIbo); /* part of the vertex array's state */
    for (i = 0; i < 3; i++) {
        p_glEnableVertexAttribArray(i);
        p_glVertexAttribPointer(i, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GXVertex), (void*)(offsetof(GXVertex, mtx) + i * 4));
    }
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)offsetof(GXVertex, pos));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)offsetof(GXVertex, nrm));
    p_glEnableVertexAttribArray(5);
    p_glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GXVertex), (void*)offsetof(GXVertex, color[0]));
    p_glEnableVertexAttribArray(6);
    p_glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GXVertex), (void*)offsetof(GXVertex, color[1]));
    for (i = 0; i < 4; i++) {
        p_glEnableVertexAttribArray(7 + i);
        p_glVertexAttribPointer(7 + i, 4, GL_FLOAT, GL_FALSE, sizeof(GXVertex), (void*)(offsetof(GXVertex, tex) + i * 4 * sizeof(float)));
    }

    /* The XF state, for the vertex shader. */
    p_glGenBuffers(1, &sXfUbo);
    p_glBindBuffer(GL_UNIFORM_BUFFER, sXfUbo);
    p_glBufferData(GL_UNIFORM_BUFFER, GX_XF_WORDS * 4, NULL, GL_DYNAMIC_DRAW);
    p_glUniformBlockBinding(sProgram, p_glGetUniformBlockIndex(sProgram, "XF"), 0);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 0, sXfUbo);
}

/* EFB copies: a full-screen triangle that samples the EFB rectangle and
 * converts it to the copy format, stored as the texture reads back. The
 * value of a narrow format is truncated to its bits and expanded again,
 * as the console's copy and texture fetch do. */
static const char* kCopyVertexShader =
    "#version 330 core\n"
    "uniform vec4 u_src;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID >> 1) * 2);\n"
    "    v_uv = mix(u_src.xy, u_src.zw, p);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char* kCopyFragmentShader =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_efb;\n"
    "uniform ivec4 u_mode; /* copy format, intensity, EFB has alpha */\n"
    "float bits(float v, int n) {\n"
    "    float i = float(int(v * 255.0 + 0.5) >> (8 - n));\n"
    "    return floor(i * 255.0 / float((1 << n) - 1) + 0.5) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = texture(u_efb, v_uv);\n"
    "    int f = u_mode.x;\n"
    "    float i;\n"
    "    if (u_mode.z == 0) c.a = 1.0;\n"
    "    /* The intensity formats store luma (BT.601, studio range). */\n"
    "    i = u_mode.y != 0 ? clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0) : c.r;\n"
    "    if (f == 0) o_color = vec4(bits(i, 4));\n"
    "    else if (f == 1 || f == 8) o_color = vec4(i);\n"
    "    else if (f == 2) o_color = vec4(vec3(bits(i, 4)), bits(c.a, 4));\n"
    "    else if (f == 3) o_color = vec4(vec3(i), c.a);\n"
    "    else if (f == 4) o_color = vec4(bits(c.r, 5), bits(c.g, 6), bits(c.b, 5), 1.0);\n"
    "    else if (f == 5 && c.a >= 224.0 / 255.0) o_color = vec4(bits(c.r, 5), bits(c.g, 5), bits(c.b, 5), 1.0);\n"
    "    else if (f == 5) o_color = vec4(bits(c.r, 4), bits(c.g, 4), bits(c.b, 4), bits(c.a, 3));\n"
    "    else if (f == 7) o_color = vec4(c.a);\n"
    "    else if (f == 9) o_color = vec4(c.g);\n"
    "    else if (f == 10) o_color = vec4(c.b);\n"
    "    else if (f == 11) o_color = vec4(vec3(c.g), c.r); /* IA8 layout: A = R, I = G */\n"
    "    else if (f == 12) o_color = vec4(vec3(c.b), c.g);\n"
    "    else o_color = c;\n"
    "}\n";

static GLuint sCopyProgram, sCopyVao;
static GLint uCopySrc, uCopyMode;

static void build_copy_program(void) {
    GLint ok;
    sCopyProgram = p_glCreateProgram();
    p_glAttachShader(sCopyProgram, compile(GL_VERTEX_SHADER, kCopyVertexShader));
    p_glAttachShader(sCopyProgram, compile(GL_FRAGMENT_SHADER, kCopyFragmentShader));
    p_glLinkProgram(sCopyProgram);
    p_glGetProgramiv(sCopyProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(sCopyProgram, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        exit(1);
    }
    p_glUseProgram(sCopyProgram);
    p_glUniform1i(p_glGetUniformLocation(sCopyProgram, "u_efb"), 0);
    uCopySrc = p_glGetUniformLocation(sCopyProgram, "u_src");
    uCopyMode = p_glGetUniformLocation(sCopyProgram, "u_mode");
    p_glGenVertexArrays(1, &sCopyVao);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
}

/* The settings menu, an image with alpha laid over the presented picture.
 * Its rows run top to bottom. */
static const char* kOverlayVertexShader =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID >> 1) * 2);\n"
    "    v_uv = vec2(p.x, 1.0 - p.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char* kOverlayFragmentShader =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_image;\n"
    "void main() { o_color = texture(u_image, v_uv); }\n";

static GLuint sOverlayProgram, sOverlayTex;

static void build_overlay_program(void) {
    GLint ok;
    sOverlayProgram = p_glCreateProgram();
    p_glAttachShader(sOverlayProgram, compile(GL_VERTEX_SHADER, kOverlayVertexShader));
    p_glAttachShader(sOverlayProgram, compile(GL_FRAGMENT_SHADER, kOverlayFragmentShader));
    p_glLinkProgram(sOverlayProgram);
    p_glGetProgramiv(sOverlayProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetProgramInfoLog(sOverlayProgram, sizeof(log), NULL, log);
        fprintf(stderr, "gl: link error:\n%s\n", log);
        exit(1);
    }
    p_glUseProgram(sOverlayProgram);
    p_glUniform1i(p_glGetUniformLocation(sOverlayProgram, "u_image"), 0);
    p_glUseProgram(sProgram);
    glGenTextures(1, &sOverlayTex);
    glBindTexture(GL_TEXTURE_2D, sOverlayTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* ------------------------------------------------------------------------ */
/* Window and EFB */

static HDC sDC; /* the game window's */
/* The DC the GL context is current on: a hidden window's of its own when
 * presenting through DXGI, else sDC. */
static HDC sGlDC;
/* What the window's picture is drawn into: sWinFbo, the size of the
 * window's client area, when presenting through DXGI, else 0. */
static GLuint sWinFbo;
/* EFB pixels to GL pixels. In 16:9 an EFB pixel is shown wider
 * (win_pixel_aspect), so the EFB gets as many more pixels across: the
 * stretched picture is then at full resolution. */
static double sScaleX = 2, sScaleY = 2;
static double sPixelAspect = 1; /* win_pixel_aspect() */
#define GLX(x) ((GLint)floor((x) * sScaleX + 0.5))
#define GLY(y) ((GLint)floor((y) * sScaleY + 0.5))
/* The canvas's margins (gx_canvas), as the EFB was made for: positions on
 * it are in EFB pixels from its top left corner (gx_canvas_draw,
 * gx_canvas_rect), CANVAS_W by CANVAS_H. */
static int sMX, sMY;
#define CANVAS_W (EFB_W + 2 * sMX)
#define CANVAS_H (EFB_H + 2 * sMY)
/* Draws go to sDrawFbo. Without anti-aliasing that is sEfbFbo, whose
 * color texture the copies and the presented picture read; with it, a
 * multisampled framebuffer resolved into sEfbFbo when they need it. */
static GLuint sEfbFbo, sEfbColor, sEfbDepth;
static GLuint sDrawFbo, sMsFbo, sMsColor, sMsDepth;
static int sMsDirty;
static GLuint sWhite;
static const char* sDumpDir;
static int sDumpEvery = 30;
static unsigned sDumpFrom;
static unsigned sPresented;
/* The settings as applied. */
static int sScale, sMsaa = 1, sMaxSamples = 1;
static float sMaxAniso; /* the most anisotropic filtering this GPU does; 0 if none */

static int gl_scale(void) { return sScale; }
static int gl_msaa(void) {
    int m;
    for (m = g_settings.msaa; m > sMaxSamples; m /= 2) {}
    return m;
}

/* (Re)allocates the EFB at the current scale and sample count, and the
 * copy textures at the matching scale (keeping their names: the texture
 * cache holds them). What they held is lost; the game redraws it. */
static void alloc_efb(void);

static void gl_apply_settings(void) {
    int w, h, s, m, mx, my;
    win_size(&w, &h);
    if (h <= 0) return; /* minimized */
    s = gx_wanted_scale(h);
    for (m = g_settings.msaa; m > sMaxSamples; m /= 2) {}
    gx_canvas(&mx, &my);
    if (s == sScale && m == sMsaa && mx == sMX && my == sMY) return;
    sScale = s;
    sMsaa = m;
    sMX = mx;
    sMY = my;
    sScaleY = s;
    sScaleX = s * sPixelAspect;
    alloc_efb();
    fprintf(stderr, "gl: internal resolution %dx (%d x %d), anti-aliasing %s\n", s, GLX(CANVAS_W), GLY(CANVAS_H),
            m > 1 ? (m == 2 ? "2x" : m == 4 ? "4x" : "8x") : "off");
}

/* Brings sEfbFbo up to date with the multisampled draws. */
static void resolve(void) {
    gl_forget();
    if (sDrawFbo == sEfbFbo || !sMsDirty) return;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sMsFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEfbFbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, GLX(CANVAS_W), GLY(CANVAS_H), 0, 0, GLX(CANVAS_W), GLY(CANVAS_H), GL_COLOR_BUFFER_BIT,
                        GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    sMsDirty = 0;
}

/* ------------------------------------------------------------------------ */
/* Presenting
 *
 * The window is shown through a DXGI swapchain of our own, with the GL
 * picture handed over in a D3D11 texture (WGL_NV_DX_interop2), rather
 * than by SwapBuffers. On NVIDIA, SwapBuffers on a window presents
 * through a DXGI swapchain the driver keeps itself ("layered on DXGI"),
 * and that path crashed about once in 40 minutes of play, usually just
 * after a slow frame: one of the driver's threads (or the game thread,
 * inside SwapBuffers) called ID3D11DeviceContext4::Wait with a fence that
 * was never made, a read of address 0 in d3d11.dll. That call is not made
 * for ordinary frames (none in 15,000 presents), only on some rarer path
 * of the driver's, and nothing in the GL calls made before it is wrong
 * (a debug context reports nothing). The GL context is made on a hidden
 * window of its own, never presented, so that path is never entered.
 *
 * Flip model, discarding, and with tearing allowed where the system
 * supports it, so presenting doesn't wait for the display (as
 * SwapInterval 0 did). Each frame the picture is drawn into sWinFbo as
 * it was into the window, then flipped into the shared texture (D3D's
 * row 0 is the top) and copied to the back buffer.
 *
 * Without the extension, or with RECOMP_GL_PRESENT=swapbuffers, the
 * context moves to the game's window and SwapBuffers presents it. */

typedef HANDLE(WINAPI* PFN_wglDXOpenDeviceNV)(void*);
typedef HANDLE(WINAPI* PFN_wglDXRegisterObjectNV)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFN_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE*);
static PFN_wglDXRegisterObjectNV p_wglDXRegisterObjectNV;
static PFN_wglDXUnregisterObjectNV p_wglDXUnregisterObjectNV;
static PFN_wglDXLockObjectsNV p_wglDXLockObjectsNV, p_wglDXUnlockObjectsNV;
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

static ID3D11Device* sD3d;
static ID3D11DeviceContext* sD3dCtx;
static IDXGISwapChain1* sChain;
static ID3D11Texture2D* sShared; /* the picture, written by GL */
static HANDLE sInteropDev, sInterop;
static GLuint sWinColor, sSharedFbo, sSharedColor;
static int sChainW, sChainH, sTearing;

/* Sets up presenting through DXGI; 0 if it can't be. The D3D11 device
 * must be on the GL context's adapter: the first adapter the interop
 * accepts. */
static int dx_init(void) {
    PFN_wglDXOpenDeviceNV open = (PFN_wglDXOpenDeviceNV)(void*)wglGetProcAddress("wglDXOpenDeviceNV");
    IDXGIFactory1* f1 = NULL;
    IDXGIFactory2* f2 = NULL;
    IDXGIFactory5* f5 = NULL;
    IDXGIAdapter1* adapter;
    DXGI_SWAP_CHAIN_DESC1 d;
    UINT i;
    p_wglDXRegisterObjectNV = (PFN_wglDXRegisterObjectNV)(void*)wglGetProcAddress("wglDXRegisterObjectNV");
    p_wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)(void*)wglGetProcAddress("wglDXUnregisterObjectNV");
    p_wglDXLockObjectsNV = (PFN_wglDXLockObjectsNV)(void*)wglGetProcAddress("wglDXLockObjectsNV");
    p_wglDXUnlockObjectsNV = (PFN_wglDXLockObjectsNV)(void*)wglGetProcAddress("wglDXUnlockObjectsNV");
    if (!open || !p_wglDXRegisterObjectNV || !p_wglDXUnregisterObjectNV || !p_wglDXLockObjectsNV ||
        !p_wglDXUnlockObjectsNV) {
        fprintf(stderr, "gl: no WGL_NV_DX_interop2\n");
        return 0;
    }
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&f1))) return 0;
    for (i = 0; !sInteropDev && IDXGIFactory1_EnumAdapters1(f1, i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 ad;
        IDXGIAdapter1_GetDesc1(adapter, &ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D11CreateDevice((IDXGIAdapter*)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                        D3D11_SDK_VERSION, &sD3d, NULL, &sD3dCtx))) {
            sInteropDev = open(sD3d);
            if (!sInteropDev) {
                ID3D11DeviceContext_Release(sD3dCtx);
                ID3D11Device_Release(sD3d);
                sD3d = NULL;
                sD3dCtx = NULL;
            }
        }
        IDXGIAdapter1_Release(adapter);
    }
    if (!sInteropDev || FAILED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory2, (void**)&f2))) {
        fprintf(stderr, "gl: no D3D11 device the GL context can share with\n");
        IDXGIFactory1_Release(f1);
        return 0;
    }
    if (SUCCEEDED(IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory5, (void**)&f5))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                        sizeof(tearing))))
            sTearing = tearing != FALSE;
        IDXGIFactory5_Release(f5);
    }
    memset(&d, 0, sizeof(d));
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = sTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown*)sD3d, (HWND)win_handle(), &d, NULL, NULL, &sChain))) {
        fprintf(stderr, "gl: no DXGI swapchain for the window\n");
        sChain = NULL;
    } else {
        /* Alt+Enter is the game's own (borderless fullscreen). */
        IDXGIFactory2_MakeWindowAssociation(f2, (HWND)win_handle(), DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }
    IDXGIFactory2_Release(f2);
    IDXGIFactory1_Release(f1);
    if (!sChain) return 0;
    p_glGenFramebuffers(1, &sWinFbo);
    p_glGenFramebuffers(1, &sSharedFbo);
    return 1;
}

/* Sizes the swapchain, the shared texture and sWinFbo to the window's
 * client area, w x h (not 0). */
static void dx_size(int w, int h) {
    D3D11_TEXTURE2D_DESC t;
    HRESULT hr;
    if (w == sChainW && h == sChainH) return;
    if (sInterop) {
        p_wglDXUnregisterObjectNV(sInteropDev, sInterop);
        sInterop = NULL;
        p_glDeleteRenderbuffers(1, &sSharedColor);
        p_glDeleteRenderbuffers(1, &sWinColor);
        ID3D11Texture2D_Release(sShared);
        sShared = NULL;
    }
    hr = IDXGISwapChain1_ResizeBuffers(sChain, 0, w, h, DXGI_FORMAT_UNKNOWN,
                                       sTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        /* Nothing is shown until a resize works (tried each frame). */
        static int reported;
        if (!reported++) fprintf(stderr, "gl: resizing the swapchain to %d x %d failed (%08lX)\n", w, h, (unsigned long)hr);
        return;
    }
    memset(&t, 0, sizeof(t));
    t.Width = w;
    t.Height = h;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1;
    t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(sD3d, &t, NULL, &sShared))) {
        fprintf(stderr, "gl: no %d x %d shared texture\n", w, h);
        sShared = NULL;
        return;
    }
    p_glGenRenderbuffers(1, &sSharedColor);
    sInterop = p_wglDXRegisterObjectNV(sInteropDev, sShared, sSharedColor, GL_RENDERBUFFER, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!sInterop) {
        fprintf(stderr, "gl: sharing the %d x %d texture failed\n", w, h);
        p_glDeleteRenderbuffers(1, &sSharedColor);
        ID3D11Texture2D_Release(sShared);
        sShared = NULL;
        return;
    }
    p_wglDXLockObjectsNV(sInteropDev, 1, &sInterop);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sSharedFbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sSharedColor);
    p_wglDXUnlockObjectsNV(sInteropDev, 1, &sInterop);
    p_glGenRenderbuffers(1, &sWinColor);
    p_glBindRenderbuffer(GL_RENDERBUFFER, sWinColor);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sWinColor);
    sChainW = w;
    sChainH = h;
}

/* Writes the shared texture, as presented, to a BMP (RECOMP_GX_DUMP). */
static void dump_shared(const char* path) {
    D3D11_TEXTURE2D_DESC t;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D* staging;
    FILE* f;
    if (!sShared) return;
    ID3D11Texture2D_GetDesc(sShared, &t);
    t.Usage = D3D11_USAGE_STAGING;
    t.BindFlags = 0;
    t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(sD3d, &t, NULL, &staging))) return;
    ID3D11DeviceContext_CopyResource(sD3dCtx, (ID3D11Resource*)staging, (ID3D11Resource*)sShared);
    if (SUCCEEDED(ID3D11DeviceContext_Map(sD3dCtx, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &m))) {
        if ((f = fopen(path, "wb")) != NULL) {
            uint8_t hdr[54] = {'B', 'M'};
            uint8_t* row = (uint8_t*)malloc((size_t)t.Width * 4);
            UINT x, y;
            *(uint32_t*)(hdr + 2) = 54 + t.Width * t.Height * 4;
            hdr[10] = 54;
            hdr[14] = 40;
            *(int32_t*)(hdr + 18) = (int32_t)t.Width;
            *(int32_t*)(hdr + 22) = -(int32_t)t.Height; /* top row first, as D3D has it */
            hdr[26] = 1;
            hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            for (y = 0; y < t.Height; y++) {
                const uint8_t* px = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (x = 0; x < t.Width; x++) {
                    row[x * 4] = px[x * 4 + 2];
                    row[x * 4 + 1] = px[x * 4 + 1];
                    row[x * 4 + 2] = px[x * 4];
                    row[x * 4 + 3] = 255;
                }
                fwrite(row, 4, t.Width, f);
            }
            free(row);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(sD3dCtx, (ID3D11Resource*)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

/* Shows what was drawn into the window's framebuffer, w x h; 1 if it was
 * left out, as may_drop allows. */
static int window_present(int w, int h, int may_drop) {
    ID3D11Texture2D* back;
    HRESULT hr;
    static int failures;
    if (!sChain) {
        SwapBuffers(sDC);
        return 0;
    }
    if (!sInterop || w != sChainW || h != sChainH) return 0; /* minimized, or no shared texture */
    if (!p_wglDXLockObjectsNV(sInteropDev, 1, &sInterop)) {
        if (failures++ < 5) fprintf(stderr, "gl: locking the shared texture failed\n");
        return 0;
    }
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sWinFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sSharedFbo);
    p_glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p_wglDXUnlockObjectsNV(sInteropDev, 1, &sInterop);
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(sChain, 0, &IID_ID3D11Texture2D, (void**)&back))) {
        ID3D11DeviceContext_CopyResource(sD3dCtx, (ID3D11Resource*)back, (ID3D11Resource*)sShared);
        ID3D11Texture2D_Release(back);
    }
    hr = IDXGISwapChain1_Present(sChain, 0,
                                 (sTearing ? DXGI_PRESENT_ALLOW_TEARING : 0) | (may_drop ? DXGI_PRESENT_DO_NOT_WAIT : 0));
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return 1;
    if (FAILED(hr) && failures++ < 5) fprintf(stderr, "gl: present failed (%08lX)\n", (unsigned long)hr);
    return 0;
}

/* RECOMP_GL_DEBUG=1: a debug context (compatibility profile, as the
 * shaders need), with the driver's messages about errors and misuse
 * printed as each call makes them. */
static void APIENTRY debug_message(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                   const GLchar* message, const void* user) {
    static unsigned count;
    (void)source;
    (void)length;
    (void)user;
    if (severity == 0x826B /* GL_DEBUG_SEVERITY_NOTIFICATION */) return;
    if (++count > 200) return;
    fprintf(stderr, "gl debug: type %04X id %u severity %04X: %s\n", type, id, severity, message);
}

static HGLRC debug_context(HGLRC legacy) {
    typedef HGLRC(WINAPI * PFN_create)(HDC, HGLRC, const int*);
    typedef void(APIENTRY * PFN_callback)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*);
    typedef void(APIENTRY * PFN_set_callback)(PFN_callback, const void*);
    static const int attribs[] = {0x2091, 4,      /* WGL_CONTEXT_MAJOR_VERSION_ARB */
                                  0x2092, 6,      /* WGL_CONTEXT_MINOR_VERSION_ARB */
                                  0x2094, 1,      /* WGL_CONTEXT_FLAGS_ARB: WGL_CONTEXT_DEBUG_BIT_ARB */
                                  0x9126, 2,      /* WGL_CONTEXT_PROFILE_MASK_ARB: compatibility */
                                  0};
    PFN_create create = (PFN_create)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
    PFN_set_callback set_callback;
    HGLRC rc = create ? create(sGlDC, NULL, attribs) : NULL;
    if (!rc) {
        fprintf(stderr, "gl: no debug context\n");
        return legacy;
    }
    wglMakeCurrent(sGlDC, rc);
    wglDeleteContext(legacy);
    set_callback = (PFN_set_callback)(void*)wglGetProcAddress("glDebugMessageCallback");
    if (set_callback) {
        glEnable(0x92E0); /* GL_DEBUG_OUTPUT */
        glEnable(0x8242); /* GL_DEBUG_OUTPUT_SYNCHRONOUS */
        set_callback(debug_message, NULL);
    }
    fprintf(stderr, "gl: debug context, synchronous debug output\n");
    return rc;
}

static int gl_init(void) {
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC rc;
    const char* v;
    uint32_t white = 0xFFFFFFFF;
    sPixelAspect = win_pixel_aspect();
    sDumpDir = getenv("RECOMP_GX_DUMP");
    if ((v = getenv("RECOMP_GX_DUMP_EVERY")) && atoi(v) > 0) sDumpEvery = atoi(v);
    if ((v = getenv("RECOMP_GX_DUMP_FROM")) && atoi(v) > 0) sDumpFrom = (unsigned)atoi(v);

    sDC = GetDC((HWND)win_handle());
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    /* The context starts on a hidden window of its own (see Presenting). */
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "PokeParkRecompGL";
    RegisterClassA(&wc);
    sGlDC = GetDC(CreateWindowA("PokeParkRecompGL", "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL));
    SetPixelFormat(sGlDC, ChoosePixelFormat(sGlDC, &pfd), &pfd);
    rc = wglCreateContext(sGlDC);
    wglMakeCurrent(sGlDC, rc);
    if ((v = getenv("RECOMP_GL_DEBUG")) && *v && *v != '0') rc = debug_context(rc);
    load_gl();
    v = getenv("RECOMP_GL_PRESENT");
    if (!(v && strcmp(v, "swapbuffers") == 0) && dx_init()) {
        fprintf(stderr, "gl: presenting through DXGI (flip model%s)\n", sTearing ? ", tearing allowed" : "");
    } else {
        /* SwapBuffers on the game's window: the context moves there. */
        SetPixelFormat(sDC, GetPixelFormat(sGlDC), &pfd);
        wglMakeCurrent(sDC, rc);
        sGlDC = sDC;
        fprintf(stderr, "gl: presenting with SwapBuffers\n");
    }
    fprintf(stderr, "gl: %s, %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    glGetIntegerv(GL_MAX_SAMPLES, &sMaxSamples);
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &sMaxAniso);
    while (glGetError() != GL_NO_ERROR) {}
    build_program();
    build_copy_program();
    build_overlay_program();
    pSwapInterval = (BOOL(WINAPI*)(int))(void*)wglGetProcAddress("wglSwapIntervalEXT");
    if (pSwapInterval) pSwapInterval(0);

    glGenTextures(1, &sEfbColor);
    glBindTexture(GL_TEXTURE_2D, sEfbColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glGenRenderbuffers(1, &sEfbDepth);
    p_glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    p_glGenFramebuffers(1, &sEfbFbo);
    gl_apply_settings();

    glGenTextures(1, &sWhite);
    glBindTexture(GL_TEXTURE_2D, sWhite);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Textures */

static GLint wrap_mode(int w) { return w == 0 ? GL_CLAMP_TO_EDGE : (w == 1 ? GL_REPEAT : GL_MIRRORED_REPEAT); }

static void upload_levels(int fresh, int width, int height, int levels, const uint8_t* rgba) {
    int l;
    for (l = 0; l < levels; l++) {
        if (fresh) glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        else glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        rgba += (size_t)width * height * 4;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
}

/* Sampled as the game set it: TX_SETMODE0's wrap, magnification and
 * minification filters (mipmap: none, nearest or linear), and LOD bias.
 * Anisotropic filtering, as the setting says, on what is filtered
 * linearly. */
/* Textures are RGBA8 alone here (no texture_pixels). */
static uint32_t gl_texture(int pix, int width, int height, int levels, const uint8_t* rgba, uint32_t mode0) {
    static const GLenum kMin[2][3] = {{GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR},
                                      {GL_LINEAR, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR}};
    int mip = (mode0 >> 5) & 3, min_linear = (mode0 >> 7) & 1;
    GLuint t;
    (void)pix;
    gl_forget();
    if (mip > 2) mip = 2;
    if (levels <= 1) mip = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    upload_levels(1, width, height, levels, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(mode0 & 3));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode((mode0 >> 2) & 3));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, kMin[min_linear][mip]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    if (mip) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f);
    if (sMaxAniso > 1 && g_settings.aniso > 1 && min_linear)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                        (float)g_settings.aniso < sMaxAniso ? (float)g_settings.aniso : sMaxAniso);
    return t;
}

static void gl_texture_update(uint32_t handle, int pix, int width, int height, int levels, const uint8_t* rgba) {
    (void)pix;
    gl_forget();
    glBindTexture(GL_TEXTURE_2D, handle);
    upload_levels(0, width, height, levels, rgba);
}

static void gl_texture_delete(const uint32_t* handles, int n) { glDeleteTextures(n, handles); }

/* ------------------------------------------------------------------------ */
/* Drawing */

static const GLenum kBlendSrc[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
                                    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
static const GLenum kBlendDst[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
/* Without an alpha channel (RGB8 and RGB565 EFB formats) the EFB's
 * alpha reads as 1: the bloom in Mew's intro weights its copies by it. */
static GLenum blend_factor(GLenum f, int efb_alpha) {
    if (efb_alpha) return f;
    if (f == GL_DST_ALPHA) return GL_ONE;
    if (f == GL_ONE_MINUS_DST_ALPHA) return GL_ZERO;
    return f;
}
/* GL state as draws last set it, so a draw changes only what differs
 * (consecutive batches differ in a few fields; each GL call costs driver
 * time). Anything else that changes GL state (copies, clears, presenting,
 * making textures) calls gl_forget, and the next draw sets it all. The
 * TEV uniforms belong to the draw program alone and are never forgotten. */
static struct {
    int valid;
    GLuint tex[8];
    int blend;
    GLenum blend_src, blend_dst, blend_eq;
    GLboolean color_mask[4], depth_mask;
    int depth;
    GLenum depth_func;
    int cull;
    GLenum cull_face;
    GLint viewport[4], scissor[4];
    double depth_range[2];
} sGl;

static void gl_forget(void) { sGl.valid = 0; }

/* Uploads a uniform array only when it differs from what was uploaded. */
#define UNIFORM_IF_CHANGED(fn, loc, n, values, last)                                                          \
    do {                                                                                                     \
        if (!sUniformsSet || memcmp(values, last, (size_t)(n) * 4 * sizeof(values[0])) != 0) {               \
            memcpy(last, values, (size_t)(n) * 4 * sizeof(values[0]));                                       \
            fn(loc, n, values);                                                                              \
        }                                                                                                    \
    } while (0)

/* Mirrored for the reversed depth. */
static const GLenum kDepthFunc[8] = {GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS};

/* Sets everything a draw needs but its XF state and vertices. */
static void apply_draw_state(const GXDrawState* st) {
    static GLint lcin[64], lain[64], lcop[64], laop[64], lmap[64], lsel[64], lswap[16], lalpha[4];
    static GLfloat lreg[16], lkonst[16], laref[4], lfogf[4], lfogc[4];
    static GLint lfog[4];
    static int sUniformsSet, lstages;
    GLint cin[64], ain[64], cop[64], aop[64], map[64], sel[64], swap[16], alpha[4];
    GLfloat reg[16], konst[16], aref[4], fogf[4], fogc[4];
    GLint fog[4];
    int i, all = !sGl.valid;
    sMsDirty = 1;
    if (all) {
        p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
        p_glUseProgram(sProgram);
        p_glBindVertexArray(sVao);
        p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
        p_glBindBuffer(GL_UNIFORM_BUFFER, sXfUbo);
    }

    for (i = 0; i < st->num_stages; i++) {
        const GXTevStage* s = &st->stage[i];
        int k;
        for (k = 0; k < 4; k++) {
            cin[4 * i + k] = s->color_in[k];
            ain[4 * i + k] = s->alpha_in[k];
        }
        cop[4 * i + 0] = s->color_op;
        cop[4 * i + 1] = s->color_bias;
        cop[4 * i + 2] = s->color_scale;
        cop[4 * i + 3] = s->color_clamp;
        aop[4 * i + 0] = s->alpha_op;
        aop[4 * i + 1] = s->alpha_bias;
        aop[4 * i + 2] = s->alpha_scale;
        aop[4 * i + 3] = s->alpha_clamp;
        map[4 * i + 0] = s->texmap;
        map[4 * i + 1] = s->texcoord;
        map[4 * i + 2] = s->tex_enable;
        /* (The bump alphas, 5 and 6, with an indirect stage to give them.) */
        map[4 * i + 3] = s->channel == 7 || (st->ind_any && (s->channel == 5 || s->channel == 6)) ? s->channel : (s->channel & 1);
        sel[4 * i + 0] = s->color_dest;
        sel[4 * i + 1] = s->alpha_dest;
        sel[4 * i + 2] = s->kcolor_sel | (s->kalpha_sel << 5);
        sel[4 * i + 3] = s->tex_swap | (s->ras_swap << 2);
    }
    for (i = 0; i < 4; i++) {
        int k;
        for (k = 0; k < 4; k++) {
            reg[4 * i + k] = st->tev_reg[i][k];
            konst[4 * i + k] = st->konst[i][k];
            swap[4 * i + k] = st->swap_table[i][k];
        }
    }
    alpha[0] = st->alpha_func[0];
    alpha[1] = st->alpha_func[1];
    alpha[2] = st->alpha_logic;
    alpha[3] = 0;
    aref[0] = (float)st->alpha_ref[0];
    aref[1] = (float)st->alpha_ref[1];
    aref[2] = aref[3] = 0;
    fog[0] = st->fog_type;
    fog[1] = st->fog_ortho;
    fog[2] = st->fog_b_mag;
    fog[3] = st->fog_b_shift;
    fogf[0] = st->fog_a;
    fogf[1] = st->fog_c;
    fogf[2] = fogf[3] = 0;
    fogc[0] = st->fog_color[0];
    fogc[1] = st->fog_color[1];
    fogc[2] = st->fog_color[2];
    fogc[3] = 1;

    if (!sUniformsSet || lstages != st->num_stages) {
        p_glUniform1i(uNumStages, st->num_stages);
        lstages = st->num_stages;
        sUniformsSet = 0; /* the arrays' lengths changed: upload them all */
    }
    UNIFORM_IF_CHANGED(p_glUniform4iv, uCin, st->num_stages, cin, lcin);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uAin, st->num_stages, ain, lain);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uCop, st->num_stages, cop, lcop);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uAop, st->num_stages, aop, laop);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uMap, st->num_stages, map, lmap);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uSel, st->num_stages, sel, lsel);
    UNIFORM_IF_CHANGED(p_glUniform4fv, uReg, 4, reg, lreg);
    UNIFORM_IF_CHANGED(p_glUniform4fv, uKonst, 4, konst, lkonst);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uSwap, 4, swap, lswap);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uAlpha, 1, alpha, lalpha);
    UNIFORM_IF_CHANGED(p_glUniform4fv, uAlphaRef, 1, aref, laref);
    UNIFORM_IF_CHANGED(p_glUniform4iv, uFog, 1, fog, lfog);
    UNIFORM_IF_CHANGED(p_glUniform4fv, uFogF, 1, fogf, lfogf);
    UNIFORM_IF_CHANGED(p_glUniform4fv, uFogColor, 1, fogc, lfogc);
    {
        static GLfloat lrange[16];
        GLfloat range[16] = {0};
        if (st->fog_range) {
            range[0] = (GLfloat)(1.0 / sScaleX);
            range[1] = -st->fog_center - (GLfloat)sMX;
            range[2] = 1;
            for (i = 0; i < 10; i++) range[4 + i] = st->fog_k[i];
        }
        UNIFORM_IF_CHANGED(p_glUniform4fv, uFogRange, 4, range, lrange);
    }
    {
        static GLint lind[4], lcmd[16], lstage[16], lmtx[24], lsize[16];
        GLint ind[4] = {0}, cmd[16] = {0}, istage[16] = {0}, mtx[24] = {0}, size[16] = {0};
        if (st->ind_any) {
            int k;
            ind[0] = st->ind_stages;
            ind[1] = 1;
            for (i = 0; i < st->num_stages; i++) cmd[i] = (GLint)st->ind_cmd[i];
            for (i = 0; i < 4; i++) {
                istage[4 * i] = st->ind_map[i];
                istage[4 * i + 1] = st->ind_coord[i];
                istage[4 * i + 2] = st->ind_scale[i][0];
                istage[4 * i + 3] = st->ind_scale[i][1];
            }
            for (i = 0; i < 3; i++) {
                for (k = 0; k < 3; k++) {
                    mtx[8 * i + k] = st->ind_mtx[i][0][k];
                    mtx[8 * i + 4 + k] = st->ind_mtx[i][1][k];
                }
                mtx[8 * i + 3] = mtx[8 * i + 7] = 17 - st->ind_mtx_scale[i];
            }
            for (i = 0; i < 8; i++) {
                size[2 * i] = st->tc_size[i][0];
                size[2 * i + 1] = st->tc_size[i][1];
            }
        }
        UNIFORM_IF_CHANGED(p_glUniform4iv, uInd, 1, ind, lind);
        if (st->ind_any) {
            UNIFORM_IF_CHANGED(p_glUniform4iv, uIndCmd, 4, cmd, lcmd);
            UNIFORM_IF_CHANGED(p_glUniform4iv, uIndStage, 4, istage, lstage);
            UNIFORM_IF_CHANGED(p_glUniform4iv, uIndMtx, 6, mtx, lmtx);
            UNIFORM_IF_CHANGED(p_glUniform4iv, uTcSize, 4, size, lsize);
        }
    }
    sUniformsSet = 1;

    for (i = 0; i < 8; i++) {
        GLuint t = st->texture[i] ? st->texture[i] : sWhite;
        if (all || sGl.tex[i] != t) {
            p_glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, t);
            sGl.tex[i] = t;
        }
    }
    p_glActiveTexture(GL_TEXTURE0);

    {
        int blend = st->blend_enable;
        GLenum src = blend_factor(kBlendSrc[st->blend_src], st->efb_alpha);
        GLenum dst = blend_factor(kBlendDst[st->blend_dst], st->efb_alpha);
        GLenum eq = st->blend_subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        if (st->blend_subtract) src = dst = GL_ONE;
        if (all || sGl.blend != blend) {
            if (blend) glEnable(GL_BLEND);
            else glDisable(GL_BLEND);
            sGl.blend = blend;
        }
        if (blend && (all || sGl.blend_src != src || sGl.blend_dst != dst)) {
            glBlendFunc(src, dst);
            sGl.blend_src = src;
            sGl.blend_dst = dst;
        }
        if (blend && (all || sGl.blend_eq != eq)) {
            p_glBlendEquation(eq);
            sGl.blend_eq = eq;
        }
        if (!blend && all) sGl.blend_src = sGl.blend_dst = sGl.blend_eq = 0; /* set again when blending is on */
    }
    {
        GLboolean c = (GLboolean)st->color_update, a = (GLboolean)(st->alpha_update && st->efb_alpha);
        if (all || sGl.color_mask[0] != c || sGl.color_mask[3] != a) {
            glColorMask(c, c, c, a);
            sGl.color_mask[0] = sGl.color_mask[1] = sGl.color_mask[2] = c;
            sGl.color_mask[3] = a;
        }
    }
    {
        int depth = st->z_enable;
        GLboolean mask = depth && st->z_update ? GL_TRUE : GL_FALSE;
        if (all || sGl.depth != depth) {
            if (depth) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
            sGl.depth = depth;
        }
        if (depth && (all || sGl.depth_func != kDepthFunc[st->z_func])) {
            glDepthFunc(kDepthFunc[st->z_func]);
            sGl.depth_func = kDepthFunc[st->z_func];
        }
        if (!depth && all) sGl.depth_func = 0;
        if (all || sGl.depth_mask != mask) {
            glDepthMask(mask);
            sGl.depth_mask = mask;
        }
    }
    {
        /* Edges are scaled and rounded, rather than the size, so
         * neighbouring rectangles still meet at a fractional scale. */
        GLint vp[4], sc[4];
        double dr[2];
        /* On the canvas, with its clip space scaled (u_canvas). */
        static GLfloat lk[4];
        float v[4], k[4] = {1, 1, 0, 0};
        int s[4];
        gx_canvas_draw(st, sMX, sMY, v, s, k);
        if (all || memcmp(k, lk, sizeof(k)) != 0) {
            p_glUniform4fv(uCanvas, 1, k);
            memcpy(lk, k, sizeof(k));
        }
        vp[0] = GLX(v[0]);
        vp[1] = GLY(CANVAS_H - (v[1] + v[3]));
        vp[2] = GLX(v[0] + v[2]) - GLX(v[0]);
        vp[3] = GLY(CANVAS_H - v[1]) - GLY(CANVAS_H - (v[1] + v[3]));
        dr[0] = 1.0 - (st->viewport[5] > 1 ? 1 : st->viewport[5]);
        dr[1] = 1.0 - (st->viewport[4] < 0 ? 0 : st->viewport[4]);
        sc[0] = GLX(s[0]);
        sc[1] = GLY(CANVAS_H - s[3]);
        sc[2] = GLX(s[2]) - GLX(s[0]);
        sc[3] = GLY(CANVAS_H - s[1]) - GLY(CANVAS_H - s[3]);
        if (all || memcmp(vp, sGl.viewport, sizeof(vp)) != 0) {
            glViewport(vp[0], vp[1], vp[2], vp[3]);
            memcpy(sGl.viewport, vp, sizeof(vp));
        }
        if (all || memcmp(dr, sGl.depth_range, sizeof(dr)) != 0) {
            glDepthRange(dr[0], dr[1]);
            memcpy(sGl.depth_range, dr, sizeof(dr));
        }
        if (all) glEnable(GL_SCISSOR_TEST);
        if (all || memcmp(sc, sGl.scissor, sizeof(sc)) != 0) {
            glScissor(sc[0], sc[1], sc[2], sc[3]);
            memcpy(sGl.scissor, sc, sizeof(sc));
        }
    }
    /* Face culling (GX_CULL_NONE, FRONT, BACK, ALL). GX's front faces wind
     * counter-clockwise in GL's window coordinates, GL's default. Games
     * rely on it: the picture confirmation dialog hides a placeholder pane
     * by turning it away from the camera. */
    {
        static const GLenum kCull[4] = {GL_BACK, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
        int cull = st->cull_mode != 0;
        if (all || sGl.cull != cull) {
            if (cull) glEnable(GL_CULL_FACE);
            else glDisable(GL_CULL_FACE);
            sGl.cull = cull;
        }
        if (cull && (all || sGl.cull_face != kCull[st->cull_mode])) {
            glCullFace(kCull[st->cull_mode]);
            sGl.cull_face = kCull[st->cull_mode];
        }
        if (!cull && all) sGl.cull_face = 0;
    }
    sGl.valid = 1;
}

/* The XF uniform buffer holds what the decoder's draws last left in it,
 * unless a recorded frame was drawn since: then its next draw sends all of
 * it (sXfStale), and the recording's draws go by their own copy of what
 * it holds (sXfShadow). */
static int sXfStale;
static uint32_t sXfShadow[GX_XF_WORDS];
static int sXfShadowValid;

static void gl_draw(const GXDrawState* st, const GXVertex* v, int first_vertex, int vertices, const uint32_t* index,
              int first_index, int count) {
    apply_draw_state(st);
    if (st->xf && sXfStale) {
        p_glBufferSubData(GL_UNIFORM_BUFFER, 0, GX_XF_WORDS * 4, st->xf);
        sXfStale = 0;
    } else if (st->xf && st->xf_dirty_lo < st->xf_dirty_hi) {
        /* Only the XF words written since the last draw. */
        p_glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)st->xf_dirty_lo * 4,
                          (GLsizeiptr)(st->xf_dirty_hi - st->xf_dirty_lo) * 4, st->xf + st->xf_dirty_lo);
    }
    sXfShadowValid = 0;
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertices * sizeof(GXVertex), v + first_vertex, GL_STREAM_DRAW);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * sizeof(uint32_t), index + first_index, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, NULL);
}

/* The frame's vertices and indices go to buffers made again each frame at
 * a size that only grows. Made at the frame's own size, a little
 * different every frame (24 MB of vertices in the park), the driver kept
 * the old ones' memory: RAM and GPU memory grew by 24 MB at a time, to
 * gigabytes. */
static GLsizeiptr sFrameVbSize, sFrameIbSize;

static void frame_buffer(GLenum target, GLsizeiptr* size, GLsizeiptr bytes, const void* data) {
    if (bytes > *size) *size = (bytes + bytes / 4 + 0xFFFFF) & ~(GLsizeiptr)0xFFFFF;
    p_glBufferData(target, *size, NULL, GL_STREAM_DRAW);
    p_glBufferSubData(target, 0, bytes, data);
}

static void gl_frame_data(const GXVertex* v, int vertices, const uint32_t* index, int count) {
    p_glBindVertexArray(sVao);
    p_glBindBuffer(GL_ARRAY_BUFFER, sVbo);
    frame_buffer(GL_ARRAY_BUFFER, &sFrameVbSize, (GLsizeiptr)vertices * sizeof(GXVertex), v);
    frame_buffer(GL_ELEMENT_ARRAY_BUFFER, &sFrameIbSize, (GLsizeiptr)count * sizeof(uint32_t), index);
}

/* Sends the words of xf that differ from what the buffer holds. */
static void upload_xf(const uint32_t* xf) {
    int lo = 0, hi = GX_XF_WORDS;
    if (sXfShadowValid) {
        while (lo < GX_XF_WORDS && xf[lo] == sXfShadow[lo]) lo++;
        if (lo == GX_XF_WORDS) return;
        while (hi > lo && xf[hi - 1] == sXfShadow[hi - 1]) hi--;
    }
    p_glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)lo * 4, (GLsizeiptr)(hi - lo) * 4, xf + lo);
    memcpy(sXfShadow + lo, xf + lo, (size_t)(hi - lo) * 4);
    sXfShadowValid = 1;
    sXfStale = 1;
}

static void gl_draw_recorded(const GXDrawState* st, const uint32_t* xf, int first_vertex, int first_index, int count) {
    apply_draw_state(st);
    upload_xf(xf);
    p_glDrawElementsBaseVertex(GL_TRIANGLES, count, GL_UNSIGNED_INT, (void*)((size_t)first_index * 4), first_vertex);
}

static void gl_clear(int color, int alpha, int depth, uint32_t rgba, uint32_t z) {
    GLbitfield mask = 0;
    gl_forget();
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    sMsDirty = 1;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(color, color, color, alpha);
    glDepthMask(depth ? GL_TRUE : GL_FALSE);
    glClearColor(((rgba >> 24) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                 (rgba & 0xFF) / 255.0f);
    glClearDepth(1.0 - z / 16777215.0);
    if (color || alpha) mask |= GL_COLOR_BUFFER_BIT;
    if (depth) mask |= GL_DEPTH_BUFFER_BIT;
    if (mask) glClear(mask);
}

/* EFB copies to textures. GX textures have row 0 at the top while the EFB
 * framebuffer has it at the bottom, so the copy flips. */
typedef struct CopyTarget {
    uint32_t addr;
    int w, h;
    GLuint tex, fbo;
} CopyTarget;
static CopyTarget sTargets[64];
static int sTargetCount;

/* A copy target's texture size: a copy of the whole picture has the
 * canvas's margins. */
static void target_size(const CopyTarget* t, int* w, int* h) {
    int x = 0, y = 0;
    *w = t->w;
    *h = t->h;
    gx_canvas_rect(sMX, sMY, &x, &y, w, h);
    *w = GLX(*w);
    *h = GLY(*h);
}

static int gl_copy_target(uint32_t addr, int w, int h) {
    int tw, th;
    CopyTarget* t;
    int i;
    gl_forget();
    for (i = 0; i < sTargetCount; i++) {
        if (sTargets[i].addr == addr && sTargets[i].w == w && sTargets[i].h == h) return i;
    }
    if (sTargetCount == 64) return -1;
    t = &sTargets[sTargetCount];
    t->addr = addr;
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    target_size(t, &tw, &th);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glGenFramebuffers(1, &t->fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    return sTargetCount++;
}

static uint32_t gl_copy_texture(int target) { return sTargets[target].tex; }

/* The copy's texture read back (gx_frames.c's write backs). */
static uint8_t* gl_copy_read(int target, int* w, int* h) {
    CopyTarget* t = &sTargets[target];
    uint8_t* out;
    gl_forget();
    target_size(t, w, h);
    out = (uint8_t*)malloc((size_t)*w * *h * 4);
    if (!out) return NULL;
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return out;
}

/* Draws the EFB rectangle into the copy's texture. */
static void gl_copy(int target, int x, int y, int w, int h, int fmt, int intensity, int efb_alpha) {
    CopyTarget* t = &sTargets[target];
    GLint mode[4];
    GLfloat src[4];
    int tw, th;
    gl_forget();
    resolve();
    target_size(t, &tw, &th);
    gx_canvas_rect(sMX, sMY, &x, &y, &w, &h);
    /* Row 0 of the texture is the top row of the rectangle. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, tw, th);
    p_glUseProgram(sCopyProgram);
    p_glBindVertexArray(sCopyVao);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sEfbColor);
    src[0] = (float)x / CANVAS_W;
    src[1] = (float)(CANVAS_H - y) / CANVAS_H;
    src[2] = (float)(x + w) / CANVAS_W;
    src[3] = (float)(CANVAS_H - y - h) / CANVAS_H;
    p_glUniform4fv(uCopySrc, 1, src);
    mode[0] = fmt;
    mode[1] = intensity;
    mode[2] = efb_alpha;
    mode[3] = 0;
    p_glUniform4iv(uCopyMode, 1, mode);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glUseProgram(sProgram);
    p_glBindVertexArray(sVao);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
}

static void alloc_efb(void) {
    int w = GLX(CANVAS_W), h = GLY(CANVAS_H), i;
    gl_forget();
    glBindTexture(GL_TEXTURE_2D, sEfbColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glBindRenderbuffer(GL_RENDERBUFFER, sEfbDepth);
    /* Depth is stored reversed, 1 - GX depth, in a float buffer: GX
     * scenes put their depths within a few millionths of 1, where a 24-bit
     * buffer fed by float math can't tell surfaces one GX step apart (the
     * picture in the picture confirmation dialog lost to its frame). */
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, sMsaa > 1 ? 1 : w, sMsaa > 1 ? 1 : h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, sEfbFbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEfbColor, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sMsaa > 1 ? 0 : sEfbDepth);
    if (sMsFbo) {
        p_glDeleteFramebuffers(1, &sMsFbo);
        p_glDeleteRenderbuffers(1, &sMsColor);
        p_glDeleteRenderbuffers(1, &sMsDepth);
        sMsFbo = sMsColor = sMsDepth = 0;
    }
    sDrawFbo = sEfbFbo;
    if (sMsaa > 1) {
        p_glGenRenderbuffers(1, &sMsColor);
        p_glBindRenderbuffer(GL_RENDERBUFFER, sMsColor);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, sMsaa, GL_RGBA8, w, h);
        p_glGenRenderbuffers(1, &sMsDepth);
        p_glBindRenderbuffer(GL_RENDERBUFFER, sMsDepth);
        p_glRenderbufferStorageMultisample(GL_RENDERBUFFER, sMsaa, GL_DEPTH_COMPONENT32F, w, h);
        p_glGenFramebuffers(1, &sMsFbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, sMsFbo);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, sMsColor);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sMsDepth);
        sDrawFbo = sMsFbo;
    }
    for (i = 0; i < sTargetCount; i++) {
        int tw, th;
        target_size(&sTargets[i], &tw, &th);
        glBindTexture(GL_TEXTURE_2D, sTargets[i].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClearDepth(0.0); /* the far plane, reversed */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    sMsDirty = 0;
}

static void dump_bmp(const char* path, int w, int h) {
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    FILE* f = fopen(path, "wb");
    uint8_t hdr[54] = {'B', 'M'};
    int i;
    if (!f) {
        free(px);
        return;
    }
    glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    *(uint32_t*)(hdr + 2) = 54 + w * h * 4;
    hdr[10] = 54;
    hdr[14] = 40;
    *(int32_t*)(hdr + 18) = w;
    *(int32_t*)(hdr + 22) = h;
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    for (i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
    free(px);
}

static int gl_present(int x, int y, int cw, int ch, int may_drop) {
    int w, h, dw, dh, dropped;
    double aspect;
    gl_forget();
    win_size(&w, &h);
    /* The copied rectangle of the EFB, as large as fits in the window at
     * the shape it has on a TV (an EFB pixel is square in 4:3, wider in
     * 16:9; on the canvas), with black bars on the sides left over. */
    gx_canvas_rect(sMX, sMY, &x, &y, &cw, &ch);
    aspect = ch > 0 ? cw * sPixelAspect / ch : 1;
    dw = w;
    dh = (int)(w / aspect + 0.5);
    if (dh > h) {
        dh = h;
        dw = (int)(h * aspect + 0.5);
    }
    if (sChain && w > 0 && h > 0) dx_size(w, h);
    resolve();
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, sEfbFbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sWinFbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(GLX(x), GLY(CANVAS_H - y - ch), GLX(x + cw), GLY(CANVAS_H - y), (w - dw) / 2, (h - dh) / 2,
                        (w - dw) / 2 + dw, (h - dh) / 2 + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    {
        const uint32_t* menu = menu_draw(w, h);
        if (menu) {
            p_glBindFramebuffer(GL_FRAMEBUFFER, sWinFbo);
            p_glUseProgram(sOverlayProgram);
            p_glBindVertexArray(sCopyVao);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, sOverlayTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, menu);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            p_glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_BLEND);
            p_glUseProgram(sProgram);
            p_glBindVertexArray(sVao);
        }
    }
    if (sDumpDir && sPresented >= sDumpFrom && sPresented % (unsigned)sDumpEvery == 0 && !sChain) {
        char path[512];
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        snprintf(path, sizeof(path), "%s/frame_%05u.bmp", sDumpDir, sPresented);
        dump_bmp(path, w, h);
    }
    dropped = window_present(w, h, may_drop);
    if (sDumpDir && sPresented >= sDumpFrom && sPresented % (unsigned)sDumpEvery == 0 && sChain) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05u.bmp", sDumpDir, sPresented);
        dump_shared(path);
    }
    sPresented++;
    p_glBindFramebuffer(GL_FRAMEBUFFER, sDrawFbo);
    return dropped;
}

const GXDevice g_gl_device = {
    "OpenGL",       gl_init,       gl_texture,     gl_texture_update, gl_texture_delete, gl_draw,
    gl_frame_data,  gl_draw_recorded, gl_clear,    gl_copy_target,    gl_copy_texture,   gl_copy,
    gl_present,     gl_apply_settings, gl_scale,   gl_msaa,          NULL,              NULL,
    NULL,           NULL,          gl_copy_read,
};
