#version 450
// SWR TAA (dlss.c, ini taa=1): this frame's scene blended with the previous result, which is moved by the motion vectors
// (render pixels, current to previous). Same maths as the OpenGL renderer's TAA_FS (gpu_gl.c).
//
// The history is clipped into this frame's 3x3 colour box (YCoCg on tone-mapped colour: min/max and mean +-1 sigma)
// along the line from the pixel's own colour to the history. The box always holds the pixel's own colour, so a clipped
// history keeps the texture detail it had: a shot's flash or anything else that changes the lighting only makes the
// history follow the new brightness. The blend gives 1/20 of this frame where nothing moves, up to 1/4 for fast
// motion, weighted by inverse brightness (bright sparks do not flicker).
layout(set = 0, binding = 0) uniform sampler2D cur;
layout(set = 0, binding = 1) uniform sampler2D hist;
layout(set = 0, binding = 2) uniform sampler2D mvt;
layout(push_constant) uniform PC {
    vec4 info; // x history valid, y motion vectors, z weight of this frame where nothing moves, w for fast motion
    vec4 jit;  // xy: this frame's jitter in pixels (what was drawn at texel q belongs at q - jitter), z: kernel constant (0 = no jitter)
    vec4 ad;   // unused
} pc;
layout(location = 0) out vec4 oh;
layout(location = 1) out vec4 oc;
ivec2 sz;
vec3 tm(vec3 c) { c = max(c, vec3(0.0)); return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0 / 65504.0); }
vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
vec3 hf(ivec2 p) { return texelFetch(hist, clamp(p, ivec2(0), sz - 1), 0).rgb; }
vec3 hs(vec2 uv) { // Catmull-Rom; exact at texel centres (no motion: the history comes back unchanged)
    vec2 q = uv * vec2(sz) - 0.5;
    vec2 i = floor(q);
    vec2 f = q - i;
    vec2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    vec3 r = vec3(0.0);
    ivec2 b = ivec2(i) - 1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) r += hf(b + ivec2(x, y)) * (w[x].x * w[y].y);
    return clamp(r, vec3(0.0), vec3(1.0));
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    sz = textureSize(cur, 0);
    vec3 c0 = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0), mn = vec3(1e9), mx = vec3(-1e9);
    float cw = 0.0, best = -1.0;
    vec2 mv = vec2(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz - 1);
            vec3 t = tm(texelFetch(cur, q, 0).rgb);
            // this pixel's own colour: without jitter the pixel itself; with jitter a Gaussian at the true sample spots
            vec2 d = vec2(x, y) - pc.jit.xy;
            float w = pc.jit.z > 0.0 ? exp(-pc.jit.z * dot(d, d)) : (x == 0 && y == 0 ? 1.0 : 0.0);
            c0 += t * w;
            cw += w;
            vec3 s = ycc(t);
            m1 += s;
            m2 += s * s;
            mn = min(mn, s);
            mx = max(mx, s);
            if (pc.info.y > 0.5) {
                vec2 v = texelFetch(mvt, q, 0).xy;
                float l = dot(v, v);
                if (l > best) { best = l; mv = v; }
            }
        }
    c0 /= cw;
    vec3 c = ycc(c0);
    m1 /= 9.0;
    vec3 sd = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));
    vec3 lo = min(max(mn, m1 - sd), c), hi = max(min(mx, m1 + sd), c); // the box always holds the pixel's own colour
    vec2 uv = (vec2(p) + 0.5 + mv) / vec2(sz);
    vec3 o = c0;
    if (pc.info.x > 0.5 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
        vec3 h = ycc(hs(uv));
        vec3 dv = h - c; // clip: walk from the pixel's colour towards the history, stop where the box ends
        float t = 1.0;
        for (int i = 0; i < 3; i++)
            if (abs(dv[i]) > 1e-7) t = min(t, max(((dv[i] > 0.0 ? hi[i] : lo[i]) - c[i]) / dv[i], 0.0));
        vec3 hr = rgb(c + dv * t);
        float a = mix(pc.info.z, pc.info.w, smoothstep(0.5, 8.0, length(mv)));
        float wc = a / (1.0 + c.x), wh = (1.0 - a) / (1.0 + ycc(hr).x);
        o = (c0 * wc + hr * wh) / (wc + wh);
    }
    o = clamp(o, vec3(0.0), vec3(0.999)); // YCoCg can reach outside RGB
    oh = vec4(o, 1.0);
    oc = vec4(itm(o), 1.0);
}
