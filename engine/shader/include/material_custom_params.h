// 内置 shader 对「动态属性块」customParams 的消费。
//
// customParams 是 4 个 vec4，槽位含义由引擎静态表写死（运行时侧见
// Runtime/Function/Framework/Component/Mesh/MaterialCustomParamNames.h）：
//
//   customParams[0].x = DissolveAmount    0=不溶解，1=全溶解
//   customParams[0].y = DissolveEdgeWidth 边缘带宽度（噪声空间）
//   customParams[1]   = DissolveEdgeColor rgb=颜色，a=强度
//   customParams[2]   = CustomParam0      项目自留，shader 不解释
//   customParams[3]   = CustomParam1      项目自留，shader 不解释
//
// 为什么是动态分支而不是 keyword 变体：内置 shader 走
// CanUseRuntimePrimaryShaderPass() 恒为 false（vertex/fragment_shader_file 为空），
// 拿不到带宏编译的 PSO，keyword 在今天的内置路径上改不到实际 shader。动态分支比
// 变体编译费一点，但它是唯一"今天就能生效"的做法。等 ShaderLab 运行时链路通了，
// 可以再叠加 keyword 版本。
//
// 调用方需要提供 vec3 世界坐标（片元位置）。溶解是**世界空间**的，不是 UV 空间的：
// 角色做溶解时希望从身上某个方向烧过去，跟贴图怎么展没关系。

#ifndef MATERIAL_CUSTOM_PARAMS_H
#define MATERIAL_CUSTOM_PARAMS_H

// 溶解噪声。描述符集 2 的 binding 0-5 已经被 UBO + 5 张贴图占满，没有空位再挂
// 一张噪声图，所以用世界坐标哈希做程序化噪声。floor(...) 把它量化成块状 —— 经典
// 溶解就是这个样子，而且块的大小可以直接调。
highp float ZMaterialDissolveNoise(highp vec3 world_position)
{
    highp vec3 cell = floor(world_position * 24.0);
    highp vec3 p    = fract(cell * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// 返回 true = 这个片元该被 discard（已经溶掉）。
// out_edge_emissive 是边缘带的自发光，只有落在边缘带里才非零。
//
// amount <= 0 时提前返回，不付噪声的代价 —— 绝大多数物体根本没开溶解。
bool ZMaterialDissolveCull(highp vec3 world_position,
                           highp vec4 dissolve_params,
                           highp vec4 edge_color,
                           out highp vec3 out_edge_emissive)
{
    out_edge_emissive = vec3(0.0);

    highp float amount = dissolve_params.x;
    if (amount <= 0.0)
    {
        return false;
    }

    highp float noise = ZMaterialDissolveNoise(world_position);

    // 乘 (1 + edge) 是为了让 amount=1 时把边缘带一起吃掉，否则烧到最后会剩一圈
    // 亮边挂着不肯消失。
    highp float edge      = max(dissolve_params.y, 0.0);
    highp float threshold = amount * (1.0 + edge);

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
