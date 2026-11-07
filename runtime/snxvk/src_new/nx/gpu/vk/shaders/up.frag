#version 450
// SwitchRecomp (hdr.c vk_upsample_draw, [Graphics] bloom_quality=1): a glow target at the game's own size, spread over
// the real pixels of the full-size target that reads it with a Catmull-Rom cubic instead of one bilinear tap per pixel.
// It passes through the texels, so the glow keeps the game's strength, and the result stays within the 4 nearest
// texels (no ringing): the game's soft glow without blocky steps at 2x and above. As gpu_gl.c UP_FS.
layout(set = 0, binding = 0) uniform sampler2D img;
layout(push_constant) uniform PC {
    vec2 ts;  // 1 / the glow target's size
    vec2 inv; // 1 / the output's size
} pc;
layout(location = 0) out vec4 o;

void main() {
    ivec2 sz = textureSize(img, 0);
    vec2 p = gl_FragCoord.xy * pc.inv * vec2(sz) - 0.5;
    vec2 fi = floor(p), f = p - fi;
    ivec2 i = ivec2(fi);
    vec2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    vec4 c = vec4(0.0), lo = vec4(1e30), hi = vec4(-1e30);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            vec4 s = texelFetch(img, clamp(i + ivec2(x - 1, y - 1), ivec2(0), sz - 1), 0);
            c += w[x].x * w[y].y * s;
            if (x >= 1 && x <= 2 && y >= 1 && y <= 2) {
                lo = min(lo, s);
                hi = max(hi, s);
            }
        }
    o = clamp(c, lo, hi);
}
