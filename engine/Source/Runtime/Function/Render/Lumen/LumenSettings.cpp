#include "LumenSettings.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Console/ConsoleManager.h"

namespace LumenSettings
{
namespace
{
    // CVar 存储。初值取 LumenConfig 自己的默认值，这样"一个 CVar 都没动过"时
    // BuildConfig() 的结果和原来写死在 LumenRenderPass::Initialize 里的完全一致。
    const LumenConfig kDefaults {};

    int g_enable {kDefaults.enabled ? 1 : 0};
    int g_global_illumination {kDefaults.enable_global_illumination ? 1 : 0};
    int g_reflections {kDefaults.enable_reflections ? 1 : 0};
    int g_probe_budget {static_cast<int>(kDefaults.gi_probe_update_budget)};
    int g_reflection_budget {static_cast<int>(kDefaults.reflection_update_budget)};
    int g_sample_count {static_cast<int>(kDefaults.gi_sample_count)};
    float g_probe_spacing {kDefaults.gi_probe_spacing};
    float g_temporal_blend_rate {kDefaults.gi_temporal_blend_rate};
    // 间接光的输出强度。1.0 = 直接用 probe 网格算出来的辐照度。
    float g_indirect_intensity {1.0f};
}  // namespace

bool IsEnabled() { return g_enable != 0; }
bool IsGlobalIlluminationEnabled() { return g_enable != 0 && g_global_illumination != 0; }
bool IsReflectionsEnabled() { return g_enable != 0 && g_reflections != 0; }
float GetProbeSpacing() { return g_probe_spacing; }
uint32_t GetProbeUpdateBudget() { return static_cast<uint32_t>(std::max(g_probe_budget, 1)); }
uint32_t GetReflectionUpdateBudget() { return static_cast<uint32_t>(std::max(g_reflection_budget, 1)); }
float GetTemporalBlendRate() { return g_temporal_blend_rate; }
uint32_t GetSampleCount() { return static_cast<uint32_t>(std::max(g_sample_count, 1)); }
float GetIndirectIntensity() { return g_indirect_intensity; }

void ApplyTo(LumenConfig& out_config)
{
    out_config.enabled = g_enable != 0;
    out_config.enable_global_illumination = g_global_illumination != 0;
    out_config.enable_reflections = g_reflections != 0;
    out_config.gi_probe_update_budget = static_cast<uint32_t>(std::max(g_probe_budget, 1));
    out_config.gi_sample_count = static_cast<uint32_t>(std::max(g_sample_count, 1));
    out_config.gi_probe_spacing = std::max(g_probe_spacing, 0.05f);
    out_config.gi_temporal_blend_rate = g_temporal_blend_rate;
    // 反射走同一套预算 / 采样 / 收敛语义：r.Lumen.SampleCount 既决定每个 probe 打几条
    // 光线，也决定每个反射着色点在粗糙度锥里打几条。
    out_config.reflection_update_budget = static_cast<uint32_t>(std::max(g_reflection_budget, 1));
    out_config.reflection_sample_count = static_cast<uint32_t>(std::max(g_sample_count, 1));
    out_config.reflection_temporal_blend_rate = g_temporal_blend_rate;
}

LumenConfig BuildConfig()
{
    LumenConfig config {};
    ApplyTo(config);
    return config;
}

void RegisterConsoleVariables(ConsoleManager& console)
{
    console.RegisterIntVariable("r.Lumen.Enable",
                                "Enable Lumen (software distance-field GI + reflections). 0=off, 1=on.",
                                g_enable,
                                &g_enable);
    console.RegisterIntVariable("r.Lumen.GlobalIllumination",
                                "Enable the Lumen irradiance probe grid. Requires r.Lumen.Enable.",
                                g_global_illumination,
                                &g_global_illumination);
    console.RegisterIntVariable("r.Lumen.Reflections",
                                "Enable Lumen distance-field reflections. Requires r.Lumen.Enable.",
                                g_reflections,
                                &g_reflections);
    console.RegisterIntVariable("r.Lumen.ProbeBudget",
                                "How many irradiance probes may be re-traced per frame (the grid rotates).",
                                g_probe_budget,
                                &g_probe_budget);
    console.RegisterIntVariable("r.Lumen.ReflectionBudget",
                                "How many surface-cache pages may have their reflection re-traced per "
                                "frame (the list rotates).",
                                g_reflection_budget,
                                &g_reflection_budget);
    console.RegisterIntVariable("r.Lumen.SampleCount",
                                "Rays traced per probe, and per reflection shading point (inside the "
                                "roughness cone). Higher = smoother, slower.",
                                g_sample_count,
                                &g_sample_count);
    console.RegisterFloatVariable("r.Lumen.ProbeSpacing",
                                  "Desired world-space spacing between irradiance probes. The per-axis "
                                  "resolution is clamped, so tiny values make the spacing larger instead.",
                                  g_probe_spacing,
                                  &g_probe_spacing);
    console.RegisterFloatVariable("r.Lumen.TemporalBlendRate",
                                  "Convergence rate per second for both probes and reflections. 0 "
                                  "freezes each sample on its first value; larger values track the "
                                  "scene faster but keep less history.",
                                  g_temporal_blend_rate,
                                  &g_temporal_blend_rate);
    console.RegisterFloatVariable("r.Lumen.IndirectIntensity",
                                  "Scale applied to the probe grid's irradiance before it reaches the "
                                  "deferred lighting pass.",
                                  g_indirect_intensity,
                                  &g_indirect_intensity);
}

}  // namespace LumenSettings
