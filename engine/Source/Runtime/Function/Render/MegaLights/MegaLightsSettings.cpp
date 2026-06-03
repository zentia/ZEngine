#include "MegaLightsSettings.h"

#include "MegaLightsDefinitions.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Console/ConsoleManager.h"

namespace MegaLights
{
namespace
{
    int g_enable {0};
    int g_temporal_denoise {1};
    int g_spatial_denoise {1};
    int g_num_samples_per_pixel {static_cast<int>(kDefaultSamplesPerPixel)};
    int g_ss_shadow_steps {static_cast<int>(kDefaultScreenSpaceShadowSteps)};
    int g_max_lights {static_cast<int>(kMaxLocalLights)};
    int g_spatial_radius {static_cast<int>(kDefaultSpatialRadius)};
    float g_temporal_blend {kDefaultTemporalBlend};
    float g_disocclusion_threshold {kDefaultDisocclusionThreshold};
    float g_spatial_depth_sigma {kDefaultSpatialDepthSigma};
    float g_spatial_normal_power {kDefaultSpatialNormalPower};
}  // namespace

bool IsEnabled() { return g_enable != 0; }
bool IsTemporalDenoiseEnabled() { return g_temporal_denoise != 0; }
bool IsSpatialDenoiseEnabled() { return g_spatial_denoise != 0; }
int GetNumSamplesPerPixel() { return g_num_samples_per_pixel; }
int GetScreenSpaceShadowSteps() { return g_ss_shadow_steps; }
int GetMaxLights() { return g_max_lights; }
int GetSpatialRadius() { return g_spatial_radius; }
float GetTemporalBlend() { return g_temporal_blend; }
float GetDisocclusionThreshold() { return g_disocclusion_threshold; }
float GetSpatialDepthSigma() { return g_spatial_depth_sigma; }
float GetSpatialNormalPower() { return g_spatial_normal_power; }

void SetEnabled(bool enabled) { g_enable = enabled ? 1 : 0; }

void RegisterConsoleVariables()
{
    if (auto console = GET_SYSTEM(ConsoleManager))
    {
        console->RegisterIntVariable(
            "r.MegaLights.Enable",
            "Enable MegaLights stochastic deferred local lights (0=legacy point loop, 1=MegaLights).",
            0,
            &g_enable);
        console->RegisterIntVariable(
            "r.MegaLights.TemporalDenoise.Enable",
            "Enable temporal denoise for MegaLights stochastic direct lighting.",
            1,
            &g_temporal_denoise);
        console->RegisterIntVariable(
            "r.MegaLights.SpatialDenoise.Enable",
            "Enable spatial denoise pass for MegaLights direct lighting (runs after temporal).",
            1,
            &g_spatial_denoise);
        console->RegisterIntVariable(
            "r.MegaLights.NumSamplesPerPixel",
            "Stochastic light samples per pixel for MegaLights.",
            static_cast<int>(kDefaultSamplesPerPixel),
            &g_num_samples_per_pixel);
        console->RegisterIntVariable(
            "r.MegaLights.ScreenSpaceShadowSteps",
            "Screen-space shadow ray march steps for MegaLights.",
            static_cast<int>(kDefaultScreenSpaceShadowSteps),
            &g_ss_shadow_steps);
        console->RegisterIntVariable(
            "r.MegaLights.MaxLights",
            "Maximum local lights uploaded for MegaLights.",
            static_cast<int>(kMaxLocalLights),
            &g_max_lights);
        console->RegisterIntVariable(
            "r.MegaLights.SpatialDenoise.Radius",
            "Spatial filter radius in pixels (1=3x3, 2=5x5).",
            static_cast<int>(kDefaultSpatialRadius),
            &g_spatial_radius);
        console->RegisterFloatVariable(
            "r.MegaLights.TemporalDenoise.Blend",
            "History blend weight for MegaLights temporal denoise (0=current frame only, 1=history only).",
            kDefaultTemporalBlend,
            &g_temporal_blend);
        console->RegisterFloatVariable(
            "r.MegaLights.TemporalDenoise.DisocclusionThreshold",
            "UV reprojection threshold for MegaLights temporal history rejection.",
            kDefaultDisocclusionThreshold,
            &g_disocclusion_threshold);
        console->RegisterFloatVariable(
            "r.MegaLights.SpatialDenoise.DepthSigma",
            "Depth edge-stopping sigma for MegaLights spatial bilateral filter.",
            kDefaultSpatialDepthSigma,
            &g_spatial_depth_sigma);
        console->RegisterFloatVariable(
            "r.MegaLights.SpatialDenoise.NormalPower",
            "Normal edge-stopping exponent for MegaLights spatial bilateral filter.",
            kDefaultSpatialNormalPower,
            &g_spatial_normal_power);
    }
}

}  // namespace MegaLights
