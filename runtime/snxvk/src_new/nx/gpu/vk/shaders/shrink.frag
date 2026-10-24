#version 450
// SwitchRecomp (hdr.c vk_shrink): the game's frame shrunk to its on-screen size. A frame larger than the window
// (resolution 2 and up) went to the window through one linear blit, which reads 2x2 texels per window pixel: at 2x
// and more most of the frame was skipped, not averaged, and edges shimmered (GitHub issue #17). Each window pixel
// averages 4x4 filtered taps over its footprint instead, as the OpenGL window pass does (gpu_gl.c DOWN_FS pick).
layout(set = 0, binding = 0) uniform sampler2D img;
layout(push_constant) uniform PC {
    vec2 org;  // the frame's rectangle in the source image, its top left, in texels
    vec2 step; // source texels per window pixel
} pc;
layout(location = 0) out vec4 o;

void main() {
    vec2 inv = 1.0 / vec2(textureSize(img, 0));
    vec2 p = pc.org + gl_FragCoord.xy * pc.step; // the window pixel's center, in texels
    vec4 c = vec4(0.0);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) c += texture(img, (p + ((vec2(i, j) + 0.5) * 0.25 - 0.5) * pc.step) * inv);
    o = c * (1.0 / 16.0);
}
