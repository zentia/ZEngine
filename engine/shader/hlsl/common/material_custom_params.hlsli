// 内置 shader 对「动态属性块」customParams 的消费。GLSL 侧镜像见
// shader/include/material_custom_params.h —— 两边必须保持一致（槽位含义、噪声
// 公式、阈值公式），否则 Vulkan / Metal 和 DX12 会溶解出不一样的样子。
//
//   customParams[0].x = DissolveAmount    0=不溶解，1=全溶解
//   customParams[0].y = DissolveEdgeWidth 边缘带宽度（噪声空间）
//   customParams[1]   = DissolveEdgeColor rgb=颜色，a=强度
//   customParams[2]   = CustomParam0      项目自留，shader 不解释
//   customParams[3]   = CustomParam1      项目自留，shader 不解释

#ifndef MATERIAL_CUSTOM_PARAMS_HLSLI
#define MATERIAL_CUSTOM_PARAMS_HLSLI

// 描述符集 2 的 binding 0-5 已经被 UBO + 5 张贴图占满，没有空位再挂一张噪声图，
// 所以用世界坐标哈希做程序化噪声。floor(...) 量化成块状 = 经典溶解的样子。
float ZMaterialDissolveNoise(float3 world_position)
{
    float3 cell = floor(world_position * 24.0);
    float3 p    = frac(cell * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// 返回 true = 这个片元该被 discard（已经溶掉）。
// out_edge_emissive 是边缘带的自发光，只有落在边缘带里才非零。
bool ZMaterialDissolveCull(float3 world_position,
                           float4 dissolve_params,
                           float4 edge_color,
                           out float3 out_edge_emissive)
{
    out_edge_emissive = float3(0.0, 0.0, 0.0);

    float amount = dissolve_params.x;
    if (amount <= 0.0)
    {
        return false;
    }

    float noise = ZMaterialDissolveNoise(world_position);

    // 乘 (1 + edge) 是为了让 amount=1 时把边缘带一起吃掉，否则烧到最后会剩一圈
    // 亮边挂着不肯消失。
    float edge      = max(dissolve_params.y, 0.0);
    float threshold = amount * (1.0 + edge);

    if (noise < threshold - edge)
    {
        return true;
    }

    if (noise < threshold)
    {
        out_edge_emissive = edge_color.rgb * edge_color.a;
    }
    return false;
}

#endif
