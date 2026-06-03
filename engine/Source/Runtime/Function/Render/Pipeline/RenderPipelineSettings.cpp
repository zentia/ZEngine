#include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"

#include "Runtime/Core/Base/Platform.h"
#include "Runtime/Function/Console/ConsoleManager.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace RenderPipelineSettings
{
namespace
{
    int g_render_path {static_cast<int>(RenderPath::Auto)};
    std::atomic<bool> g_path_dirty {false};

    RenderPath ClampToPath(int value)
    {
        switch (value)
        {
            case static_cast<int>(RenderPath::Desktop): return RenderPath::Desktop;
            case static_cast<int>(RenderPath::Mobile): return RenderPath::Mobile;
            default: return RenderPath::Auto;
        }
    }

    bool EqualsIgnoreCase(const char* a, const char* b)
    {
        while (*a != '\0' && *b != '\0')
        {
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }

    // Startup override (mirrors the ZENGINE_V8_DEBUG_PORT env pattern): seeds the
    // configured path once from ZENGINE_RENDER_PATH so headless / CI runs and quick
    // manual smoke tests can force a path without the editor UI. Accepts the case-
    // insensitive names auto|desktop|mobile or the numeric 0|1|2.
    void ApplyEnvOverrideOnce()
    {
        static bool s_applied = false;
        if (s_applied)
        {
            return;
        }
        s_applied = true;

        const char* env = std::getenv("ZENGINE_RENDER_PATH");
        if (env == nullptr || env[0] == '\0')
        {
            return;
        }

        if (std::isdigit(static_cast<unsigned char>(env[0])) != 0)
        {
            g_render_path = static_cast<int>(ClampToPath(std::atoi(env)));
            return;
        }
        if (EqualsIgnoreCase(env, "desktop"))
        {
            g_render_path = static_cast<int>(RenderPath::Desktop);
        }
        else if (EqualsIgnoreCase(env, "mobile"))
        {
            g_render_path = static_cast<int>(RenderPath::Mobile);
        }
        else
        {
            g_render_path = static_cast<int>(RenderPath::Auto);
        }
    }
}  // namespace

RenderPath GetConfiguredPath()
{
    ApplyEnvOverrideOnce();
    return ClampToPath(g_render_path);
}

RenderPath ResolveAuto()
{
#if Z_PLATFORM_IS_ANDROID || Z_PLATFORM_IS_IOS || Z_PLATFORM_IS_OHOS
    return RenderPath::Mobile;
#else
    return RenderPath::Desktop;
#endif
}

RenderPath GetEffectivePath()
{
    const RenderPath configured = GetConfiguredPath();
    return configured == RenderPath::Auto ? ResolveAuto() : configured;
}

const char* ToString(RenderPath path)
{
    switch (path)
    {
        case RenderPath::Desktop: return "Desktop";
        case RenderPath::Mobile: return "Mobile";
        case RenderPath::Auto:
        default: return "Auto";
    }
}

void SetConfiguredPath(RenderPath path)
{
    const int new_value = static_cast<int>(path);
    if (new_value != g_render_path)
    {
        g_render_path = new_value;
        g_path_dirty.store(true, std::memory_order_release);
    }
    // Keep the CVar storage in sync (it shares g_render_path, so nothing more to do).
}

bool ConsumePathDirty()
{
    return g_path_dirty.exchange(false, std::memory_order_acq_rel);
}

void RegisterConsoleVariables(ConsoleManager& console)
{
    auto cvar = console.RegisterIntVariable(
        "r.RenderPath",
        "Active render path: 0=Auto (platform default), 1=Desktop (deferred+forward), 2=Mobile (forward).",
        static_cast<int>(RenderPath::Auto),
        &g_render_path);
    if (cvar)
    {
        cvar->setOnChangedCallback([]() { g_path_dirty.store(true, std::memory_order_release); });
    }
}

}  // namespace RenderPipelineSettings
