#version 450
// SWR XeGTAO (dlss.c): the AO factor, multiplied into the scene target by the pipeline's blend (dst * src colour)
layout(set = 0, binding = 0) uniform sampler2D aoF;
layout(push_constant) uniform PC { vec4 a; } pc; // x strength
layout(location = 0) out vec4 o;
void main() {
    float v = texelFetch(aoF, ivec2(gl_FragCoord.xy), 0).r;
    o = vec4(vec3(mix(1.0, v, pc.a.x)), 1.0);
}
