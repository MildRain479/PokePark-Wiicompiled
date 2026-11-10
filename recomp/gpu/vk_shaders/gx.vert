#version 450
// The GPU's transform unit (XF), per vertex, for vk_backend.c: as
// gl_backend.c's vertex shader, which says more. The XF state is a copy in
// the frame's data buffer, at word pc.xf, packed (xw): XF memory 0x000-0x0FF
// (position and texture matrices), 0x400-0x47F (normal matrices),
// 0x500-0x5FF (post-transform matrices), 0x600-0x67F (lights), then the XF
// registers 0x1000-0x105F.

// From word pc.xf of the data buffer on, which the descriptor's offset
// gives: a uniform buffer, read faster than a storage one.
layout(std140, set = 1, binding = 1) uniform XF { uvec4 xu[(0x360 + 8 + 8 * 12) / 4]; };
// canvas: what clip space's x and y are multiplied by (gx_canvas_draw).
layout(push_constant) uniform Draw { uint xf, tev; uint tex[8]; vec2 canvas; } pc;

layout(location = 0) in uvec4 a_mtx0;
layout(location = 1) in uvec4 a_mtx1;
layout(location = 2) in uvec4 a_mtx2;
layout(location = 3) in vec3 a_pos;
layout(location = 4) in vec3 a_nrm;
layout(location = 5) in vec4 a_col0;
layout(location = 6) in vec4 a_col1;
layout(location = 7) in vec4 a_tex01;
layout(location = 8) in vec4 a_tex23;
layout(location = 9) in vec4 a_tex45;
layout(location = 10) in vec4 a_tex67;

// A pipeline made for one transform setup (vk_backend.c's vs_words) has
// the four lighting channel controls here, VS_SPEC set, and the compiler
// leaves out the lights and lighting the setup has no use for; and VS_MTX,
// which matrix indices the vertices carry (bit 0 the position's, bit 1
// texture coordinates'): the others are the same for every vertex, and a
// phone's GPU reads matrices by those far faster. (The texture
// coordinates' setup isn't here: Adreno's compiler crashes on that.)
layout(constant_id = 64) const bool VS_SPEC = false;
layout(constant_id = 65) const uint VS_CC0 = 0u;
layout(constant_id = 66) const uint VS_CC1 = 0u;
layout(constant_id = 67) const uint VS_AC0 = 0u;
layout(constant_id = 68) const uint VS_AC1 = 0u;
layout(constant_id = 69) const uint VS_MTX = 3u;
// And whether the coordinates' setup is vk_backend.c's texgen_program's.
layout(constant_id = 70) const bool VS_TEXGEN = false;

// TEX_IO, of the build (tools/recomp/build.py makes one for each): the
// texture coordinates made, as many as the draw's TEV stages read
// (vk_backend.c's vs_words). Each costs every vertex, in every tile of the
// picture a phone's GPU draws it in, whether read or not.
#ifndef TEX_IO
#define TEX_IO 8
#endif

layout(location = 0) out vec4 v_col0;
layout(location = 1) out vec4 v_col1;
#if TEX_IO > 0
layout(location = 2) out vec3 v_tex[TEX_IO];
#endif

