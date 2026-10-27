// ============================================================================
// SpotLightSmokeTest — 聚光灯锥角衰减纯算术核心（SpotAttenuation.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 spot_attenuation 命名空间里的两个 inline 纯函数：
//   - SmoothStep(edge0, edge1, x)：与 GLSL/HLSL smoothstep 同构（含退化保护）。
//   - ConeAttenuation(cos_theta, inner_cos, outer_cos)：锥内=1、锥外=0、中间平滑，
//     非法配置（内外颠倒）自动纠正。
//
// 为什么只测纯函数、不测 RenderScene/shader：聚光灯的"语义风险"全在这一段锥角
// 衰减算术里（内外锥边界、平滑过渡、退化/非法输入）。它是 shader 侧
// mesh_lighting.inl 那段 smoothstep 的 CPU 权威镜像，把它抽成 header-only 纯函数
// 即可脱离引擎单测——无需 RenderScene / LightComponent / RHI。场景收集与 UBO
// 上传的接线由编译期类型检查保证（ScenePointLight 字段一一对应）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_SPOTLIGHT_SMOKE_TEST=ON
//   cmake --build build --target SpotLightSmokeTest
// ============================================================================

#include "Runtime/Function/Render/SpotAttenuation.h"

#include <cstdio>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }

    void CheckNear(float a, float b, float eps, const char* msg)
    {
        const float diff = a - b;
        Check(diff >= -eps && diff <= eps, msg);
    }
}  // namespace

int main()
{
    std::printf("SpotLightSmokeTest (pure cone-attenuation core)\n");

    // --- SmoothStep: 基本边界 -------------------------------------------------
    Check(spot_attenuation::SmoothStep(0.0f, 1.0f, 0.0f) == 0.0f, "SmoothStep: at edge0 -> 0");
    Check(spot_attenuation::SmoothStep(0.0f, 1.0f, 1.0f) == 1.0f, "SmoothStep: at edge1 -> 1");
    Check(spot_attenuation::SmoothStep(0.0f, 1.0f, 0.5f) == 0.5f, "SmoothStep: midpoint -> 0.5 (Hermite symmetric)");

    // 中点确实是 Hermite 3t^2-2t^3：t=0.25 -> 0.15625
    CheckNear(spot_attenuation::SmoothStep(0.0f, 1.0f, 0.25f), 0.15625f, 1e-6f,
              "SmoothStep: t=0.25 -> 0.15625 (Hermite curve)");

    // --- SmoothStep: 钳制（超出 [edge0,edge1] 不越界）------------------------
    Check(spot_attenuation::SmoothStep(0.0f, 1.0f, -1.0f) == 0.0f, "SmoothStep: below edge0 clamps to 0");
    Check(spot_attenuation::SmoothStep(0.0f, 1.0f, 2.0f) == 1.0f, "SmoothStep: above edge1 clamps to 1");

    // --- SmoothStep: 退化（edge0 == edge1）退成硬阈值 ------------------------
    Check(spot_attenuation::SmoothStep(0.5f, 0.5f, 0.4f) == 0.0f, "SmoothStep: degenerate, x<edge -> 0");
    Check(spot_attenuation::SmoothStep(0.5f, 0.5f, 0.5f) == 1.0f, "SmoothStep: degenerate, x==edge -> 1");
    Check(spot_attenuation::SmoothStep(0.5f, 0.5f, 0.6f) == 1.0f, "SmoothStep: degenerate, x>edge -> 1");

    // --- ConeAttenuation: 锥内全亮 / 锥外为零 --------------------------------
    // 典型配置：inner=25° -> cos≈0.9063, outer=45° -> cos≈0.7071
    const float inner = 0.9063f;
    const float outer = 0.7071f;
    Check(spot_attenuation::ConeAttenuation(1.0f, inner, outer) == 1.0f,
          "ConeAttenuation: on axis (cos=1) -> full brightness");
    Check(spot_attenuation::ConeAttenuation(inner, inner, outer) == 1.0f,
          "ConeAttenuation: at inner cone edge -> full brightness");
    Check(spot_attenuation::ConeAttenuation(outer, inner, outer) == 0.0f,
          "ConeAttenuation: at outer cone edge -> zero");
    Check(spot_attenuation::ConeAttenuation(0.0f, inner, outer) == 0.0f,
          "ConeAttenuation: far outside cone (cos=0) -> zero");

    // --- ConeAttenuation: 中间平滑（介于内外锥之间取 (0,1) 开区间）-----------
    {
        const float mid = 0.5f * (inner + outer);
        const float f = spot_attenuation::ConeAttenuation(mid, inner, outer);
        Check(f > 0.0f && f < 1.0f, "ConeAttenuation: between cones -> smooth (0,1)");
        // 对称中点应接近 0.5（cos 空间近似线性，Hermite 中点=0.5）
        CheckNear(f, 0.5f, 1e-3f, "ConeAttenuation: cone midpoint ~= 0.5");
    }

    // --- ConeAttenuation: 单调递减（cos 越小越暗）----------------------------
    {
        float prev = 1.1f;
        bool monotonic = true;
        for (int i = 0; i <= 10; ++i)
        {
            const float cos_theta = 1.0f - 0.1f * i;  // 1.0 -> 0.0
            const float f = spot_attenuation::ConeAttenuation(cos_theta, inner, outer);
            if (f > prev + 1e-6f)
                monotonic = false;
            prev = f;
        }
        Check(monotonic, "ConeAttenuation: monotonically non-increasing as cos_theta drops");
    }

    // --- ConeAttenuation: 非法配置（内外颠倒）自动纠正，不产生负值 -----------
    {
        const float f = spot_attenuation::ConeAttenuation(0.8f, outer, inner);  // 故意颠倒
        Check(f >= 0.0f && f <= 1.0f, "ConeAttenuation: swapped inner/outer clamped to [0,1] (no negative)");
    }

    // --- ConeAttenuation: 无过渡带（inner==outer）退成硬阈值 -----------------
    {
        const float c = 0.8f;
        Check(spot_attenuation::ConeAttenuation(c, c, c) == 1.0f,
              "ConeAttenuation: inner==outer, cos==edge -> hard on");
        Check(spot_attenuation::ConeAttenuation(c - 0.01f, c, c) == 0.0f,
              "ConeAttenuation: inner==outer, cos<edge -> hard off");
    }

    // --- 端到端：点光行为不变（type=0 时 shader 不进锥角分支）----------------
    // 这里验证的是"若误用锥角函数于点光"不会破坏：cos=1（正对）应全亮。
    Check(spot_attenuation::ConeAttenuation(1.0f, 1.0f, 1.0f) == 1.0f,
          "sanity: full-alignment cone with no falloff band -> 1.0");

    if (g_Failures == 0)
    {
        std::printf("SpotLightSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("SpotLightSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
