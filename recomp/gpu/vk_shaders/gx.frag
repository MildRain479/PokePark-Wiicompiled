#version 450
// TEV, the GPU's color combiner, for vk_backend.c: as gl_backend.c's
// fragment shader, which says more. The TEV state is in the frame's data
// buffer at word pc.tev, read as a uniform buffer from there on (a phone's
// GPU keeps one in its constant registers): header (stages, alpha
// compare functions and logic), alpha references, registers, konst colors,
// swap tables, fog (kind, orthographic, b's magnitude and shift, a, c and
// the color as RGB8), indirect texturing (from word 64: each stage's
// command; at 80 the indirect stages' maps, coordinates and scales; at 96
// the matrices' rows, each with its shift; at 120 the indirect stages and
// whether any stage is indirect; at 128 the coordinates' sizes), fog range
// adjustment (at 144: the pixel's offset from the center as x a + b, on,
// and from 148 the ten cosines), then from word 160 per stage its color and alpha inputs and operations, texture
// map and selections, 24 words each. Textures are pc.tex[map] of all the
// device's textures.

layout(std140, set = 1, binding = 2) uniform Tev { uvec4 tu[(160 + 24 * 16) / 4]; };
layout(set = 0, binding = 0) uniform sampler2D textures[16384];
layout(push_constant) uniform Draw { uint xf, tev; uint tex[8]; vec2 canvas; } pc;

// TEX_IO, of the build: the texture coordinates gx.vert makes (as many as
// the TEV stages read, in a specialized pipeline).
#ifndef TEX_IO
#define TEX_IO 8
#endif

layout(location = 0) in vec4 v_col0;
layout(location = 1) in vec4 v_col1;
#if TEX_IO > 0
layout(location = 2) in vec3 v_tex[TEX_IO];
#endif
layout(location = 0) out vec4 o_color;

ivec4 ti(uint w) { return ivec4(tu[w >> 2]); }
vec4 tf(uint w) { return intBitsToFloat(ti(w)); }

// No arrays indexed per pixel (registers, inputs, konst colors, swap
// tables): a phone's GPU keeps those in memory, not registers, and they
// cost more than all the rest. The selections are the same for the whole
// draw, so the switches branch alike for every pixel.
vec4 r0, r1, r2, r3;

// Off when the alpha compare always passes (vk_backend.c's pipeline key):
// without a discard, the GPU can test depth before shading.
layout(constant_id = 0) const bool ALPHA_TEST = true;

// A pipeline made for one TEV setup (vk_backend.c's spec_words) has it
// here, and the compiler leaves out all it doesn't use: SPEC_STAGES
// stages (-1: none, read from the data buffer as above), SPEC_SWAP the swap
// tables (2 bits each), and per stage three words, as unpacked in main.
layout(constant_id = 1) const int SPEC_STAGES = -1;
layout(constant_id = 2) const uint SPEC_SWAP = 0u;
layout(constant_id = 3) const uint S0A = 0u;
layout(constant_id = 4) const uint S0B = 0u;
layout(constant_id = 5) const uint S0C = 0u;
layout(constant_id = 6) const uint S1A = 0u;
layout(constant_id = 7) const uint S1B = 0u;
layout(constant_id = 8) const uint S1C = 0u;
layout(constant_id = 9) const uint S2A = 0u;
layout(constant_id = 10) const uint S2B = 0u;
layout(constant_id = 11) const uint S2C = 0u;
layout(constant_id = 12) const uint S3A = 0u;
layout(constant_id = 13) const uint S3B = 0u;
layout(constant_id = 14) const uint S3C = 0u;
layout(constant_id = 15) const uint S4A = 0u;
layout(constant_id = 16) const uint S4B = 0u;
layout(constant_id = 17) const uint S4C = 0u;
layout(constant_id = 18) const uint S5A = 0u;
layout(constant_id = 19) const uint S5B = 0u;
layout(constant_id = 20) const uint S5C = 0u;
layout(constant_id = 21) const uint S6A = 0u;
layout(constant_id = 22) const uint S6B = 0u;
layout(constant_id = 23) const uint S6C = 0u;
layout(constant_id = 24) const uint S7A = 0u;
layout(constant_id = 25) const uint S7B = 0u;
layout(constant_id = 26) const uint S7C = 0u;
layout(constant_id = 27) const uint S8A = 0u;
layout(constant_id = 28) const uint S8B = 0u;
layout(constant_id = 29) const uint S8C = 0u;
layout(constant_id = 30) const uint S9A = 0u;
layout(constant_id = 31) const uint S9B = 0u;
layout(constant_id = 32) const uint S9C = 0u;
layout(constant_id = 33) const uint S10A = 0u;
layout(constant_id = 34) const uint S10B = 0u;
layout(constant_id = 35) const uint S10C = 0u;
layout(constant_id = 36) const uint S11A = 0u;
layout(constant_id = 37) const uint S11B = 0u;
layout(constant_id = 38) const uint S11C = 0u;
layout(constant_id = 39) const uint S12A = 0u;
layout(constant_id = 40) const uint S12B = 0u;
layout(constant_id = 41) const uint S12C = 0u;
layout(constant_id = 42) const uint S13A = 0u;
layout(constant_id = 43) const uint S13B = 0u;
layout(constant_id = 44) const uint S13C = 0u;
layout(constant_id = 45) const uint S14A = 0u;
layout(constant_id = 46) const uint S14B = 0u;
layout(constant_id = 47) const uint S14C = 0u;
layout(constant_id = 48) const uint S15A = 0u;
layout(constant_id = 49) const uint S15B = 0u;
layout(constant_id = 50) const uint S15C = 0u;

