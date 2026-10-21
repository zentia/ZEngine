#pragma once

#include "LumenTypes.h"

class ConsoleManager;

// Lumen 的 CVar 层（r.Lumen.*），照 MegaLightsSettings 的模式：CVar 直接写进文件内的
// int/float 存储，再用 ApplyTo 组装成 LumenConfig。
//
// 为什么要有这一层：LumenConfig 今天只活在 LumenRenderPass 里、写死在
// Initialize() 中，运行时没有任何办法开关 GI 或调 probe 密度。
//
// 这些 CVar 是**每帧生效**的（LumenRenderPass::Render 开头会重新 ApplyTo），
// 不要求重建渲染器。probe 网格本身对参数变化是安全的：分辨率变了
// ResizeProbeGrid 会重建并丢弃历史，没变就保留历史。
namespace LumenSettings
{
    // 开关
    bool IsEnabled();
    bool IsGlobalIlluminationEnabled();
    bool IsReflectionsEnabled();

    // 质量 / 性能
    float GetProbeSpacing();
    uint32_t GetProbeUpdateBudget();
    uint32_t GetReflectionUpdateBudget();
    float GetTemporalBlendRate();
    uint32_t GetSampleCount();
    float GetIndirectIntensity();

    // 把当前 CVar 值写进 config。只覆盖"可以热改"的字段 —— 结构性字段
    // （surface_cache_resolution / distance_field_resolution）仍然由
    // LumenRenderPass::Initialize 决定，因为改它们必须重建资源。
    void ApplyTo(LumenConfig& out_config);

    // 用当前 CVar 值组装一份完整配置
    LumenConfig BuildConfig();

    // 注册 r.Lumen.*（从 RegisterRuntimeConsoleCommands 调用）
    void RegisterConsoleVariables(ConsoleManager& console);
}  // namespace LumenSettings
