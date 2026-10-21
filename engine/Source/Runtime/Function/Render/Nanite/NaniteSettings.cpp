#include "NaniteSettings.h"

#include "Runtime/Function/Console/ConsoleManager.h"

#include <algorithm>

namespace NaniteSettings
{
namespace
{
    // CVar 存储。初值取 NaniteConfig 自己的默认值，这样"一个 CVar 都没动过"时
    // BuildConfig() 的结果和 NaniteConfig 的默认构造完全一致。
    const NaniteConfig kDefaults {};

    int g_enable {kDefaults.enabled ? 1 : 0};
    int g_lod {kDefaults.enable_lod ? 1 : 0};
    int g_culling {kDefaults.enable_culling ? 1 : 0};
    int g_gpu {kDefaults.use_gpu_culling ? 1 : 0};
    float g_max_screen_error {kDefaults.max_screen_error};
    float g_lod_bias {kDefaults.lod_bias};
}  // namespace

bool IsEnabled() { return g_enable != 0; }
bool IsLodEnabled() { return g_enable != 0 && g_lod != 0; }
bool IsCullingEnabled() { return g_enable != 0 && g_culling != 0; }
bool IsGPUEnabled() { return g_enable != 0 && g_gpu != 0; }
float GetMaxScreenError() { return g_max_screen_error; }
float GetLodBias() { return g_lod_bias; }

void ApplyTo(NaniteConfig& out_config)
{
    out_config.enabled = g_enable != 0;
    out_config.enable_lod = g_lod != 0;
    out_config.enable_culling = g_culling != 0;
    out_config.use_gpu_culling = g_gpu != 0;
    // 0 像素的误差预算意味着"永远选最细的一档"，那等于关掉 LOD 却还留着它的开销，
    // 所以这里给一个下限。
    out_config.max_screen_error = std::max(g_max_screen_error, 0.01f);
    out_config.lod_bias = g_lod_bias;
}

NaniteConfig BuildConfig()
{
    NaniteConfig config {};
    ApplyTo(config);
    return config;
}

void RegisterConsoleVariables(ConsoleManager& console)
{
    console.RegisterIntVariable("r.Nanite.Enable",
                                "Enable Nanite (cluster build + per-cluster culling + render batches). "
                                "The GPU rasterisation path does not exist yet, so this currently only "
                                "costs CPU time. 0=off, 1=on.",
                                g_enable,
                                &g_enable);
    console.RegisterIntVariable("r.Nanite.LOD",
                                "Let Nanite drop clusters whose projected error is under budget. "
                                "Requires r.Nanite.Enable.",
                                g_lod,
                                &g_lod);
    console.RegisterIntVariable("r.Nanite.Culling",
                                "Enable per-cluster frustum + LOD culling. Requires r.Nanite.Enable.",
                                g_culling,
                                &g_culling);
    console.RegisterIntVariable("r.Nanite.GPU",
                                "Run the per-cluster culling on the GPU (nanite_cluster_cull.comp) and "
                                "draw the result with a single CmdDrawIndexedIndirect, instead of doing "
                                "it all on the CPU. Requires r.Nanite.Enable. When the compute shader is "
                                "unavailable this silently falls back to the CPU path.",
                                g_gpu,
                                &g_gpu);
    console.RegisterFloatVariable("r.Nanite.MaxScreenError",
                                  "Screen-space error budget (pixels) a cluster may have before it is "
                                  "dropped: larger = coarser and cheaper.",
                                  g_max_screen_error,
                                  &g_max_screen_error);
    console.RegisterFloatVariable("r.Nanite.LodBias",
                                  "Bias added to the projected cluster error. Positive = coarser.",
                                  g_lod_bias,
                                  &g_lod_bias);
}

}  // namespace NaniteSettings