vec4 sample_map(int m, vec2 uv) { return texture(textures[pc.tex[m]], uv); }
vec4 konst(int k) { return tf(24 + 4 * k); }
vec4 swizzle(vec4 c, int t) {
    ivec4 s;
    if (SPEC_STAGES >= 0) {
        uint w = SPEC_SWAP >> uint(8 * t);
        s = ivec4(w & 3u, (w >> 2) & 3u, (w >> 4) & 3u, (w >> 6) & 3u);
    } else s = ti(40 + 4 * t);
    return vec4(c[s.x], c[s.y], c[s.z], c[s.w]);
}
float kfrac(int s) { return s < 8 ? float(8 - s) / 8.0 : 0.0; }
vec3 kcolor(int s) {
    if (s < 8) return vec3(kfrac(s));
    if (s >= 12 && s < 16) return konst(s - 12).rgb;
    if (s >= 16) {
        int k = (s - 16) & 3, c = (s - 16) >> 2;
        return vec3(konst(k)[c]);
    }
    return vec3(0.0);
}
float kalpha(int s) {
    if (s < 8) return kfrac(s);
    if (s >= 16) {
        int k = (s - 16) & 3, c = (s - 16) >> 2;
        return konst(k)[c];
    }
    return 0.0;
}
vec3 cin(int i, vec4 tex, vec4 ras, vec3 kc) {
    switch (i) {
    case 0: return r0.rgb;
    case 1: return vec3(r0.a);
    case 2: return r1.rgb;
    case 3: return vec3(r1.a);
    case 4: return r2.rgb;
    case 5: return vec3(r2.a);
    case 6: return r3.rgb;
    case 7: return vec3(r3.a);
    case 8: return tex.rgb;
    case 9: return vec3(tex.a);
    case 10: return ras.rgb;
    case 11: return vec3(ras.a);
    case 12: return vec3(1.0);
    case 13: return vec3(0.5);
    case 14: return kc;
    }
    return vec3(0.0);
}
float ain(int i, vec4 tex, vec4 ras, float ka) {
    switch (i) {
    case 0: return r0.a;
    case 1: return r1.a;
    case 2: return r2.a;
    case 3: return r3.a;
    case 4: return tex.a;
    case 5: return ras.a;
    case 6: return ka;
    }
    return 0.0;
}
bool acmp(int f, float a, float r) {
    if (f == 0) return false;
    if (f == 1) return a < r;
    if (f == 2) return a == r;
    if (f == 3) return a <= r;
    if (f == 4) return a > r;
    if (f == 5) return a != r;
    if (f == 6) return a >= r;
    return true;
}
float q8(float x) { return floor(x * 255.0 + 0.5) / 255.0; }
vec3 q8(vec3 x) { return floor(x * 255.0 + 0.5) / 255.0; }

// A texture map sampled at a coordinate in texels with 7 fractional bits.
vec4 sample_at(int m, ivec2 c) {
    return sample_map(m, vec2(c) / (vec2(textureSize(textures[pc.tex[m]], 0)) * 128.0));
}

