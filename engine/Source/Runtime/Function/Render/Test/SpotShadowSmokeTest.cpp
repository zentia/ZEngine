// ============================================================================
// SpotShadowSmokeTest — 聚光灯阴影投影纯标量核心（SpotShadow.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 spot_shadow 命名空间里的纯标量函数：
//   - ShadowFovYHalfAngle / ShadowFovY：阴影透视锥覆盖整个光锥（fov=2*outer_cone），
//     负角归零、超半圆钳到 pi/2（透视投影物理上限）。
//   - ShadowNear：贴光源的极小近面（避免 z=0 投影退化）。
//   - ShadowFar：等于光衰减半径；<=0 退成极小正值（避免 znear==zfar）。
//   - ShadowAspect：正方形（锥圆形对称）。
//
// 这是阶段 2（聚光单面 2D 阴影）里唯一能 headless 验证的部分。完整的 view_proj
// 矩阵组装在引擎侧用 Math::MakePerspectiveMatrix + MakeLookAtMatrix 完成（拖入整个
// Math 链，不适合 headless）。真正的阴影 pass（建 2D depth、画深度、shader 采样）
// 是 GPU/真机绑定，本机无法验证——见 SPOTLIGHT_IMPLEMENTATION_PLAN.md 阶段 2 边界。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_SPOT_SHADOW_SMOKE_TEST=ON
//   cmake --build build --target SpotShadowSmokeTest
// ============================================================================

#include "Runtime/Function/Render/SpotShadow.h"

#include <cmath>
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

    constexpr float kPi = 3.14159265f;
    constexpr float kHalfPi = 1.5707963f;
}  // namespace

int main()
{
    std::printf("SpotShadowSmokeTest (pure spot-shadow projection scalars)\n");

    // --- ShadowFovYHalfAngle: 覆盖整个光锥 -----------------------------------
    CheckNear(spot_shadow::ShadowFovYHalfAngle(0.7854f), 0.7854f, 1e-6f,
              "ShadowFovYHalfAngle: 45° outer cone -> same half-angle (no change)");

    // --- ShadowFovY: 完整 fov = 2 * outer_cone -------------------------------
    CheckNear(spot_shadow::ShadowFovY(0.7854f), 1.5708f, 1e-4f,
              "ShadowFovY: 45° outer cone -> 90° vertical fov (covers full cone)");
    CheckNear(spot_shadow::ShadowFovY(0.4363f), 0.8726f, 1e-4f,
              "ShadowFovY: 25° outer cone -> 50° vertical fov");

    // --- ShadowFovYHalfAngle: 负角归零 ---------------------------------------
    Check(spot_shadow::ShadowFovYHalfAngle(-0.5f) == 0.0f,
          "ShadowFovYHalfAngle: negative angle clamps to 0");
    Check(spot_shadow::ShadowFovY(0.0f) == 0.0f,
          "ShadowFovY: zero outer cone -> zero fov (degenerate, no shadow frustum)");

    // --- ShadowFovYHalfAngle: 超半圆钳到 pi/2（透视投影物理上限 fov=180°）----
    CheckNear(spot_shadow::ShadowFovYHalfAngle(2.0f), kHalfPi, 1e-5f,
              "ShadowFovYHalfAngle: >90° clamps to pi/2 (perspective max)");
    CheckNear(spot_shadow::ShadowFovY(2.0f), kPi, 1e-5f,
              "ShadowFovY: huge outer cone -> 180° fov (perspective limit)");

    // --- ShadowNear: 贴光源的极小近面 ----------------------------------------
    Check(spot_shadow::ShadowNear() > 0.0f && spot_shadow::ShadowNear() < 0.1f,
          "ShadowNear: tiny positive (avoids z=0 projection degeneracy)");

    // --- ShadowFar: 等于光衰减半径 -------------------------------------------
    CheckNear(spot_shadow::ShadowFar(8.0f), 8.0f, 1e-6f,
              "ShadowFar: normal range passes through (shadow reaches light radius)");

    // --- ShadowFar: 非正/极小范围退成极小正值（避免 znear==zfar）-------------
    Check(spot_shadow::ShadowFar(0.0f) > 0.0f,
          "ShadowFar: zero range -> tiny positive (no znear==zfar degeneracy)");
    Check(spot_shadow::ShadowFar(-5.0f) > 0.0f,
          "ShadowFar: negative range -> tiny positive");
    Check(spot_shadow::ShadowFar(0.001f) > spot_shadow::ShadowNear(),
          "ShadowFar: sub-near range still exceeds near plane (valid frustum)");

    // --- ShadowAspect: 正方形 -------------------------------------------------
    CheckNear(spot_shadow::ShadowAspect(), 1.0f, 1e-6f,
              "ShadowAspect: square map (circularly symmetric cone)");

    // --- 端到端：一个典型聚光（25° 内 / 45° 外，8m 范围）的阴影锥参数 ---------
    {
        const float outer = 0.7854f;  // 45°
        const float fovy = spot_shadow::ShadowFovY(outer);
        const float near_z = spot_shadow::ShadowNear();
        const float far_z = spot_shadow::ShadowFar(8.0f);
        // fov 应在 (0, pi) 开区间，near < far，构成合法透视锥
        Check(fovy > 0.0f && fovy < kPi, "end-to-end: fov in (0, pi) -> valid perspective frustum");
        Check(near_z < far_z, "end-to-end: near < far -> valid depth range");
        // 90° fov 的锥在 8m 处的半宽 = 8 * tan(45°) = 8m，正好覆盖光锥半径
        CheckNear(far_z * tanf(fovy * 0.5f), 8.0f, 1e-3f,
                  "end-to-end: frustum half-width at far == light range (cone fully covered)");
    }

    if (g_Failures == 0)
    {
        std::printf("SpotShadowSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("SpotShadowSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
