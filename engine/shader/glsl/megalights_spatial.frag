#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
#include "gbuffer.h"
#include "megalights_definitions.h"

struct DirectionalLight
{
    highp vec3 direction;
    lowp float _padding_direction;
    highp vec3 color;
    lowp float _padding_color;
};

struct PointLight
{
    highp vec3 position;
    highp float radius;
    highp vec3 intensity;
    lowp float _padding_intensity;
};

layout(set = 0, binding = 0) readonly buffer _mesh_per_frame
{
    highp mat4 proj_view_matrix;
    highp vec3 camera_position;
    lowp float _padding_camera_position;
    highp vec3 ambient_light;
    lowp float _padding_ambient_light;
    highp uint point_light_num;
    highp uint show_skybox;
    uint _padding_point_light_num_2;
    uint _padding_point_light_num_3;
    PointLight scene_point_lights[m_max_point_light_count];
    DirectionalLight scene_directional_light;
    highp mat4 directional_light_proj_view;
};

layout(set = 0, binding = 8) readonly buffer _ml_header
{
    highp uint ml_light_count;
    highp uint ml_tile_count_x;
    highp uint ml_tile_count_y;
    highp uint ml_num_samples;
    highp uint ml_frame_index;
    highp uint ml_ss_steps;
    highp uint ml_viewport_width;
    highp uint ml_viewport_height;
    highp uint ml_temporal_enable;
    highp uint ml_history_valid;
    highp float ml_temporal_blend;
    highp float ml_disocclusion_threshold;
    highp mat4 ml_prev_proj_view;
    highp uint ml_spatial_enable;
    highp uint ml_spatial_radius;
    highp float ml_spatial_depth_sigma;
    highp float ml_spatial_normal_power;
};

layout(set = 0, binding = 3) uniform sampler2D brdfLUT_sampler;
layout(set = 0, binding = 4) uniform samplerCube irradiance_sampler;
layout(set = 0, binding = 5) uniform samplerCube specular_sampler;
layout(set = 0, binding = 7) uniform highp sampler2D directional_light_shadow;
layout(set = 0, binding = 11) uniform highp sampler2D ml_direct;
layout(set = 0, binding = 12, rgba16f) uniform highp writeonly image2D ml_history_out;

layout(set = 1, binding = 0) uniform highp sampler2D spatial_gbuffer_a;
layout(set = 1, binding = 1) uniform highp sampler2D spatial_gbuffer_b;
layout(set = 1, binding = 2) uniform highp sampler2D spatial_gbuffer_c;
layout(set = 1, binding = 3) uniform highp sampler2D spatial_scene_depth;

layout(location = 0) in highp vec2 in_texcoord;
layout(location = 0) out highp vec4 out_color;

#include "mesh_lighting.h"
#include "megalights_compose.inl"
#include "megalights_spatial_filter.inl"

void main()
{
    if (ml_spatial_enable == 0u)
    {
        discard;
    }

    highp vec4 gbuffer_a = texture(spatial_gbuffer_a, in_texcoord);
    highp vec4 gbuffer_b = texture(spatial_gbuffer_b, in_texcoord);
    highp vec4 gbuffer_c = texture(spatial_gbuffer_c, in_texcoord);
    highp float scene_depth = texture(spatial_scene_depth, in_texcoord).r;

    PGBufferData gbuffer;
    DecodeGBufferData(gbuffer, gbuffer_a, gbuffer_b, gbuffer_c);
    if (SHADINGMODELID_DEFAULT_LIT != gbuffer.shadingModelID)
    {
        discard;
    }

    highp vec3 N = gbuffer.worldNormal;
    highp vec3 basecolor = gbuffer.baseColor;
    highp float metallic = gbuffer.metallic;
    highp float dielectric_specular = 0.08 * gbuffer.specular;
    highp float roughness = gbuffer.roughness;

    highp vec4 ndc = vec4(uv_to_ndcxy(in_texcoord), scene_depth, 1.0);
    highp mat4 inverse_proj_view_matrix = inverse(proj_view_matrix);
    highp vec4 world_pos_h = inverse_proj_view_matrix * ndc;
    highp vec3 world_pos = world_pos_h.xyz / world_pos_h.w;

    highp vec3 V = normalize(camera_position - world_pos);
    highp vec3 R = reflect(-V, N);
    highp vec3 F0 = mix(vec3(dielectric_specular), basecolor, metallic);
    highp vec3 origin_samplecube_N = vec3(N.x, N.z, N.y);
    highp vec3 origin_samplecube_R = vec3(R.x, R.z, R.y);

    highp vec3 lo_filtered = megalights_spatial_filter(ml_direct,
                                                       spatial_scene_depth,
                                                       spatial_gbuffer_a,
                                                       in_texcoord,
                                                       scene_depth,
                                                       N);

    ivec2 pix = ivec2(gl_FragCoord.xy);
    imageStore(ml_history_out, pix, vec4(lo_filtered, 1.0));

    highp vec3 stable = megalights_stable_contrib(world_pos,
                                                  N,
                                                  V,
                                                  F0,
                                                  basecolor,
                                                  metallic,
                                                  roughness,
                                                  origin_samplecube_N,
                                                  origin_samplecube_R);
    out_color = vec4(lo_filtered + stable, 1.0);
}
