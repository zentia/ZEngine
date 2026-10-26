#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
precision highp float;
precision highp int;

// ---- Bloom pass 4/4: composite -------------------------------------------------------
// Adds the finished bloom back onto the scene. `scatter` widens the glow by blending the
// tight single-tap bloom against a 4-tap wide gather of the same texture -- a cheap
// stand-in for sampling a coarser mip, and it keeps the bloom chain single-mip (no mip
// allocation, no mip generation pass, works unchanged on every backend).

layout(set = 0, binding = 0) uniform sampler2D in_scene;
layout(set = 0, binding = 1) uniform BloomUB
{
    vec4 filter_params;
    vec4 combine_params;
    vec4 texel_size;
} ubo;
layout(set = 0, binding = 2) uniform sampler2D in_bloom;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    vec3 scene = texture(in_scene, in_uv).rgb;

    vec3 tight = texture(in_bloom, in_uv).rgb;

    // 4-tap wide gather at 4x the output texel size.
    vec2 ts = ubo.texel_size.zw * 4.0;
    vec3 wide = (texture(in_bloom, in_uv + vec2(-1.0, -1.0) * ts).rgb +
                 texture(in_bloom, in_uv + vec2( 1.0, -1.0) * ts).rgb +
                 texture(in_bloom, in_uv + vec2(-1.0,  1.0) * ts).rgb +
                 texture(in_bloom, in_uv + vec2( 1.0,  1.0) * ts).rgb) * 0.25;

    vec3 bloom = mix(tight, wide, clamp(ubo.combine_params.y, 0.0, 1.0));

    // Additive, in HDR, before tone mapping -- that is the whole point of doing bloom
    // on the HDR buffer rather than after the LDR conversion.
    out_color = vec4(scene + bloom * ubo.combine_params.x, 1.0);
}
