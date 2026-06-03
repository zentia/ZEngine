#version 310 es

#extension GL_GOOGLE_include_directive : enable

// TODO: geometry shader is inefficient for Mali GPU
#extension GL_EXT_geometry_shader : enable

#include "constants.h"
#include "structures.h"

layout(set = 0, binding = 0) readonly buffer _point_light_shadow_per_frame
{
    PointLightShadowPerFrame per_frame;
};

layout(location = 0) in highp float in_inv_length;
// NOTE: we can't interpolate the length of "position_view_space" directly, otherwise the result is incorrect
layout(location = 1) in highp vec3 in_inv_length_position_view_space;

layout(location = 0) out highp float out_depth;

void main()
{
    // perspective correct interpolation_
    highp vec3 position_view_space = in_inv_length_position_view_space / in_inv_length;

    highp float point_light_radius = per_frame.point_lights_position_and_radius[gl_Layer / 2].w;

    highp float ratio = length(position_view_space) / point_light_radius;

    // Trick: we don't write to depth, and thus we can use early depth test
    gl_FragDepth = ratio;
    out_depth = ratio;
}
