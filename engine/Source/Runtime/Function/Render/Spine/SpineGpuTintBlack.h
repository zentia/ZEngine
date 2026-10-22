#pragma once

// =====================================================================
// GPU-Spine：tint black（双色 / 暗色）的绘制参数
// =====================================================================
//
// 参考实现（OSGame 引擎分支，3.8）：
//   * 网格生成 `SpineMesh.h:202-205`：`tintBlack` 打开时，每个顶点带
//     `uv2 = (dark.r, dark.g)`、`uv3 = (dark.b, 1)` —— 取的是**槽位当前** `getDarkColor()`，
//     不看 `hasDarkColor`（没有暗色的槽位初值就是 (0,0,0,0) ⇒ 项退化为 0）。
//     与顶点色不同，暗色走的是 **float UV 通道，不截断、不量化**。
//   * 片元（Spine-SkeletonGraphic-TintBlack.shader / Spine-Skeleton-TintBlack.shader 同一式）：
//       out = tex * vc + vec4((1 - tex.rgb) * (_Black.rgb + dark) * tex.a * _Color.a * vc.a, 0)
//     tex 已预乘（直 alpha 图集先 `rgb *= a`），vc 是 PMA 顶点色 (rgb·a, a)。
//     加色槽位的顶点色 alpha 写 0（`SpineMesh.h` pmaVertexColors 分支）⇒ **加色项没有暗色**。
//
// ⚠️ OSGame 现状（2026-10-23 实测）：所有 prefab / 场景的 `tintBlack` 都是 0（3012 处），
//    而且这个引擎分支的 `MeshGenerator::FillVertexData()` **根本不上传 uv2 / uv3**
//    （只 SetVertices / SetUv(0) / SetColors，SpineMesh.cpp:131-143）—— 线上画面**没有**逐槽位暗色。
//    所以渲染器默认关（与 OSGame 画面一致），`spine.tintblack 1` 打开后才按上式画。
//
// 暗色可能是**负数**：TwoColor 时间轴在首帧之前 + MixBlend_First 时参考实现符号写反
// （SpineGpuSkeletonState.h 文件头第 4 条），算出 dark = -setup。参考把它原样塞进 float UV，
// 片元里就是"减亮"；这里同样**不截断**，以保持与参考一致。

#include <cstdint>

struct SpineGpuTintBlackParams
{
    bool enabled = false;   // 这一项要不要在片元里加暗色项
    float dark[3] = {0.0f, 0.0f, 0.0f};
};

// `tint_black_on`：渲染器开关（= SpineSettings::tintBlack）。
// `additive`：槽位混合模式是加色 ⇒ 顶点色 alpha = 0 ⇒ 暗色项恒为 0，直接不开。
// 暗色三分量全 0 ⇒ 暗色项恒为 0（`(1 - t) * 0 * … = 0`，加 0 不改任何位）⇒ 也不开，省一次分支。
inline SpineGpuTintBlackParams SpineGpuMakeTintBlackParams(const float slot_dark[4], bool tint_black_on, bool additive)
{
    SpineGpuTintBlackParams p;
    if (!tint_black_on || additive)
    {
        return p;
    }
    if (slot_dark[0] == 0.0f && slot_dark[1] == 0.0f && slot_dark[2] == 0.0f)
    {
        return p;
    }
    p.enabled = true;
    p.dark[0] = slot_dark[0];
    p.dark[1] = slot_dark[1];
    p.dark[2] = slot_dark[2];
    return p;
}

// 片元公式的 CPU 版（单测用，逐项对照 spine_gpu.frag）。输入 / 输出都是 [r, g, b, a]。
//   tex：图集采样值（**直 alpha**，与 shader 的 bit0 一致：先预乘）
//   tint：槽位色（直 alpha，已量化到 8 位后再还原的值由调用方决定）
inline void SpineGpuShadeReference(const float tex_straight[4], const float tint[4], bool additive,
                                   const SpineGpuTintBlackParams& tb, float out[4])
{
    const float ta = tex_straight[3];
    const float t[3] = {tex_straight[0] * ta, tex_straight[1] * ta, tex_straight[2] * ta};
    for (int k = 0; k < 3; ++k)
    {
        out[k] = t[k] * tint[k] * tint[3];
        if (tb.enabled)
        {
            out[k] += (1.0f - t[k]) * tb.dark[k] * ta * tint[3];
        }
    }
    out[3] = additive ? 0.0f : ta * tint[3];
}