// Texture coordinate n in texels with 7 fractional bits, by its size (an
// indirect stage's or stage's, as GX has them).
ivec2 fixpoint_uv(int n) {
#if TEX_IO > 0
    vec3 tc = v_tex[TEX_IO < 8 ? min(n, TEX_IO - 1) : n];
    ivec4 sz = ti(128u + 4u * uint(n >> 1));
    return ivec2(tc.xy / (tc.z == 0.0 ? 1.0 : tc.z) * vec2((n & 1) != 0 ? sz.zw : sz.xy) * 128.0);
#else
    return ivec2(0);
#endif
}

int wrap(int v, int w) { return w == 0 ? v : (w == 6 ? 0 : v & ((256 >> (w - 1)) * 128 - 1)); }

// ind: the coordinate indirect texturing made (if z isn't 0), and the bump
// alpha (w).
vec4 stage(ivec4 ci, ivec4 ai, ivec4 co, ivec4 ao, ivec4 map, ivec4 sel, ivec4 ind) {
    vec4 tex = vec4(1.0);
#if TEX_IO > 0
    if (map.z != 0) {
        if (ind.z != 0) tex = sample_at(map.x, ind.xy);
        else {
            // (Fewer than all: only stages read none beyond, but the compiler
            // sees the index in stages left out too.)
            vec3 tc = v_tex[TEX_IO < 8 ? min(map.y, TEX_IO - 1) : map.y];
            tex = sample_map(map.x, tc.xy / (tc.z == 0.0 ? 1.0 : tc.z));
        }
    }
#endif
    tex = swizzle(tex, sel.w & 3);
    vec4 ras = map.w == 0 ? v_col0 : (map.w == 1 ? v_col1 : vec4(0.0));
    if (map.w == 5) ras = vec4(0.0, 0.0, 0.0, float(ind.w) / 255.0);
    else if (map.w == 6) ras = vec4(0.0, 0.0, 0.0, float(ind.w | (ind.w >> 5)) / 255.0);
    ras = swizzle(ras, sel.w >> 2);
    vec3 kc = kcolor(sel.z & 31);
    float ka = kalpha(sel.z >> 5);
    vec3 a = clamp(cin(ci.x, tex, ras, kc), 0.0, 1.0), b = clamp(cin(ci.y, tex, ras, kc), 0.0, 1.0);
    vec3 c = clamp(cin(ci.z, tex, ras, kc), 0.0, 1.0), d = cin(ci.w, tex, ras, kc);
    vec3 rc;
    if (co.y == 3) {
        vec3 r;
        if (co.z == 3) r = vec3(co.x == 0 ? greaterThan(q8(a), q8(b)) : equal(q8(a), q8(b)));
        else {
            float av, bv;
            if (co.z == 0) { av = q8(a.r); bv = q8(b.r); }
            else if (co.z == 1) { av = q8(a.r) + q8(a.g) * 256.0; bv = q8(b.r) + q8(b.g) * 256.0; }
            else { av = q8(a.r) + q8(a.g) * 256.0 + q8(a.b) * 65536.0; bv = q8(b.r) + q8(b.g) * 256.0 + q8(b.b) * 65536.0; }
            r = vec3(co.x == 0 ? float(av > bv) : float(av == bv));
        }
        rc = d + r * c;
    } else {
        vec3 l = mix(a, b, c);
        rc = co.x == 0 ? d + l : d - l;
        rc += co.y == 1 ? 0.5 : (co.y == 2 ? -0.5 : 0.0);
        rc *= co.z == 1 ? 2.0 : (co.z == 2 ? 4.0 : (co.z == 3 ? 0.5 : 1.0));
    }
    rc = co.w != 0 ? clamp(rc, 0.0, 1.0) : clamp(rc, -1024.0 / 255.0, 1023.0 / 255.0);
    float aa = clamp(ain(ai.x, tex, ras, ka), 0.0, 1.0), ab = clamp(ain(ai.y, tex, ras, ka), 0.0, 1.0);
    float ac = clamp(ain(ai.z, tex, ras, ka), 0.0, 1.0), ad = ain(ai.w, tex, ras, ka);
    float ra;
    if (ao.y == 3) {
        float r = ao.x == 0 ? float(q8(aa) > q8(ab)) : float(q8(aa) == q8(ab));
        ra = ad + r * ac;
    } else {
        float l = mix(aa, ab, ac);
        ra = ao.x == 0 ? ad + l : ad - l;
        ra += ao.y == 1 ? 0.5 : (ao.y == 2 ? -0.5 : 0.0);
        ra *= ao.z == 1 ? 2.0 : (ao.z == 2 ? 4.0 : (ao.z == 3 ? 0.5 : 1.0));
    }
    ra = ao.w != 0 ? clamp(ra, 0.0, 1.0) : clamp(ra, -1024.0 / 255.0, 1023.0 / 255.0);
    if (sel.x == 0) r0.rgb = rc;
    else if (sel.x == 1) r1.rgb = rc;
    else if (sel.x == 2) r2.rgb = rc;
    else r3.rgb = rc;
    if (sel.y == 0) r0.a = ra;
    else if (sel.y == 1) r1.a = ra;
    else if (sel.y == 2) r2.a = ra;
    else r3.a = ra;
    return vec4(rc, ra);
}

