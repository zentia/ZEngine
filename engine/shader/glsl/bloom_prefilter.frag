#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
precision highp float;
precision highp int;

// ---- Bloom pass 1/4: threshold + soft knee + 4-tap half-res downsample --------------
// Reads the full-resolution HDR scene colour and writes the first (half-resolution)
// mip of the bloom chain. Doing the threshold here -- before any blur -- is what keeps
// the mip chain from spreading energy that should never have bloomed in the first
// place, and it is also the cheapest place to do it (one full-res read, no more).
//
// NOTE ON `const`: GLSL requires a const initialiser to be a constant expression, so
// nothing sampled from a texture or read out of a uniform block may be const-qualified.
// Only the literal-initialised weights below are.

layout(set = 0, binding = 0) uniform sampler2D in_color;

// Shared by all four bloom stages; see BloomPass::BloomUniformBuffer for the C++ side.
// std140: three vec4s, 48 bytes, no padding surprises.
layout(set = 0, binding = 1) uniform BloomUB
{
    // x: threshold, y: threshold - knee, z: knee * 2, w: 0.25 / knee
    vec4 filter_params;
    // x: intensity, y: scatter, z: clamp_max, w: unused
    vec4 combine_params;
    // xy: 1/size of the input texture, zw: 1/size of the output texture
    vec4 texel_size;
} ubo;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);

// Karis average (SIGGRAPH 2014, "Next Generation Post Processing in Call of Duty"):
// weight each tap by 1/(1+luma) so one firefly pixel inside a dark neighbourhood
// cannot dominate the whole downsample. Without it a single bright specular dot
// punches a visible square into the bloom.
vec3 KarisAverage(vec3 color)
{
    float luma = dot(color, kLumaWeights) * 0.25;
    return color * (1.0 / (1.0 + luma));
}

void main()
{
    vec2 ts = ubo.texel_size.xy;

    // 4-tap box: the destination is half the source resolution, so a 2x2 source box
    // maps exactly onto one destination pixel.
    vec3 a = texture(in_color, in_uv + vec2(-1.0, -1.0) * ts).rgb;
    vec3 b = texture(in_color, in_uv + vec2( 1.0, -1.0) * ts).rgb;
    vec3 c = texture(in_color, in_uv + vec2(-1.0,  1.0) * ts).rgb;
    vec3 d = texture(in_color, in_uv + vec2( 1.0,  1.0) * ts).rgb;

    vec3 color = (a + b + c + d) * 0.25;
    color = KarisAverage(color);

    // Soft knee curve (same shape Unity uses): a hard cutoff leaves a visible seam where
    // brightness crosses the threshold, the knee ramps the contribution in smoothly
    // over `knee` units below it.
    float brightness = max(color.r, max(color.g, color.b));
    float soft = clamp(brightness - ubo.filter_params.y, 0.0, ubo.filter_params.z);
    soft = ubo.filter_params.w * soft * soft;
    float contribution = max(soft, brightness - ubo.filter_params.x) / max(brightness, 1e-5);

    out_color = vec4(color * contribution, 1.0);
}
