#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
precision highp float;
precision highp int;

// ---- Bloom pass 2/4: 13-tap downsample ----------------------------------------------
// A plain 4-tap box aliases badly once the mip gets small: bright pixels flicker in and
// out as the camera moves. The 13-tap filter (Jimenez, SIGGRAPH 2014) is a cheap
// approximation of a wide Gaussian -- five 4-tap boxes, the centre one weighted 4x --
// and is stable enough to be used unchanged for every step of the chain.

layout(set = 0, binding = 0) uniform sampler2D in_color;

layout(set = 0, binding = 1) uniform BloomUB
{
    vec4 filter_params;
    vec4 combine_params;
    vec4 texel_size;
} ubo;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 Fetch(float dx, float dy)
{
    return texture(in_color, in_uv + vec2(dx, dy) * ubo.texel_size.xy).rgb;
}

void main()
{
    //  A B C
    //   D E
    //  F G H
    //   I J
    //  K L M
    vec3 a = Fetch(-2.0, -2.0);
    vec3 b = Fetch( 0.0, -2.0);
    vec3 c = Fetch( 2.0, -2.0);
    vec3 d = Fetch(-1.0, -1.0);
    vec3 e = Fetch( 1.0, -1.0);
    vec3 f = Fetch(-2.0,  0.0);
    vec3 g = Fetch( 0.0,  0.0);
    vec3 h = Fetch( 2.0,  0.0);
    vec3 i = Fetch(-1.0,  1.0);
    vec3 j = Fetch( 1.0,  1.0);
    vec3 k = Fetch(-2.0,  2.0);
    vec3 l = Fetch( 0.0,  2.0);
    vec3 m = Fetch( 2.0,  2.0);

    // Each group of four is averaged (the 0.25) and the centre group carries half the
    // total weight: 0.5 * 0.25 = 0.125, and 0.125 * 0.25 = 0.03125 for the outer four.
    vec3 result = (d + e + i + j) * 0.125 +
                  (a + b + g + f) * 0.03125 +
                  (b + c + h + g) * 0.03125 +
                  (f + g + l + k) * 0.03125 +
                  (g + h + m + l) * 0.03125;

    out_color = vec4(result, 1.0);
}