// A stage's three words in a specialized pipeline.
vec4 run(uint a, uint b, uint c) {
    ivec4 ci = ivec4(a & 15u, (a >> 4) & 15u, (a >> 8) & 15u, (a >> 12) & 15u);
    ivec4 ai = ivec4((a >> 16) & 7u, (a >> 19) & 7u, (a >> 22) & 7u, (a >> 25) & 7u);
    ivec4 co = ivec4(b & 15u, (b >> 4) & 15u, (b >> 8) & 15u, (b >> 12) & 15u);
    ivec4 ao = ivec4((b >> 16) & 15u, (b >> 20) & 15u, (b >> 24) & 15u, (b >> 28) & 15u);
    ivec4 map = ivec4(c & 15u, (c >> 4) & 15u, (c >> 8) & 1u, (c >> 9) & 15u);
    ivec4 sel = ivec4((c >> 13) & 3u, (c >> 15) & 3u, (c >> 17) & 1023u, (c >> 27) & 15u);
    return stage(ci, ai, co, ao, map, sel, ivec4(0));
}

// Indirect texturing, as GX does it in integers: per stage, its indirect
// stage's texel (masked, biased, by a matrix or the coordinate itself)
// offsets its coordinate, wrapped and maybe added to the last stage's.
ivec3 ind_tex[4];
ivec2 ind_coord;
ivec4 indirect(int i, int cmd, ivec4 map) {
    int bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;
    ivec2 uv = fixpoint_uv(map.y), trans = ivec2(0);
    int bump = 0;
    if (bt < ti(120).x && (bs != 0 || mid != 0)) {
        ivec3 crd = ind_tex[bt];
        if (bs != 0) bump = crd[bs - 1] & (fmt == 1 ? 0xE0 : (fmt == 2 ? 0xF0 : 0xF8));
        crd &= ivec3(fmt == 0 ? 255 : (fmt == 1 ? 31 : (fmt == 2 ? 15 : 7)));
        int add = fmt == 0 ? -128 : 1;
        crd += ivec3((bias & 1) != 0 ? add : 0, (bias & 2) != 0 ? add : 0, (bias & 4) != 0 ? add : 0);
        int m = (mid & 3) - 1;
        if (m >= 0) {
            ivec4 a = ti(96u + 8u * uint(m)), b = ti(100u + 8u * uint(m));
            if ((mid >> 2) == 0) trans = ivec2(a.x * crd.x + a.y * crd.y + a.z * crd.z, b.x * crd.x + b.y * crd.y + b.z * crd.z) >> 3;
            else if ((mid >> 2) == 1) trans = (uv * crd.x) >> 8;
            else trans = (uv * crd.y) >> 8;
            trans = a.w >= 0 ? trans >> a.w : trans << -a.w;
        }
    }
    ivec2 c = ivec2(wrap(uv.x, (cmd >> 13) & 7), wrap(uv.y, (cmd >> 16) & 7)) + trans;
    ind_coord = ((cmd >> 20) & 1) != 0 ? ind_coord + c : c;
    ind_coord = (ind_coord << 8) >> 8;
    return ivec4(ind_coord, 1, bump);
}

