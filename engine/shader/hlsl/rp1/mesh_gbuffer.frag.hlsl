// DX-B3: gbuffer MRT output (layout matches shader/include/gbuffer.h).

#include "../common/material_custom_params.hlsli"
#include "../common/vt_common.hlsli"

// 布局必须与 CPU 侧 MeshMaterialUniform（Runtime/Function/Render/RenderCommon.h）
// 逐字节一致。padding 写成三个独立 uint 而不是 uint3 的原因见
// hlsl/common/render_storage_structs.hlsli 里的注释（uint3 会被整体挪到下一个
// 寄存器，customParams 会落到 80 而不是 64，和 GLSL 的 std140 布局错开）。
cbuffer PerMaterial : register(b0, space2)
{
    float4 baseColorFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float3 emissiveFactor;
    uint is_blend;
    uint is_double_sided;
    uint vt_flags;  // 位 0 = base color 经 Virtual Texture 采样；位 1 = 采样结果按 sRGB 解码（见下）
    uint _padding_1;
    uint _padding_2;
    float4 customParams[4];
};

Texture2D base_color_texture : register(t1, space2);
SamplerState base_color_sampler : register(s1, space2);

Texture2D metallic_roughness_texture : register(t2, space2);
SamplerState metallic_roughness_sampler : register(s2, space2);

Texture2D normal_texture : register(t3, space2);
SamplerState normal_sampler : register(s3, space2);

// 材质里的 Virtual Texture 槽位（仅 DX12 的网格材质布局有这两个 binding；每帧由 RenderResource 重写）。
//   vt_flags 位 0 置位时 customParams[2] / [3] 被 VT 参数占用（uint 位模式，不是浮点）：
//     customParams[2] = { page_size, page_border, mip_count, 0 }, customParams[3] = { virtual_width, virtual_height, 0, 0 }
//   页池宽度（页数）= 页池纹理宽度 / 槽位大小，用 GetDimensions 取，不进 UBO。
// 采样契约见 vt_common.hlsli（mip 由 uv 的屏幕空间导数选，缺页回退到更粗的驻留 mip，页池 LINEAR+clamp 双线性）。
Texture2D<float2> vt_page_table : register(t6, space2);
Texture2D<float4> vt_pool : register(t7, space2);
SamplerState vt_pool_sampler : register(s7, space2);

struct PsInput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
};

struct PsOutput
{
    float4 gbuffer_a : SV_Target0;
    float4 gbuffer_b : SV_Target1;
    float4 gbuffer_c : SV_Target2;
};

float3 EncodeNormal(float3 n)
{
    return n * 0.5f + 0.5f;
}

PsOutput main(PsInput input)
{
    PsOutput output;

    // 导数必须在统一控制流里取（vt_flags 是 UBO 常量，整个 draw 一致，但下面还有 discard —— 先取最安全）。
    const float2 uv_ddx = ddx(input.texcoord);
    const float2 uv_ddy = ddy(input.texcoord);

    // 溶解（动态属性块 customParams[0]/[1]）。没开溶解时读完 amount 就返回。
    // HLSL 的 discard 不像 GLSL 那样终止执行，所以后面补一个 return。
    float3 dissolve_edge = float3(0.0f, 0.0f, 0.0f);
    if (ZMaterialDissolveCull(input.world_position, customParams[0], customParams[1], dissolve_edge))
    {
        discard;
        output.gbuffer_a = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.gbuffer_b = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.gbuffer_c = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return output;
    }

    float3 base_color = base_color_texture.Sample(base_color_sampler, input.texcoord).rgb;
    if ((vt_flags & 1u) != 0u)
    {
        VTLookupParams vt;
        vt.page_size = asuint(customParams[2].x);
        vt.border = asuint(customParams[2].y);
        vt.mip_count = asuint(customParams[2].z);
        vt.virtual_size = uint2(asuint(customParams[3].x), asuint(customParams[3].y));
        vt.pages_x = (vt.virtual_size.x + vt.page_size - 1u) / vt.page_size;
        vt.pages_y = (vt.virtual_size.y + vt.page_size - 1u) / vt.page_size;
        uint pool_w, pool_h;
        vt_pool.GetDimensions(pool_w, pool_h);
        vt.pool_width_pages = pool_w / VTSlotSize(vt);

        bool vt_resident;
        uint vt_mip;
        // 缺页（没有任何驻留的 mip 盖住这个采样点）时保持普通 base color，不画洞。
        const float4 vt_color = VTSampleBilinear(vt_page_table, vt_pool, vt_pool_sampler, vt, input.texcoord,
                                                 VTLod(vt, uv_ddx, uv_ddy), float4(base_color, 1.0f), vt_resident, vt_mip);
        if (vt_resident)
        {
            base_color = ((vt_flags & 2u) != 0u) ? VTSrgbToLinear(vt_color.rgb) : vt_color.rgb;
        }
    }
    float3 albedo = base_color * baseColorFactor.rgb;
    float4 mr = metallic_roughness_texture.Sample(metallic_roughness_sampler, input.texcoord);
    float metallic = mr.b * metallicFactor;
    float roughness = mr.g * roughnessFactor;

    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3 tangent_normal = normal_texture.Sample(normal_sampler, input.texcoord).xyz * 2.0f - 1.0f;
    tangent_normal.xy *= normalScale;
    N = normalize(mul(float3x3(T, B, N), tangent_normal));

    // GBuffer 三个 RT 没有空位放自发光，溶解边缘只能烘进 albedo（会被正常打光）。
    output.gbuffer_a = float4(EncodeNormal(N), 1.0f);
    output.gbuffer_b = float4(metallic, 0.5f, roughness, 1.0f / 255.0f);
    output.gbuffer_c = float4(albedo + dissolve_edge, 1.0f);
    return output;
}