uint xw(int a) {
    int i = a >= 0x1000 ? a - 0x1000 + 0x300 : a >= 0x500 ? a - 0x380 : a >= 0x400 ? a - 0x300 : a;
    return xu[i >> 2][i & 3];
}
float xfl(int a) { return uintBitsToFloat(xw(a)); }
// The texture coordinates' setup after the registers (vk_backend.c's
// texgen_program): word k, and coordinate i's matrix row j.
uint tw(int k) {
    int i = 0x360 + k;
    return xu[i >> 2][i & 3];
}
vec4 trow(int i, int j) { return uintBitsToFloat(xu[(0x360 + 8) / 4 + 3 * i + j]); }
vec4 row(int a) { return vec4(xfl(a), xfl(a + 1), xfl(a + 2), xfl(a + 3)); }
vec3 vec3at(int a) { return vec3(xfl(a), xfl(a + 1), xfl(a + 2)); }
vec4 unpack(uint c) {
    return vec4(float((c >> 24) & 255u), float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0;
}

// A channel control: the pipeline's, or the draw's XF state's.
uint chan_ctrl(int i) {
    if (VS_SPEC) {
        if (i == 0) return VS_CC0;
        if (i == 1) return VS_CC1;
        if (i == 2) return VS_AC0;
        return VS_AC1;
    }
    return xw(0x100E + i);
}

vec4 light(int j, uint ctrl, vec3 pos, vec3 n) {
    int b = 0x600 + j * 16;
    vec3 cosatt = vec3at(b + 4), distatt = vec3at(b + 7), lpos = vec3at(b + 10), ldirv = vec3at(b + 13);
    uint dfn = (ctrl >> 7) & 3u, afn = (ctrl >> 9) & 3u;
    vec3 ldir;
    float attn;
    if (afn == 1u) { // specular
        ldir = normalize(lpos - pos);
        attn = dot(n, ldir) >= 0.0 ? max(0.0, dot(n, ldirv)) : 0.0;
        vec3 da = dfn == 0u ? distatt : normalize(distatt);
        attn = max(0.0, dot(cosatt, vec3(1.0, attn, attn * attn))) / dot(da, vec3(1.0, attn, attn * attn));
    } else if (afn == 3u) { // spot
        vec3 d = lpos - pos;
        float dist2 = dot(d, d), dist = sqrt(dist2);
        ldir = d / dist;
        attn = max(0.0, dot(ldir, ldirv));
        attn = max(0.0, cosatt.x + cosatt.y * attn + cosatt.z * attn * attn) / dot(distatt, vec3(1.0, dist, dist2));
    } else { // none, directional
        vec3 d = lpos - pos;
        ldir = length(d) == 0.0 ? n : normalize(d);
        attn = 1.0;
    }
    float diff = dfn == 0u ? 1.0 : (dfn == 1u ? dot(ldir, n) : max(0.0, dot(ldir, n)));
    return attn * diff * unpack(xw(b + 3));
}

vec4 lights(uint ctrl, vec3 pos, vec3 n) {
    uint mask = ((ctrl >> 2) & 15u) | (((ctrl >> 11) & 15u) << 4);
    vec4 sum = vec4(0.0);
    for (int j = 0; j < 8; j++)
        if ((mask & (1u << uint(j))) != 0u) sum += light(j, ctrl, pos, n);
    return sum;
}

vec4 channel(int i, vec4 vcol, vec3 pos, vec3 n) {
    uint cc = chan_ctrl(i), ac = chan_ctrl(2 + i);
    vec4 matr = unpack(xw(0x100C + i)), ambr = unpack(xw(0x100A + i)), r;
    vec3 mat = (cc & 1u) != 0u ? vcol.rgb : matr.rgb;
    if ((cc & 2u) != 0u) {
        vec3 amb = (cc & 64u) != 0u ? vcol.rgb : ambr.rgb;
        r.rgb = mat * clamp(amb + lights(cc, pos, n).rgb, 0.0, 1.0);
    } else r.rgb = mat;
    float mata = (ac & 1u) != 0u ? vcol.a : matr.a;
    if ((ac & 2u) != 0u) {
        float amba = (ac & 64u) != 0u ? vcol.a : ambr.a;
        r.a = mata * clamp(amba + lights(ac, pos, n).a, 0.0, 1.0);
    } else r.a = mata;
    return clamp(r, 0.0, 1.0);
}

// The matrix indices: the vertex's, or XF's MATINDEX registers'.
int texmtx(int i) {
    if (!VS_SPEC || (VS_MTX & 2u) != 0u) return int(i < 3 ? a_mtx0[i + 1] : i < 7 ? a_mtx1[i - 3] : a_mtx2.x);
    if (i < 4) return int((xw(0x1018) >> uint(6 + 6 * i)) & 63u);
    return int((xw(0x1019) >> uint(6 * (i - 4))) & 63u);
}
int posmtx() {
    if (!VS_SPEC || (VS_MTX & 1u) != 0u) return int(a_mtx0.x);
    return int(xw(0x1018) & 63u);
}
vec2 texin(int i) {
    vec4 p = i < 2 ? a_tex01 : i < 4 ? a_tex23 : i < 6 ? a_tex45 : a_tex67;
    return (i & 1) == 0 ? p.xy : p.zw;
}

void main() {
    int pn = posmtx();
    vec4 in4 = vec4(a_pos, 1.0);
    vec3 world = vec3(dot(row(pn * 4), in4), dot(row(pn * 4 + 4), in4), dot(row(pn * 4 + 8), in4));
    int nb = 0x400 + (pn & 31) * 3;
    vec3 n = vec3(dot(vec3at(nb), a_nrm), dot(vec3at(nb + 3), a_nrm), dot(vec3at(nb + 6), a_nrm));
    if (dot(n, n) > 0.0) n = normalize(n);
    float p0 = xfl(0x1020), p1 = xfl(0x1021), p2 = xfl(0x1022), p3 = xfl(0x1023), p4 = xfl(0x1024), p5 = xfl(0x1025);
    vec4 clip = xw(0x1026) == 0u
        ? vec4(p0 * world.x + p1 * world.z, p2 * world.y + p3 * world.z, p4 * world.z + p5, -world.z)
        : vec4(p0 * world.x + p1, p2 * world.y + p3, p4 * world.z + p5, 1.0);
    // Depth reversed, as in gl_backend.c: 0..w, far to near.
    gl_Position = vec4(clip.x * pc.canvas.x, clip.y * pc.canvas.y, -clip.z, clip.w);
    v_col0 = channel(0, a_col0, world, n);
    v_col1 = channel(1, a_col1, world, n);
#if TEX_IO > 0
    int ntex = int(xw(0x103F) & 15u);
    if (VS_SPEC && VS_TEXGEN) {
        // Each a matrix times a vector, as the setup says.
        for (int i = 0; i < TEX_IO; i++) {
            if (i >= ntex) {
                v_tex[i] = vec3(0.0, 0.0, 1.0);
                continue;
            }
            uint s = tw(i);
            vec4 src = vec4(0.0, 0.0, 1.0, 1.0);
            if (s == 0u) src = in4;
            else if (s == 1u) src.xyz = a_nrm;
            else if (s >= 4u) src.xy = texin(int(s) - 4);
            v_tex[i] = vec3(dot(trow(i, 0), src), dot(trow(i, 1), src), dot(trow(i, 2), src));
        }
    } else {
        bool dual = (xw(0x1012) & 1u) != 0u;
        for (int i = 0; i < TEX_IO; i++) {
            if (i >= ntex) {
                v_tex[i] = vec3(0.0, 0.0, 1.0);
                continue;
            }
            uint info = xw(0x1040 + i);
            int src_row = int((info >> 7) & 31u), type = int((info >> 4) & 7u);
            vec4 src = vec4(0.0, 0.0, 1.0, 1.0);
            if (src_row == 0) src = in4;
            else if (src_row == 1) src.xyz = a_nrm;
            else if (src_row >= 5 && src_row <= 12) src.xy = texin(src_row - 5);
            vec3 stq;
            if (type == 0) {
                int m = texmtx(i) * 4;
                stq = vec3(dot(row(m), src), dot(row(m + 4), src), ((info >> 1) & 1u) != 0u ? dot(row(m + 8), src) : 1.0);
            } else stq = vec3(src.xy, 1.0);
            if (dual) { // post-transform
                uint pt = xw(0x1050 + i);
                vec4 v = vec4(stq, 1.0);
                if (((pt >> 8) & 1u) != 0u && dot(v.xyz, v.xyz) > 0.0) v.xyz = normalize(v.xyz);
                int m = 0x500 + int(pt & 63u) * 4;
                stq = vec3(dot(row(m), v), dot(row(m + 4), v), dot(row(m + 8), v));
            }
            v_tex[i] = stq;
        }
    }
#endif
}