void main() {
    ivec4 head = ti(0);
    r0 = tf(8);
    r1 = tf(12);
    r2 = tf(16);
    r3 = tf(20);
    vec4 last = r0;
    if (SPEC_STAGES >= 0) {
        if (SPEC_STAGES > 0) last = run(S0A, S0B, S0C);
        if (SPEC_STAGES > 1) last = run(S1A, S1B, S1C);
        if (SPEC_STAGES > 2) last = run(S2A, S2B, S2C);
        if (SPEC_STAGES > 3) last = run(S3A, S3B, S3C);
        if (SPEC_STAGES > 4) last = run(S4A, S4B, S4C);
        if (SPEC_STAGES > 5) last = run(S5A, S5B, S5C);
        if (SPEC_STAGES > 6) last = run(S6A, S6B, S6C);
        if (SPEC_STAGES > 7) last = run(S7A, S7B, S7C);
        if (SPEC_STAGES > 8) last = run(S8A, S8B, S8C);
        if (SPEC_STAGES > 9) last = run(S9A, S9B, S9C);
        if (SPEC_STAGES > 10) last = run(S10A, S10B, S10C);
        if (SPEC_STAGES > 11) last = run(S11A, S11B, S11C);
        if (SPEC_STAGES > 12) last = run(S12A, S12B, S12C);
        if (SPEC_STAGES > 13) last = run(S13A, S13B, S13C);
        if (SPEC_STAGES > 14) last = run(S14A, S14B, S14C);
        if (SPEC_STAGES > 15) last = run(S15A, S15B, S15C);
    } else {
        bool any = ti(120).y != 0;
        if (any) {
            ind_coord = ivec2(0);
            for (int k = 0; k < ti(120).x && k < 4; k++) {
                ivec4 is = ti(80u + 4u * uint(k));
                ind_tex[k] = ivec3(floor(sample_at(is.x, fixpoint_uv(is.y) >> is.zw).abg * 255.0 + 0.5));
            }
        }
        for (int i = 0; i < head.x; i++) {
            uint s = 160u + 24u * uint(i);
            ivec4 map = ti(s + 16);
            int cmd = any ? ti(64u + 4u * uint(i >> 2))[i & 3] : 0;
            last = stage(ti(s), ti(s + 4), ti(s + 8), ti(s + 12), map, ti(s + 20), cmd != 0 ? indirect(i, cmd, map) : ivec4(0));
        }
    }
    last = clamp(last, 0.0, 1.0);
    if (ALPHA_TEST) {
        vec4 alpha_ref = tf(4);
        float a8 = floor(last.a * 255.0 + 0.5);
        bool p0 = acmp(head.y, a8, alpha_ref.x), p1 = acmp(head.z, a8, alpha_ref.y);
        bool pass = head.w == 0 ? (p0 && p1) : (head.w == 1 ? (p0 || p1) : (head.w == 2 ? (p0 != p1) : (p0 == p1)));
        if (!pass) discard;
    }
    // Fog, as GX does it after the alpha test, from the pixel's 24-bit
    // depth (stored reversed: 1 is GX's 0).
    ivec4 fog = ti(56);
    if (fog.x != 0) {
        vec4 ff = tf(60);
        int z = int(clamp((1.0 - gl_FragCoord.z) * 16777216.0, 0.0, 16777215.0));
        float ze = fog.y == 0 ? ff.x * 16777216.0 / float(fog.z - (z >> fog.w)) : ff.x * float(z) / 16777216.0;
        vec4 fr = tf(144);
        if (floatBitsToInt(fr.z) != 0) {
            // Range adjustment: farther from the center, a longer way.
            float t = clamp(abs(gl_FragCoord.x * fr.x + fr.y) / 32.0 - 1.0, -1.0, 9.0);
            int lo = int(floor(t)), hi = min(lo + 1, 9);
            float kl = lo < 0 ? 1.0 : tf(148u + uint(lo & ~3))[lo & 3];
            float kh = tf(148u + uint(hi & ~3))[hi & 3];
            ze /= max(mix(kl, kh, t - float(lo)), 1.0 / 256.0);
        }
        float f = clamp(ze - ff.y, 0.0, 1.0);
        if (fog.x == 4) f = 1.0 - exp2(-8.0 * f);
        else if (fog.x == 5) f = 1.0 - exp2(-8.0 * f * f);
        else if (fog.x == 6) f = exp2(-8.0 * (1.0 - f));
        else if (fog.x == 7) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));
        uint fc = uint(ti(60).z);
        vec3 col = vec3(float(fc & 255u), float((fc >> 8) & 255u), float((fc >> 16) & 255u)) / 255.0;
        last.rgb = mix(last.rgb, col, floor(f * 256.0 + 0.5) / 256.0);
    }
    o_color = last;
}
