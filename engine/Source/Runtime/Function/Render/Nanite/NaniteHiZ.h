#pragma once

// ============================================================================
// NaniteHiZ — 层次 Z 缓冲（HiZ）的纯 CPU 算术核心（无引擎依赖，header-only）
// ----------------------------------------------------------------------------
// 把"从一帧深度图构建 mip 金字塔 + 按屏幕区域采样 + 判遮挡"抽成不依赖任何引擎
// 类型的 inline 函数，与 EnhancedInputSystem::ComputeFrameDelta / GasCooldown.h
// 同一手法：核心是纯函数，可脱离 RHI/窗口/Nanite 单测（NaniteHiZSmokeTest）；
// NaniteHierarchicalZBuffer 只做薄封装（存金字塔 + 把 AABB 投影到屏幕矩形）。
//
// 深度约定（关键，测试据此钉死）：
//   - 深度值是 **reversed-Z**：1.0 = 最近（相机近平面）、0.0 = 最远。现代引擎的
//     深度缓冲普遍如此（精度分配更合理）。
//   - 金字塔用 **max 归约**：每个较粗的 texel 存它覆盖的 2x2 里**最大**的深度值
//     （= 最近的那个表面）。于是"这块区域里有没有比某物更近的东西"一次采样即知。
//
// 遮挡判据（保守：拿不准就绝不剔除）：
//   物体被遮挡 ⟺ 物体最近深度 < 区域内金字塔的最大深度。
//   即：区域里**所有**深度都比物体近 ⇒ 物体藏在它们后面 ⇒ 可安全剔除。
//   只要区域里有一个 texel 的深度 ≤ 物体最近深度（更远或相等），就认为可见，
//   不剔除——这是保守遮挡剔除的正确性底线（误剔 = 画面破洞，比不剔更糟）。
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nanite_hiz
{
    // 层次 Z 金字塔。mip 0 = 基础分辨率（baseW x baseH）；mip L 尺寸向上取整
    // 除 2^L。data 平铺：mip0 全部 texel 在前，其后是 mip1……每个 texel 是该区域
    // 的最大深度（reversed-Z）。
    struct HiZPyramid
    {
        uint32_t baseW {0};
        uint32_t baseH {0};
        int      numMips {0};

        // 每个 mip 的宽度 / 高度（texel 数）。
        std::vector<uint32_t> mipW;
        std::vector<uint32_t> mipH;
        // mip[i] 在 data 里的起始 texel 索引。
        std::vector<uint32_t> mipOffset;
        // 平铺的金字塔数据（max 归约深度）。
        std::vector<float>    data;

        bool Valid() const { return baseW > 0 && baseH > 0 && numMips > 0 && data.size() > 0; }

        void Reset()
        {
            baseW = baseH = 0;
            numMips = 0;
            mipW.clear();
            mipH.clear();
            mipOffset.clear();
            data.clear();
        }

        // 取 (tx, ty) 处、mip 级别 level 的 texel 深度（越界返回 0 = 最远，保守）。
        float Texel(int level, uint32_t tx, uint32_t ty) const
        {
            if (level < 0 || level >= numMips) return 0.0f;
            const uint32_t w = mipW[level];
            const uint32_t h = mipH[level];
            if (tx >= w || ty >= h) return 0.0f;
            return data[mipOffset[level] + ty * w + tx];
        }
    };

    // 从一帧深度图（reversed-Z，行主序，长度 baseW*baseH）构建 max 归约金字塔。
    // depth 可为空（此时金字塔全 0 = 全远，任何物体都不被遮挡）。
    inline HiZPyramid BuildHiZPyramid(uint32_t baseW, uint32_t baseH, const float* depth)
    {
        HiZPyramid py;
        if (baseW == 0 || baseH == 0)
            return py;

        // mip 数 = floor(log2(maxDim)) + 1（一直降到 1x1）。用整数右移数位数，
        // 避免浮点 log2 的不确定性：maxDim>>n 仍 > 0 就还要再降一级。
        const uint32_t maxDim = std::max(baseW, baseH);
        int numMips = 1;
        while ((maxDim >> numMips) > 0u && numMips < 32)
            ++numMips;
        py.numMips = numMips;
        py.baseW = baseW;
        py.baseH = baseH;

        py.mipW.resize(numMips);
        py.mipH.resize(numMips);
        py.mipOffset.resize(numMips);

        uint32_t offset = 0;
        for (int level = 0; level < numMips; ++level)
        {
            const uint32_t w = (baseW >> level) ? (baseW >> level) : 1u;
            const uint32_t h = (baseH >> level) ? (baseH >> level) : 1u;
            py.mipW[level] = w;
            py.mipH[level] = h;
            py.mipOffset[level] = offset;
            offset += w * h;
        }
        py.data.resize(offset, 0.0f);

        // mip 0：直接拷贝深度图。
        float* mip0 = &py.data[0];
        if (depth)
        {
            for (uint32_t i = 0; i < baseW * baseH; ++i)
                mip0[i] = depth[i];
        }

        // 较粗的 mip：对上一级的 2x2 取 max（reversed-Z 的"最近"= 最大值）。
        for (int level = 1; level < numMips; ++level)
        {
            const uint32_t dw = py.mipW[level];
            const uint32_t dh = py.mipH[level];
            const uint32_t sw = py.mipW[level - 1];
            const uint32_t sh = py.mipH[level - 1];
            float* dst = &py.data[py.mipOffset[level]];
            const float* src = &py.data[py.mipOffset[level - 1]];

            for (uint32_t y = 0; y < dh; ++y)
            {
                const uint32_t sy0 = y * 2u;
                const uint32_t sy1 = std::min(sy0 + 1u, sh - 1u);
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint32_t sx0 = x * 2u;
                    const uint32_t sx1 = std::min(sx0 + 1u, sw - 1u);
                    const float a = src[sy0 * sw + sx0];
                    const float b = src[sy0 * sw + sx1];
                    const float c = src[sy1 * sw + sx0];
                    const float d = src[sy1 * sw + sx1];
                    dst[y * dw + x] = std::max({a, b, c, d});
                }
            }
        }
        return py;
    }

    // 计算金字塔每级的 (width, height, offset)。与 BuildHiZPyramid 的 mip 数/尺寸
    // 推导**逐位一致**——GPU 侧（nanite_hiz.comp）据此填 header，保证 GLSL 与 C++
    // 对同一张深度图产出完全相同的平铺布局，逐 texel 比对才有意义。
    // 返回总 texel 数（= data.size()），并把每级写入 outMips[i] = {w, h, off, 0}。
    inline uint32_t HiZMipLayout(uint32_t baseW, uint32_t baseH, uint32_t outMips[16][4], int& outNumMips)
    {
        if (baseW == 0 || baseH == 0)
        {
            outNumMips = 0;
            return 0;
        }
        const uint32_t maxDim = std::max(baseW, baseH);
        int numMips = 1;
        while ((maxDim >> numMips) > 0u && numMips < 32)
            ++numMips;
        if (numMips > 16)
            numMips = 16;  // GPU header 只留 16 级（足够 65536×65536）。
        outNumMips = numMips;

        uint32_t offset = 0;
        for (int level = 0; level < numMips; ++level)
        {
            const uint32_t w = (baseW >> level) ? (baseW >> level) : 1u;
            const uint32_t h = (baseH >> level) ? (baseH >> level) : 1u;
            outMips[level][0] = w;
            outMips[level][1] = h;
            outMips[level][2] = offset;
            outMips[level][3] = 0;
            offset += w * h;
        }
        return offset;
    }

    // 一个屏幕矩形（基础像素坐标 [x0, x1) x [y0, y1)，左闭右开）该采哪个 mip：
    // 选使矩形在金字塔上覆盖约 1~4 个 texel 的那一级——矩形越大，mip 越粗。
    // 返回的 level 满足 (x0>>level, y0>>level) 到 (x1>>level, y1>>level) 覆盖矩形。
    inline int MipLevelForRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, int numMips)
    {
        if (x1 <= x0 || y1 <= y0 || numMips <= 1)
            return 0;
        const uint32_t rw = x1 - x0;
        const uint32_t rh = y1 - y0;
        const uint32_t maxSide = std::max(rw, rh);
        // 目标是让覆盖的金字塔 texel 数大致 <= 4（2x2）：level ≈ floor(log2(maxSide/2))。
        // 用整数迭代代替 log2，避免浮点不确定。
        int level = 0;
        uint32_t span = maxSide;
        while (span > 4u && level < numMips - 1)
        {
            span >>= 1u;
            ++level;
        }
        return level;
    }

    // 采样矩形区域内的**最大**深度（reversed-Z 的最近值）。用于遮挡判据。
    // 矩形在基础像素坐标 [x0, x1) x [y0, y1)（会 clamp 到金字塔范围）。
    inline float SampleMaxDepth(const HiZPyramid& py, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        if (!py.Valid() || x1 <= x0 || y1 <= y0)
            return 0.0f;  // 空区域 / 无效 → 0（最远），保守：不遮挡。

        x0 = std::min(x0, py.baseW);
        y0 = std::min(y0, py.baseH);
        x1 = std::min(x1, py.baseW);
        y1 = std::min(y1, py.baseH);
        if (x1 <= x0 || y1 <= y0)
            return 0.0f;

        const int level = MipLevelForRect(x0, y0, x1, y1, py.numMips);
        const uint32_t div = 1u << static_cast<uint32_t>(level);  // 2^level
        // 金字塔 texel 索引区间（覆盖整个矩形，含末端）。
        const uint32_t tx0 = (x0 + div - 1u) / div;  // ceil(x0 / 2^level)
        const uint32_t ty0 = (y0 + div - 1u) / div;
        const uint32_t tx1 = (x1 - 1u) / div;        // floor((x1-1) / 2^level)
        const uint32_t ty1 = (y1 - 1u) / div;

        float maxDepth = 0.0f;
        for (uint32_t ty = ty0; ty <= ty1; ++ty)
            for (uint32_t tx = tx0; tx <= tx1; ++tx)
                maxDepth = std::max(maxDepth, py.Texel(level, tx, ty));
        return maxDepth;
    }

    // 遮挡判据：物体（其最近深度 objectNearestDepth，reversed-Z）在屏幕矩形区域
    // 内是否被完全遮挡。true = 被遮挡（可安全剔除）。
    //   物体最近深度 < 区域内最大深度（所有 texel 都比物体近）⇒ 遮挡。
    //   否则（有 texel 更远或相等）⇒ 可见，不剔除（保守）。
    inline bool TestOccluded(const HiZPyramid& py, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                             float objectNearestDepth)
    {
        const float regionMax = SampleMaxDepth(py, x0, y0, x1, y1);
        // 区域最大深度为 0（空区域 / 全远）时不遮挡。
        if (regionMax <= 0.0f)
            return false;
        return objectNearestDepth < regionMax;
    }
}  // namespace nanite_hiz
