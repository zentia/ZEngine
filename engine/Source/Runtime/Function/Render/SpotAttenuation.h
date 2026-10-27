#pragma once

// ============================================================================
// SpotAttenuation — 聚光灯锥角衰减的纯算术核心（无引擎依赖，header-only）
// ----------------------------------------------------------------------------
// 把"聚光灯在锥内全亮、锥外为零、内外锥之间平滑过渡"这件事抽成不依赖任何引擎
// 类型的 inline 函数，与 gas_cooldown / gas_periodic 同一手法：核心是纯函数，可
// 脱离 RenderScene / LightComponent / shader 单独单测（SpotLightSmokeTest）。
//
// 这是 shader 侧 mesh_lighting.inl 里那段 smoothstep 的 CPU 权威镜像——两边必须
// 语义一致：shader 用它算逐像素光照，CPU 用它做冒烟验证与（未来的）CPU 裁剪/拾取。
//
// 语义（与 GLSL/HLSL smoothstep(edge0, edge1, x) 完全一致）：
//   - 输入是"光线方向与聚光朝向夹角的余弦"（cos_theta），以及内外锥半角的余弦
//     （inner_cos >= outer_cos，因为角度越小余弦越大）。
//   - cos_theta >= inner_cos（在锥心内）  -> 1.0（全亮）
//   - cos_theta <= outer_cos（在锥外）    -> 0.0（无光）
//   - 介于两者之间                          -> Hermite 平滑过渡 (0,1)
//   - 退化保护：inner_cos == outer_cos（无过渡带）时退成硬阈值 step；
//     inner_cos < outer_cos（非法配置，内外颠倒）时按 outer==inner 处理，不产生负值。
// ============================================================================

namespace spot_attenuation
{
    // 与 GPU smoothstep(edge0, edge1, x) 同构的标量实现。
    inline float SmoothStep(float edge0, float edge1, float x)
    {
        if (edge0 == edge1)
        {
            // 退化：无过渡带，退成硬阈值（x < edge0 -> 0，否则 1）。
            return x < edge0 ? 0.0f : 1.0f;
        }
        float t = (x - edge0) / (edge1 - edge0);
        if (t < 0.0f)
            t = 0.0f;
        else if (t > 1.0f)
            t = 1.0f;
        // Hermite: 3t^2 - 2t^3
        return t * t * (3.0f - 2.0f * t);
    }

    // 聚光锥角衰减因子 [0,1]。inner_cos / outer_cos 为内外锥半角的余弦。
    inline float ConeAttenuation(float cos_theta, float inner_cos, float outer_cos)
    {
        // 非法配置（内外颠倒，inner_cos < outer_cos）：交换，避免负分母产生越界值。
        if (inner_cos < outer_cos)
        {
            const float tmp = inner_cos;
            inner_cos = outer_cos;
            outer_cos = tmp;
        }
        return SmoothStep(outer_cos, inner_cos, cos_theta);
    }
}  // namespace spot_attenuation
