#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
precision highp float;
precision highp int;

// ---- Bloom pass 3/4: 9-tap tent upsample --------------------------------------------
// COD's upsample is normally recorded with ADDITIVE blending into the larger mip (that
// is what makes the chain a true multi-scale sum). Additive blending means another
// pipeline state the pass has to own on every backend, so this variant takes the
// non-additive route instead: it reads BOTH the smaller mip and the same-resolution
// mip produced by the downsample chain and writes their sum. Same result, one RT write,
// no blend state.

layout(set = 0, binding = 0) uniform sampler2D in_lower;  // smaller (blurrier) mip
layout(set = 0, binding = 1) uniform BloomUB
{
    vec4 filter_params;
    vec4 combine_params;
    vec4 texel_size;
} ubo;
layout(set = 0, binding = 2) uniform sampler2D in_same;  // same-size mip from the down chain

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 TentFetch(float dx, float dy)
{
    return texture(in_lower, in_uv + vec2(dx, dy) * ubo.texel_size.xy).rgb;
}

void main()
{
    // 3x3 tent: weights 1 2 1 / 2 4 2 / 1 2 1, normalised by 1/16.
    vec3 lower = TentFetch(-1.0, -1.0) * 1.0 +
                 TentFetch( 0.0, -1.0) * 2.0 +
                 TentFetch( 1.0, -1.0) * 1.0 +
                 TentFetch(-1.0,  0.0) * 2.0 +
                 TentFetch( 0.0,  0.0) * 4.0 +
                 TentFetch( 1.0,  0.0) * 2.0 +
                 TentFetch(-1.0,  1.0) * 1.0 +
                 TentFetch( 0.0,  1.0) * 2.0 +
                 TentFetch( 1.0,  1.0) * 1.0;
    lower *= (1.0 / 16.0);

    vec3 same = texture(in_same, in_uv).rgb;

    // combine_params.z is the safety clamp: a pathological HDR scene can otherwise
    // produce a bloom that blows out and never recovers.
    vec3 sum = min(lower + same, vec3(ubo.combine_params.z));
    out_color = vec4(sum, 1.0);
}
