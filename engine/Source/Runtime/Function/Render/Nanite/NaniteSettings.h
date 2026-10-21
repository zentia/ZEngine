#pragma once

#include "NaniteTypes.h"

class ConsoleManager;

// Nanite 的 CVar 层（r.Nanite.*），照 LumenSettings 的模式：CVar 直接写进文件内的
// int/float 存储，再用 ApplyTo 组装成 NaniteConfig。
//
// 为什么接进帧循环的同时就要这一层：UpdateFromScene 在**场景拓扑变化**时会重建集群
// （递归二分整个网格），而 Nanite 今天还没有 GPU 路径 —— 没有 shader、没有间接绘制，
// 批次产出来也没人消费。没有开关的话，接线就等于给每个项目无条件加一份纯 CPU 开销。
//
// 这些 CVar 是**每帧生效**的（NaniteRenderPass::Render 开头会重新 ApplyTo）。
namespace NaniteSettings
{
    // 开关
    bool IsEnabled();
    bool IsLodEnabled();
    bool IsCullingEnabled();

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
