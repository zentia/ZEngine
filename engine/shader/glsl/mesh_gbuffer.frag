#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"
#include "gbuffer.h"
#include "material_custom_params.h"

// 布局必须与 CPU 侧 MeshMaterialUniform（Runtime/Function/Render/RenderCommon.h）
// 逐字节一致。改任一处都要同步另一处。
//
// 三个 _padding 是为了把 customParams 顶到 64：is_double_sided 结束在 52，而
// std140 下 vec4 数组必须落在 16 字节边界。C++ 侧 Vector4 的对齐只有 4，靠
// 编译器排会落在 52 —— 所以 padding 是布局的一部分，不是装饰。
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

// output screen color to location 0
layout(location = 0) out highp vec4 out_gbuffer_a;
layout(location = 1) out highp vec4 out_gbuffer_b;
layout(location = 2) out highp vec4 out_gbuffer_c;
// layout(location = 3) out highp vec4 out_scene_color;

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

void main()
{
    // 溶解（动态属性块 customParams[0]/[1]）。没开溶解时 ZMaterialDissolveCull
    // 在读完 amount 后就返回，代价是一次比较。
    highp vec3 dissolve_edge = vec3(0.0);
    if (ZMaterialDissolveCull(in_world_position, customParams[0], customParams[1], dissolve_edge))
    {
        discard;
    }

    PGBufferData gbuffer;
    gbuffer.worldNormal = calculateNormal();
    gbuffer.baseColor   = getBasecolor();
    // GBuffer 三个 RT 没有空位放自发光（见 shader/include/gbuffer.h 顶部注释），
    // 所以溶解边缘只能烘进 baseColor —— 它会被正常打光，亮度不如前向通道里直接
    // 加的 emissive，但至少看得见。想要真正的自发光边缘得走前向 mesh.frag。
    gbuffer.baseColor += dissolve_edge;
    gbuffer.metallic       = texture(metallic_roughness_texture_sampler, in_texcoord).z * metallicFactor;
    gbuffer.specular       = 0.5;
    gbuffer.roughness      = texture(metallic_roughness_texture_sampler, in_texcoord).y * roughnessFactor;
    gbuffer.shadingModelID = SHADINGMODELID_DEFAULT_LIT;

    highp vec3 Le = texture(emissive_color_texture_sampler, in_texcoord).xyz * emissiveFactor;

    EncodeGBufferData(gbuffer, out_gbuffer_a, out_gbuffer_b, out_gbuffer_c);

    // out_scene_color.rgba = vec4(Le, 1.0);
}