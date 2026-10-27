// ============================================================================
// NaniteHiZSmokeTest — HiZ（层次 Z 缓冲）纯 CPU 核心（NaniteHiZ.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 nanite_hiz 命名空间里的纯函数（与 EnhancedInput/GasCooldown 同一手法：核心
// 是纯函数，脱离 RHI/窗口/Nanite 单测）：
//   - BuildHiZPyramid：从深度图构建 max 归约金字塔（单 texel / 2x2 / 多 mip / 空输入）。
//   - MipLevelForRect：屏幕矩形 → 该采的 mip 级别。
//   - SampleMaxDepth：矩形区域内的最大深度（reversed-Z 的最近值）。
//   - TestOccluded：遮挡判据（物体被完全近物遮挡 / 有缝隙可见 / 空区域 / 无效金字塔）。
//
// 深度约定（测试据此钉死）：reversed-Z —— 1.0 = 最近、0.0 = 最远。金字塔 max 归约：
// 每个粗 texel 存覆盖 2x2 里的最大深度（最近表面）。遮挡判据：物体最近深度 < 区域内
// 最大深度 ⇒ 被遮挡（区域内所有东西都比物体近）。
//
// 为什么只测纯核心、不测 NaniteHierarchicalZBuffer：遮挡判据与金字塔构建的全部语义
// 风险都在 nanite_hiz 里；NaniteHierarchicalZBuffer 只做"AABB 投影到屏幕矩形"的薄
// 封装（那部分由矩阵数学保证，且其投影正确性在 NaniteCullingSmokeTest 已覆盖）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_NANITE_HIZ_SMOKE_TEST=ON
//   cmake --build build --target NaniteHiZSmokeTest
// ============================================================================

#include "Runtime/Function/Render/Nanite/NaniteHiZ.h"

#include <cstdio>
#include <vector>

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

    // 填一份 W x H 的平面深度图（所有 texel 同值），reversed-Z。
    std::vector<float> FlatDepth(uint32_t w, uint32_t h, float d)
    {
        std::vector<float> out(w * h, d);
        return out;
    }
}  // namespace

