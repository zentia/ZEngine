#pragma once

#include <cstdint>

// 一张 RenderTexture 的**描述**（Unity 的 `RenderTextureDescriptor` 的前半）。
//
// ⚠️ 每个字段都必须有默认初始化：RenderTexture 里是 `RenderTextureDesc m_Desc;`
// 这种默认构造，没有默认值的 `int width;` 在默认初始化下是**不确定值** ——
// 一台机器上读成 0、另一台读成 0xCCCCCCCC，而"宽是负数"会一路传到 RHI 才炸。
//
// 与 Unity 的差异：没有 `volumeDepth` / `dimension` / `vrUsage` / `memorylessMode`
// —— 引擎目前只有 2D 的 RT，编一排字段出来会让"我设了 volumeDepth 于是以为
// 拿到 3D RT"这类错误成为可能。
struct RenderTextureDesc
{
    int      width {0};
    int      height {0};
    /// MSAA 采样数（1 = 不开抗锯齿）。
    int      antiAliasing {1};
    /// 深度缓冲位数：0 / 16 / 24。0 = 不要深度缓冲（纯颜色 RT，如后处理中间
    /// 结果）。
    int      depth {0};
    /// RHIFormat 的**序号**（与 `Texture2D::m_Format` 同一套编号，不是 Unity
    /// 的 RenderTextureFormat）。0 表示"未指定"，由消费方挑默认。
    uint32_t format {0};
    /// 是否带 mip 链。
    bool     useMipMap {false};
    /// 是否自动生成 mip（要求 useMipMap 为真才有意义）。
    bool     autoGenerateMips {false};
};
