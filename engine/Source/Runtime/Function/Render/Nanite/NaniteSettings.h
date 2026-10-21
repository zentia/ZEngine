#pragma once

#include "NaniteTypes.h"

class ConsoleManager;

// Nanite 的 CVar 层（r.Nanite.*），照 LumenSettings 的模式：CVar 直接写进文件内的
// int/float 存储，再用 ApplyTo 组装成 NaniteConfig。
//
// 为什么接进帧循环的同时就要这一层：UpdateFromScene 在**场景拓扑变化**时会重建集群
// （递归二分整个网格），这是一份纯 CPU 开销，不是每个项目都想要。r.Nanite.GPU 决定
// 逐集群的裁剪是跑在 GPU 上（一次 dispatch + 一次间接绘制）还是 CPU 上。
//
// 这些 CVar 是**每帧生效**的（NaniteRenderPass::Render 开头会重新 ApplyTo）。
namespace NaniteSettings
{
    // 开关
    bool IsEnabled();
    bool IsLodEnabled();
    bool IsCullingEnabled();
    // 逐集群裁剪跑在 GPU 上（compute 剔除 + 一次间接绘制），而不是 CPU。
    bool IsGPUEnabled();

    // 质量
    float GetMaxScreenError();
    float GetLodBias();

    // 把当前 CVar 值写进 config。只覆盖"可以热改"的字段。
    void ApplyTo(NaniteConfig& out_config);

    // 用当前 CVar 值组装一份完整配置
    NaniteConfig BuildConfig();

    // 注册 r.Nanite.*（从 RegisterRuntimeConsoleCommands 调用）
    void RegisterConsoleVariables(ConsoleManager& console);
}  // namespace NaniteSettings