int main()
{
    std::printf("NaniteHiZSmokeTest (pure CPU pyramid core)\n");

    // --- BuildHiZPyramid：尺寸 / mip 数 ---------------------------------------
    {
        auto d = FlatDepth(4, 4, 0.5f);
        const auto py = nanite_hiz::BuildHiZPyramid(4, 4, d.data());
        Check(py.Valid(), "pyramid: valid after build (4x4)");
        Check(py.baseW == 4 && py.baseH == 4, "pyramid: base dims stored");
        // 4x4 -> mip0 4x4, mip1 2x2, mip2 1x1 => 3 mips。
        Check(py.numMips == 3, "pyramid: 4x4 yields 3 mip levels (4/2/1)");
        Check(py.mipW[0] == 4 && py.mipH[0] == 4 && py.mipW[1] == 2 && py.mipH[1] == 2 &&
                  py.mipW[2] == 1 && py.mipH[2] == 1,
              "pyramid: mip dims halve each level");
    }

    // --- BuildHiZPyramid：max 归约正确性 --------------------------------------
    {
        // 4x4 全 0.1，左上 2x2 是 0.9（更近）。mip1 的左上 texel 应取到 0.9。
        std::vector<float> d(16, 0.1f);
        d[0] = d[1] = d[4] = d[5] = 0.9f;
        const auto py = nanite_hiz::BuildHiZPyramid(4, 4, d.data());
        Check(py.Texel(1, 0, 0) == 0.9f, "pyramid: mip1 top-left = max of near corner (0.9)");
        // mip1 的右上 texel 覆盖原图右上 2x2（全 0.1）。
        Check(py.Texel(1, 1, 0) == 0.1f, "pyramid: mip1 top-right = max of far corner (0.1)");
        // mip2 (1x1) 是全局最大 = 0.9。
        Check(py.Texel(2, 0, 0) == 0.9f, "pyramid: mip2 single texel = global max (0.9)");
    }

    // --- BuildHiZPyramid：空输入 → 全 0（全远），任何物体都不被遮挡 -------------
    {
        const auto py = nanite_hiz::BuildHiZPyramid(2, 2, nullptr);
        Check(py.Valid(), "pyramid: still valid with null depth");
        Check(py.Texel(0, 0, 0) == 0.0f && py.Texel(1, 0, 0) == 0.0f,
              "pyramid: null depth -> all mips zero (far)");
    }

    // --- MipLevelForRect：矩形大小 → level ------------------------------------
    {
        Check(nanite_hiz::MipLevelForRect(0, 0, 4, 4, 3) == 0, "mip: small rect (4px) -> level 0");
        Check(nanite_hiz::MipLevelForRect(0, 0, 8, 8, 3) == 1, "mip: 8px rect -> level 1");
        Check(nanite_hiz::MipLevelForRect(0, 0, 16, 16, 3) == 2, "mip: 16px rect -> level 2");
        Check(nanite_hiz::MipLevelForRect(0, 0, 1, 1, 3) == 0, "mip: degenerate 1px rect -> level 0");
        Check(nanite_hiz::MipLevelForRect(0, 0, 0, 4, 3) == 0, "mip: empty rect -> level 0");
    }

    // --- SampleMaxDepth：区域采样 ---------------------------------------------
    {
        // 8x8 全 0.2，左上 4x4 是 0.8（更近）。采左上 4x4 应得 0.8。
        std::vector<float> d(64, 0.2f);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                d[y * 8 + x] = 0.8f;
        const auto py = nanite_hiz::BuildHiZPyramid(8, 8, d.data());
        Check(nanite_hiz::SampleMaxDepth(py, 0, 0, 4, 4) == 0.8f,
              "sample: top-left quarter -> max 0.8");
        Check(nanite_hiz::SampleMaxDepth(py, 4, 4, 8, 8) == 0.2f,
              "sample: bottom-right quarter -> max 0.2");
        Check(nanite_hiz::SampleMaxDepth(py, 0, 0, 8, 8) == 0.8f,
              "sample: full rect -> global max 0.8");
        Check(nanite_hiz::SampleMaxDepth(py, 0, 0, 0, 0) == 0.0f,
              "sample: empty rect -> 0 (far)");
    }

    // --- TestOccluded：遮挡判据 -----------------------------------------------
    // 注意 reversed-Z 语义：深度值越小 = 越远。区域最大深度 = 区域内最近表面。
    {
        // 全屏都是 0.5。
        const auto py = nanite_hiz::BuildHiZPyramid(4, 4, FlatDepth(4, 4, 0.5f).data());

        // 物体深度 0.1（比区域 0.5 更远）⇒ 藏在区域表面后面 ⇒ 被遮挡。
        Check(nanite_hiz::TestOccluded(py, 0, 0, 4, 4, 0.1f),
              "occlusion: object farther (0.1) than region (0.5) -> occluded");

        // 物体深度 0.5（等于区域最大）⇒ 有相等 ⇒ 可见（保守，不剔除）。
        Check(!nanite_hiz::TestOccluded(py, 0, 0, 4, 4, 0.5f),
              "occlusion: object equal to region max -> visible (conservative)");

        // 物体深度 0.9（比区域 0.5 更近）⇒ 物体在前面 ⇒ 可见（不遮挡）。
        Check(!nanite_hiz::TestOccluded(py, 0, 0, 4, 4, 0.9f),
              "occlusion: object nearer (0.9) than region (0.5) -> visible");
    }

    // --- TestOccluded：部分近物遮挡 -------------------------------------------
    {
        // 左半屏 0.9（reversed-Z：很近），右半屏 0.1（很远）。物体深度 0.5。
        std::vector<float> d(16, 0.1f);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 2; ++x)
                d[y * 4 + x] = 0.9f;
        const auto py = nanite_hiz::BuildHiZPyramid(4, 4, d.data());

        // 采左半屏（全 0.9，很近）：物体 0.5 比 0.9 更远 ⇒ 藏在近物后 ⇒ 遮挡。
        Check(nanite_hiz::TestOccluded(py, 0, 0, 2, 4, 0.5f),
              "occlusion: object 0.5 vs near region 0.9 -> occluded");

        // 采右半屏（全 0.1，很远）：物体 0.5 比 0.1 更近 ⇒ 在前面 ⇒ 可见。
        Check(!nanite_hiz::TestOccluded(py, 2, 0, 4, 4, 0.5f),
              "occlusion: object 0.5 vs far region 0.1 -> visible (object in front)");

        // 采左半屏，物体 0.9 等于区域 0.9 ⇒ 有相等 ⇒ 可见（保守）。
        Check(!nanite_hiz::TestOccluded(py, 0, 0, 2, 4, 0.9f),
              "occlusion: object 0.9 equal to near region 0.9 -> visible (conservative)");
    }

    // --- TestOccluded：空区域 / 无效金字塔 ⇒ 不遮挡 ----------------------------
    {
        const auto py = nanite_hiz::BuildHiZPyramid(4, 4, FlatDepth(4, 4, 0.5f).data());
        Check(!nanite_hiz::TestOccluded(py, 4, 4, 8, 8, 0.9f),
              "occlusion: out-of-bounds rect -> not occluded (empty region)");

        nanite_hiz::HiZPyramid empty;
        Check(!nanite_hiz::TestOccluded(empty, 0, 0, 4, 4, 0.9f),
              "occlusion: invalid pyramid -> not occluded");
    }

    if (g_Failures == 0)
    {
        std::printf("NaniteHiZSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("NaniteHiZSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
