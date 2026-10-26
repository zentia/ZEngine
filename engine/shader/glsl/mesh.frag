#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
#include "structures.h"
#include "material_custom_params.h"

layout(set = 0, binding = 0) readonly buffer _main_camera_per_frame
{
    MainCameraPerFrame per_frame;
};
#include "main_camera_per_frame_access.inl"

layout(set = 0, binding = 3) uniform sampler2D brdfLUT_sampler;
layout(set = 0, binding = 4) uniform samplerCube irradiance_sampler;
layout(set = 0, binding = 5) uniform samplerCube specular_sampler;
layout(set = 0, binding = 6) uniform highp sampler2DArray point_lights_shadow;
layout(set = 0, binding = 7) uniform highp sampler2DArray directional_light_shadow;

// 布局必须与 CPU 侧 MeshMaterialUniform（Runtime/Function/Render/RenderCommon.h）
// 逐字节一致。三个 _padding 把 customParams 顶到 64（std140 下 vec4 数组要落在
// 16 字节边界，is_double_sided 结束在 52）。
layout(set = 2, binding = 0) uniform _unused_name_permaterial
{
    highp vec4  baseColorFactor;
    highp float metallicFactor;
    highp float roughnessFactor;
    highp float normalScale;
    highp float occlusionStrength;
    highp vec3  emissiveFactor;
    uint        is_blend;
    uint        is_double_sided;
    uint        _padding_0;
    uint        _padding_1;
    uint        _padding_2;
    highp vec4  customParams[4];
};

layout(set = 2, binding = 1) uniform sampler2D base_color_texture_sampler;
layout(set = 2, binding = 2) uniform sampler2D metallic_roughness_texture_sampler;
layout(set = 2, binding = 3) uniform sampler2D normal_texture_sampler;
layout(set = 2, binding = 4) uniform sampler2D occlusion_texture_sampler;
layout(set = 2, binding = 5) uniform sampler2D emissive_color_texture_sampler;

// read in fragnormal (from vertex shader)
layout(location = 0) in highp vec3 in_world_position;
layout(location = 1) in highp vec3 in_normal;
layout(location = 2) in highp vec3 in_tangent;
layout(location = 3) in highp vec2 in_texcoord;

layout(location = 0) out highp vec4 out_scene_color;

highp vec3 getBasecolor()
{
    highp vec3 basecolor = texture(base_color_texture_sampler, in_texcoord).xyz * baseColorFactor.xyz;
    return basecolor;
}

highp vec3 calculateNormal()
{
    highp vec3 tangent_normal = texture(normal_texture_sampler, in_texcoord).xyz * 2.0 - 1.0;

    highp vec3 N = normalize(in_normal);
    highp vec3 T = normalize(in_tangent.xyz);
    highp vec3 B = normalize(cross(N, T));

    highp mat3 TBN = mat3(T, B, N);
    return normalize(TBN * tangent_normal);
}

#include "mesh_lighting.h"

void main()
{
    // 溶解（动态属性块 customParams[0]/[1]）。没开溶解时读完 amount 就返回。
    highp vec3 dissolve_edge = vec3(0.0);
    if (ZMaterialDissolveCull(in_world_position, customParams[0], customParams[1], dissolve_edge))
    {
        discard;
    }

    highp vec3  N                   = calculateNormal();
    highp vec3  basecolor           = getBasecolor();
    highp float metallic            = texture(metallic_roughness_texture_sampler, in_texcoord).z * metallicFactor;
    highp float dielectric_specular = 0.04;
    highp float roughness           = texture(metallic_roughness_texture_sampler, in_texcoord).y * roughnessFactor;

    highp vec3 result_color;

#include "mesh_lighting.inl"

    // 前向通道有真正的输出颜色，溶解边缘按自发光直接加上去（不参与光照）。
    out_scene_color = vec4(result_color + dissolve_edge, 1.0);
}
