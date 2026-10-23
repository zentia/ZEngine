#include "Runtime/Function/Render/SsaoSettings.h"

#include "Runtime/Function/Console/ConsoleManager.h"

#include <algorithm>

namespace SsaoSettings
{
namespace
{
    // 默认值的来历：
    //   Radius 0.5m —— 引擎的世界单位是米（场景实体坐标量级 10~30），UE 的
    //     AmbientOcclusionRadius 默认 200cm，URP 的 0.035 是视空间单位不能照搬。
    //     0.5m 能把墙角 / 物体落地处压出来，又不至于把整面墙糊黑。
    //   Intensity 1.0、SampleCount 12、DirectLightingStrength 0.25 —— URP 默认档。
    int   g_enable {1};
    float g_radius {0.5f};
    float g_intensity {1.0f};
    float g_direct_lighting_strength {0.25f};
    int   g_sample_count {12};
    int   g_blur {1};
}  // namespace

bool  IsEnabled() { return g_enable != 0; }
float GetRadius() { return std::max(g_radius, 0.001f); }
float GetIntensity() { return std::max(g_intensity, 0.0f); }
float GetDirectLightingStrength() { return std::clamp(g_direct_lighting_strength, 0.0f, 1.0f); }
// 上限 32：MSL 那边的循环上限是 64，再多就是纯烧 GPU；下限 1：0 个采样会除 0。
uint32_t GetSampleCount() { return static_cast<uint32_t>(std::clamp(g_sample_count, 1, 32)); }
bool     IsBlurEnabled() { return g_blur != 0; }

void RegisterConsoleVariables(ConsoleManager& console)
{
    console.RegisterIntVariable("r.SSAO.Enable",
                                "Screen-space ambient occlusion (Apple/Metal deferred path). 0=off, 1=on. "
                                "Off binds a 1x1 white texture, so lighting is exactly the no-SSAO result.",
                                g_enable,
                                &g_enable);
    console.RegisterFloatVariable("r.SSAO.Radius",
                                  "SSAO sample hemisphere radius in world units (meters).",
                                  g_radius,
                                  &g_radius);
    console.RegisterFloatVariable("r.SSAO.Intensity",
                                  "SSAO strength (applied before the contrast curve).",
                                  g_intensity,
                                  &g_intensity);
    console.RegisterFloatVariable("r.SSAO.DirectLightingStrength",
                                  "How much of the occlusion also darkens direct lighting. 0 = indirect only "
                                  "(UE behaviour), 1 = full (URP 'After Opaque' look). Default 0.25 (URP).",
                                  g_direct_lighting_strength,
                                  &g_direct_lighting_strength);
    console.RegisterIntVariable("r.SSAO.SampleCount",
                                "SSAO samples per pixel (clamped to 1..32).",
                                g_sample_count,
                                &g_sample_count);
    console.RegisterIntVariable("r.SSAO.Blur",
                                "Two-pass depth/normal-aware blur of the raw SSAO. 0 = raw noisy result (debug).",
                                g_blur,
                                &g_blur);
}

}  // namespace SsaoSettings
