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

layout(set = 0, binding = 8) readonly buffer _ml_lights
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
    MegaLight ml_lights[];
};

layout(set = 0, binding = 11) uniform highp sampler2D ml_history;
layout(set = 0, binding = 12, rgba16f) uniform highp writeonly image2D ml_history_out;

layout(set = 0, binding = 9) readonly buffer _ml_tile_indices
{
    highp uint ml_tile_indices[];
};

layout(set = 0, binding = 10) readonly buffer _ml_tile_ranges
{
    highp uvec2 ml_tile_ranges[];
};

layout(set = 0, binding = 3) uniform sampler2D brdfLUT_sampler;
layout(set = 0, binding = 4) uniform samplerCube irradiance_sampler;
layout(set = 0, binding = 5) uniform samplerCube specular_sampler;
layout(set = 0, binding = 7) uniform highp sampler2D directional_light_shadow;

layout(input_attachment_index = 0, set = 1, binding = 0) uniform highp subpassInput in_gbuffer_a;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform highp subpassInput in_gbuffer_b;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform highp subpassInput in_gbuffer_c;
layout(input_attachment_index = 3, set = 1, binding = 3) uniform highp subpassInput in_scene_depth;

layout(set = 2, binding = 1) uniform samplerCube skybox_sampler;

layout(location = 0) in highp vec2 in_texcoord;
layout(location = 0) out highp vec4 out_color;

#include "mesh_lighting.h"
#include "megalights_shading.inl"
#include "megalights_compose.inl"

void main()
{
    PGBufferData gbuffer;
    highp vec4 gbuffer_a = subpassLoad(in_gbuffer_a).rgba;
    highp vec4 gbuffer_b = subpassLoad(in_gbuffer_b).rgba;
    highp vec4 gbuffer_c = subpassLoad(in_gbuffer_c).rgba;
    DecodeGBufferData(gbuffer, gbuffer_a, gbuffer_b, gbuffer_c);

    highp vec3 N                   = gbuffer.worldNormal;
    highp vec3 basecolor           = gbuffer.baseColor;
    highp float metallic            = gbuffer.metallic;
    highp float dielectric_specular = 0.08 * gbuffer.specular;
    highp float roughness           = gbuffer.roughness;

    highp vec3 in_world_position;
    highp float scene_depth;
    {
        scene_depth                            = subpassLoad(in_scene_depth).r;
        highp vec4 ndc                         = vec4(uv_to_ndcxy(in_texcoord), scene_depth, 1.0);
        highp mat4 inverse_proj_view_matrix    = inverse(proj_view_matrix);
        highp vec4 in_world_position_with_w     = inverse_proj_view_matrix * ndc;
        in_world_position                      = in_world_position_with_w.xyz / in_world_position_with_w.www;
    }

    highp vec3 result_color = vec3(0.0, 0.0, 0.0);

    if (SHADINGMODELID_UNLIT == gbuffer.shadingModelID)
    {
        if (show_skybox == 0U)
        {
            discard;
        }

        highp vec3 in_UVW            = normalize(in_world_position - camera_position);
        highp vec3 origin_sample_UVW = vec3(in_UVW.x, in_UVW.z, in_UVW.y);
        result_color                 = textureLod(skybox_sampler, origin_sample_UVW, 0.0).rgb;
    }
    else if (SHADINGMODELID_DEFAULT_LIT == gbuffer.shadingModelID)
    {
        highp vec3 V   = normalize(camera_position - in_world_position);
        highp vec3 R   = reflect(-V, N);
        highp vec3 F0  = mix(vec3(dielectric_specular), basecolor, metallic);
        highp vec3 origin_samplecube_N = vec3(N.x, N.z, N.y);
        highp vec3 origin_samplecube_R = vec3(R.x, R.z, R.y);

        highp vec3 lo_raw = megalights_stochastic_direct(in_world_position,
                                                       scene_depth,
                                                       N,
                                                       V,
                                                       F0,
                                                       basecolor,
                                                       metallic,
                                                       roughness);
        highp vec3 Lo = megalights_temporal_denoise(lo_raw,
                                                    in_world_position,
                                                    scene_depth,
                                                    in_texcoord);

        if (ml_spatial_enable != 0u)
        {
            highp vec3 stable = megalights_stable_contrib(in_world_position,
                                                          N,
                                                          V,
                                                          F0,
                                                          basecolor,
                                                          metallic,
                                                          roughness,
                                                          origin_samplecube_N,
                                                          origin_samplecube_R);
            result_color = Lo + stable;
        }
        else
        {
            // Directional (legacy shadow map path)
            {
                highp vec3 L   = normalize(scene_directional_light.direction);
                highp float NoL = min(dot(N, L), 1.0);
                if (NoL > 0.0)
                {
                    highp float shadow;
                    {
                        highp vec4 position_clip = directional_light_proj_view * vec4(in_world_position, 1.0);
                        highp vec3 position_ndc  = position_clip.xyz / position_clip.w;
                        highp vec2 uv            = ndcxy_to_uv(position_ndc.xy);
                        highp float closest_depth = texture(directional_light_shadow, uv).r + 0.000075;
                        highp float current_depth = position_ndc.z;
                        shadow = (closest_depth >= current_depth) ? 1.0 : 0.0;
                    }
                    if (shadow > 0.0)
                    {
                        Lo += BRDF(L, V, N, F0, basecolor, metallic, roughness) * scene_directional_light.color * NoL;
                    }
                }
            }

            highp vec3 La = basecolor * ambient_light;

            highp vec3 irradiance = texture(irradiance_sampler, origin_samplecube_N).rgb;
            highp vec3 diffuse    = irradiance * basecolor;
            highp vec3 F          = F_SchlickR(clamp(dot(N, V), 0.0, 1.0), F0, roughness);
            highp vec2 brdfLUT    = texture(brdfLUT_sampler, vec2(clamp(dot(N, V), 0.0, 1.0), roughness)).rg;
            highp float lod       = roughness * MAX_REFLECTION_LOD;
            highp vec3 reflection = textureLod(specular_sampler, origin_samplecube_R, lod).rgb;
            highp vec3 specular   = reflection * (F * brdfLUT.x + brdfLUT.y);
            highp vec3 kD         = (1.0 - F) * (1.0 - metallic);
            highp vec3 Libl       = kD * diffuse + specular;

            result_color = Lo + La + Libl;
        }
    }

    out_color = vec4(result_color, 1.0);
}
