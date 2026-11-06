#version 450
// SWR TAA (dlss.c, ini taa=1): this frame's scene blended with the previous result, moved by the motion vectors
// (dlss_mv.frag: render pixels, current to previous) and limited to the colour range of the pixel's 3x3 neighbourhood
// (variance clipping in YCoCg on tone-mapped colour). History: RGBA16F, tone-mapped, read with a Catmull-Rom filter.
// Same maths as the OpenGL renderer's TAA_FS (gpu_gl.c).
layout(set = 0, binding = 0) uniform sampler2D cur;
layout(set = 0, binding = 1) uniform sampler2D hist;
layout(set = 0, binding = 2) uniform sampler2D mvt;
layout(push_constant) uniform PC {
    vec4 info; // x history valid, y motion vectors, z weight of this frame
} pc;
layout(location = 0) out vec4 oh;
layout(location = 1) out vec4 oc;
ivec2 sz;
vec3 tm(vec3 c) { c = max(c, vec3(0.0)); return c / (1.0 + max(c.r, max(c.g, c.b))); }
vec3 itm(vec3 c) { return c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0 / 65504.0); }
vec3 ycc(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }
vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
vec3 hf(ivec2 p) { return texelFetch(hist, clamp(p, ivec2(0), sz - 1), 0).rgb; }
vec3 hs(vec2 uv) {
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
    vec3 c0 = tm(texelFetch(cur, p, 0).rgb);
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    vec2 mv = vec2(0.0);
    float best = -1.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz - 1);
            vec3 s = ycc(tm(texelFetch(cur, q, 0).rgb));
            m1 += s;
            m2 += s * s;
            if (pc.info.y > 0.5) {
                vec2 v = texelFetch(mvt, q, 0).xy;
                float l = dot(v, v);
                if (l > best) { best = l; mv = v; }
            }
        }
    m1 /= 9.0;
    vec3 sd = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));
    vec2 uv = (vec2(p) + 0.5 + mv) / vec2(sz);
    vec3 o = c0;
    if (pc.info.x > 0.5 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
        vec3 h = clamp(ycc(hs(uv)), m1 - 1.25 * sd, m1 + 1.25 * sd);
        o = mix(rgb(h), c0, pc.info.z);
    }
    oh = vec4(o, 1.0);
    oc = vec4(itm(o), 1.0);
}
