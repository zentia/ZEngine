#include "RuntimeConsoleCommands.h"

#include "ConsoleManager.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/LargeWorldCoordinatesSettings.h"
#if defined(Z_HAS_LUMEN)
    #include "Runtime/Function/Render/Lumen/LumenSettings.h"
#endif
#include "Runtime/Function/Render/Nanite/NaniteSettings.h"
#include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"
#include "Runtime/Function/Render/Spine/SpineGpuRenderer.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UMG/Asset/UMGAssetIO.h"
#include "Runtime/UMG/Core/UMGViewport.h"
#include "Runtime/UMG/Core/UUserWidget.h"
#include "Runtime/UMG/Demo/UMGDemoWidget.h"

#include <cstdlib>
#include <memory>

namespace
{
    bool CmdStat(const std::vector<std::string>& args)
    {
        auto app = GET_SYSTEM(Application);
        if (!app)
        {
            LOG_ERROR(ZConsole, "Application not available");
            return false;
        }

        const std::string sub = args.empty() ? "fps" : args[0];
        if (sub == "fps" || sub == "unit")
        {
            LOG_INFO(ZConsole, "FPS: {}", app->GetFps());
            return true;
        }

        LOG_ERROR(ZConsole, "Unknown stat target '{}'. Try: stat fps", sub);
        return false;
    }

    bool CmdQuit(const std::vector<std::string>&)
    {
        if (auto app = GET_SYSTEM(Application))
        {
            app->RequestQuit();
        }
        if (auto window = GET_SYSTEM(WindowSystem))
        {
            window->RequestClose();
        }
        LOG_INFO(ZConsole, "Quit requested");
        return true;
    }

    bool CmdEcho(const std::vector<std::string>& args)
    {
        std::string message;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i > 0)
            {
                message += ' ';
            }
            message += args[i];
        }
        LOG_INFO(ZConsole, "{}", message);
        return true;
    }

    // GPU-Spine 渲染入口（SpineGpuRenderer）：加载发生在渲染线程下一帧，结果看 spine.status
    bool CmdSpineLoad(const std::vector<std::string>& args)
    {
        if (args.empty())
        {
            LOG_ERROR(ZConsole, "Usage: spine.load <skeleton .skel.bytes|.skel|.json> [anim index]");
            return false;
        }
        const int anim = args.size() >= 2 ? std::atoi(args[1].c_str()) : 0;
        SpineGpuRenderer::Get().RequestLoad(args[0], anim);
        LOG_INFO(ZConsole, "spine.load: requested '{}' anim #{}", args[0], anim);
        return true;
    }

    bool CmdSpineClear(const std::vector<std::string>&)
    {
        SpineGpuRenderer::Get().RequestClear();
        LOG_INFO(ZConsole, "spine.clear: requested");
        return true;
    }

    bool CmdSpineStatus(const std::vector<std::string>&)
    {
        LOG_INFO(ZConsole, "spine.status: {}", SpineGpuRenderer::Get().Status());
        return true;
    }

    // ---- 多轨播放（S5，语义 = spine::AnimationState）----
    // 数字参数解析失败不猜默认值：报用法并返回 false。
    bool ParseFloatArg(const std::string& s, float& out)
    {
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0')
        {
            return false;
        }
        out = v;
        return true;
    }

    bool ParseIntArg(const std::string& s, int& out)
    {
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || v < 0 || v > 64)
        {
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }

    bool CmdSpinePlay(const std::vector<std::string>& args)
    {
        int track = 0;
        int loop = 1;
        if (args.empty() || (args.size() >= 2 && !ParseIntArg(args[1], track)) ||
            (args.size() >= 3 && !ParseIntArg(args[2], loop)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.play <anim index|name> [track=0] [loop=1]");
            return false;
        }
        SpineGpuRenderer::Get().RequestSetAnimation(track, args[0], loop);
        LOG_INFO(ZConsole, "spine.play: track {} <- '{}' (loop {})", track, args[0], loop);
        return true;
    }

    bool CmdSpineQueue(const std::vector<std::string>& args)
    {
        int track = 0;
        float delay = 0.0f;
        int loop = 1;
        if (args.empty() || (args.size() >= 2 && !ParseIntArg(args[1], track)) ||
            (args.size() >= 3 && !ParseFloatArg(args[2], delay)) || (args.size() >= 4 && !ParseIntArg(args[3], loop)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.queue <anim index|name> [track=0] [delay=0 (<=0: after current, minus mix)] [loop=1]");
            return false;
        }
        SpineGpuRenderer::Get().RequestAddAnimation(track, args[0], loop != 0, delay);
        LOG_INFO(ZConsole, "spine.queue: track {} += '{}' (delay {}, loop {})", track, args[0], delay, loop);
        return true;
    }

    bool CmdSpineEmpty(const std::vector<std::string>& args)
    {
        int track = 0;
        float mix = 0.2f;
        if ((args.size() >= 1 && !ParseIntArg(args[0], track)) || (args.size() >= 2 && !ParseFloatArg(args[1], mix)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.empty [track=0] [mix seconds=0.2]");
            return false;
        }
        SpineGpuRenderer::Get().RequestSetEmptyAnimation(track, mix);
        LOG_INFO(ZConsole, "spine.empty: track {} fades out over {}s", track, mix);
        return true;
    }

    bool CmdSpineMix(const std::vector<std::string>& args)
    {
        float mix = 0.0f;
        if (args.size() != 1 || !ParseFloatArg(args[0], mix))
        {
            LOG_ERROR(ZConsole, "Usage: spine.mix <default mix seconds>");
            return false;
        }
        SpineGpuRenderer::Get().RequestDefaultMix(mix);
        return true;
    }

    bool CmdSpineSpeed(const std::vector<std::string>& args)
    {
        float scale = 1.0f;
        if (args.size() != 1 || !ParseFloatArg(args[0], scale))
        {
            LOG_ERROR(ZConsole, "Usage: spine.speed <time scale>");
            return false;
        }
        SpineGpuRenderer::Get().RequestTimeScale(scale);
        LOG_INFO(ZConsole, "spine.speed: {}", scale);
        return true;
    }

    // ---- 多实例 ----
    bool ParseUintArg(const std::string& s, uint32_t& out, uint32_t max_value)
    {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s.c_str(), &end, 10);
        if (s.empty() || s[0] == '-' || end == s.c_str() || *end != '\0' || v > max_value)
        {
            return false;
        }
        out = static_cast<uint32_t>(v);
        return true;
    }

    bool CmdSpineAdd(const std::vector<std::string>& args)
    {
        uint32_t anim = 0;
        uint32_t count = 1;
        if (args.empty() || (args.size() >= 2 && !ParseUintArg(args[1], anim, 100000u)) ||
            (args.size() >= 3 && (!ParseUintArg(args[2], count, 1024u) || count == 0)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.add <skeleton .skel.bytes|.skel|.json> [anim index=0] [count=1..1024]");
            return false;
        }
        uint32_t first = 0;
        uint32_t last = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            last = SpineGpuRenderer::Get().RequestAddInstance(args[0], static_cast<int>(anim));
            first = i == 0 ? last : first;
        }
        if (count == 1)
        {
            LOG_INFO(ZConsole, "spine.add: instance #{} <- '{}' anim #{} (selected)", last, args[0], anim);
        }
        else
        {
            LOG_INFO(ZConsole, "spine.add: instances #{}..#{} <- '{}' anim #{} (#{} selected)", first, last, args[0], anim,
                     last);
        }
        return true;
    }

    bool CmdSpineRemove(const std::vector<std::string>& args)
    {
        uint32_t id = 0;
        if (args.size() > 1 || (args.size() == 1 && !ParseUintArg(args[0], id, 0xFFFFFFFFu)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.remove [instance id=selected]");
            return false;
        }
        if (!SpineGpuRenderer::Get().RequestRemoveInstance(id))
        {
            LOG_ERROR(ZConsole, "spine.remove: no instance #{}", id == 0 ? std::string("(none selected)") : std::to_string(id));
            return false;
        }
        LOG_INFO(ZConsole, "spine.remove: requested; selected is now #{}", SpineGpuRenderer::Get().Selected());
        return true;
    }

    bool CmdSpineSelect(const std::vector<std::string>& args)
    {
        uint32_t id = 0;
        if (args.size() != 1 || !ParseUintArg(args[0], id, 0xFFFFFFFFu))
        {
            LOG_ERROR(ZConsole, "Usage: spine.select <instance id>");
            return false;
        }
        if (!SpineGpuRenderer::Get().RequestSelect(id))
        {
            LOG_ERROR(ZConsole, "spine.select: no instance #{}", id);
            return false;
        }
        LOG_INFO(ZConsole, "spine.select: #{}", id);
        return true;
    }

    bool CmdSpinePlace(const std::vector<std::string>& args)
    {
        if (args.size() == 1 && args[0] == "auto")
        {
            SpineGpuRenderer::Get().RequestPlaceAuto();
            LOG_INFO(ZConsole, "spine.place: #{} back to the auto grid", SpineGpuRenderer::Get().Selected());
            return true;
        }
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        if (args.size() < 2 || args.size() > 3 || !ParseFloatArg(args[0], x) || !ParseFloatArg(args[1], y) ||
            (args.size() == 3 && !ParseFloatArg(args[2], scale)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.place <x px> <y px, up> [scale px/unit=1] | spine.place auto");
            return false;
        }
        SpineGpuRenderer::Get().RequestPlace(x, y, scale);
        LOG_INFO(ZConsole, "spine.place: #{} at ({}, {}) x{}", SpineGpuRenderer::Get().Selected(), x, y, scale);
        return true;
    }

    bool CmdSpineSkin(const std::vector<std::string>& args)
    {
        int setup = 1;
        if (args.empty() || args.size() > 2 || (args.size() == 2 && (!ParseIntArg(args[1], setup) || setup > 1)))
        {
            LOG_ERROR(ZConsole, "Usage: spine.skin <skin name|index|none> [setSlotsToSetupPose 0|1=1]");
            return false;
        }
        SpineGpuRenderer::Get().RequestSkin(args[0], setup != 0);
        LOG_INFO(ZConsole, "spine.skin: #{} <- '{}'{}", SpineGpuRenderer::Get().Selected(), args[0],
                 setup != 0 ? " (+ setSlotsToSetupPose)" : "");
        return true;
    }

    bool CmdSpineTintBlack(const std::vector<std::string>& args)
    {
        int on = 0;
        if (args.size() != 1 || !ParseIntArg(args[0], on) || on > 1)
        {
            LOG_ERROR(ZConsole, "Usage: spine.tintblack <0|1>  (SkeletonGraphic tintBlack; default 0 like OSGame)");
            return false;
        }
        SpineGpuRenderer::Get().RequestTintBlack(on != 0);
        LOG_INFO(ZConsole, "spine.tintblack: {}", on != 0 ? "on" : "off");
        return true;
    }

    bool CmdSpineAlpha(const std::vector<std::string>& args)
    {
        int track = 0;
        float alpha = 1.0f;
        if (args.size() != 2 || !ParseIntArg(args[0], track) || !ParseFloatArg(args[1], alpha))
        {
            LOG_ERROR(ZConsole, "Usage: spine.alpha <track> <alpha 0..1>");
            return false;
        }
        SpineGpuRenderer::Get().RequestTrackAlpha(track, alpha);
        LOG_INFO(ZConsole, "spine.alpha: track {} = {}", track, alpha);
        return true;
    }

    // P1 acceptance: toggle a C++-authored UMG UserWidget on the runtime viewport.
    // Proves the UMG object model -> ZSlate -> shared UIPass render + input loop.
    bool CmdUmgDemo(const std::vector<std::string>& args)
    {
        static std::shared_ptr<ZUMG::UMGDemoWidget> s_demo;
        const std::string sub = args.empty() ? "toggle" : args[0];

        if (sub == "hide" || (sub == "toggle" && s_demo && s_demo->IsInViewport()))
        {
            if (s_demo)
            {
                s_demo->RemoveFromViewport();
                LOG_INFO(ZConsole, "umg.demo: hidden");
            }
            return true;
        }

        if (!s_demo)
        {
            s_demo = std::make_shared<ZUMG::UMGDemoWidget>();
        }
        s_demo->AddToViewport(0);
        LOG_INFO(ZConsole, "umg.demo: shown");
        return true;
    }

    std::shared_ptr<ZUMG::UUserWidget>& LoadedUmgAsset()
    {
        static std::shared_ptr<ZUMG::UUserWidget> s_loaded;
        return s_loaded;
    }

    // P2 acceptance (save): flatten the demo widget tree into a .zasset.
    bool CmdUmgSave(const std::vector<std::string>& args)
    {
        const std::string url = args.empty() ? "UMGDemo.zasset" : args[0];
        auto demo = std::make_shared<ZUMG::UMGDemoWidget>();
        // Build the tree without showing it (TakeWidget runs Build()).
        demo->TakeWidget();
        if (!ZUMG::SaveWidgetTreeAsset(demo->GetRootWidget(), url))
        {
            LOG_ERROR(ZConsole, "umg.save: failed to write '{}'", url);
            return false;
        }
        LOG_INFO(ZConsole, "umg.save: wrote '{}'", url);
        return true;
    }

    // P2 acceptance (load): rebuild a UserWidget from a .zasset and show it.
    bool CmdUmgLoad(const std::vector<std::string>& args)
    {
        const std::string url = args.empty() ? "UMGDemo.zasset" : args[0];
        std::shared_ptr<ZUMG::UUserWidget>& loaded = LoadedUmgAsset();
        if (loaded && loaded->IsInViewport())
            loaded->RemoveFromViewport();
        loaded = ZUMG::LoadUserWidgetFromAsset(url);
        if (!loaded)
        {
            LOG_ERROR(ZConsole, "umg.load: failed to load '{}'", url);
            return false;
        }
        loaded->AddToViewport(1);
        LOG_INFO(ZConsole, "umg.load: shown from '{}'", url);
        return true;
    }
}  // namespace

void RegisterRuntimeConsoleCommands(ConsoleManager& console)
{
    console.RegisterCommand("stat", "Performance stats. Usage: stat fps", CmdStat);
    console.RegisterCommand("quit", "Exit the application", CmdQuit);
    console.RegisterCommand("exit", "Alias for quit", CmdQuit);
    console.RegisterCommand("echo", "Print text to the console log", CmdEcho);
    console.RegisterCommand("spine.load",
                            "Draw a Spine skeleton with GPU skinning. Usage: spine.load <skeleton file> [anim index]",
                            CmdSpineLoad);
    console.RegisterCommand("spine.clear", "Remove all GPU-Spine instances", CmdSpineClear);
    console.RegisterCommand("spine.add",
                            "Add GPU-Spine instance(s) (shares the asset by path; selects the last). Usage: spine.add "
                            "<skeleton file> [anim index] [count]",
                            CmdSpineAdd);
    console.RegisterCommand("spine.remove", "Remove a GPU-Spine instance. Usage: spine.remove [id=selected]",
                            CmdSpineRemove);
    console.RegisterCommand("spine.select",
                            "Select the GPU-Spine instance that play/queue/empty/speed/alpha/place act on. Usage: "
                            "spine.select <id>",
                            CmdSpineSelect);
    console.RegisterCommand("spine.place",
                            "Place the selected GPU-Spine instance. Usage: spine.place <x> <y> [scale] | spine.place auto",
                            CmdSpinePlace);
    console.RegisterCommand("spine.status", "Print GPU-Spine renderer status", CmdSpineStatus);
    console.RegisterCommand("spine.play",
                            "Set a GPU-Spine animation on a track (crossfades). Usage: spine.play <anim> [track] [loop]",
                            CmdSpinePlay);
    console.RegisterCommand("spine.queue",
                            "Queue a GPU-Spine animation after the current one. Usage: spine.queue <anim> [track] [delay] [loop]",
                            CmdSpineQueue);
    console.RegisterCommand("spine.empty", "Fade a GPU-Spine track out. Usage: spine.empty [track] [mix seconds]",
                            CmdSpineEmpty);
    console.RegisterCommand("spine.mix", "Default GPU-Spine crossfade duration. Usage: spine.mix <seconds>", CmdSpineMix);
    console.RegisterCommand("spine.speed", "GPU-Spine AnimationState time scale. Usage: spine.speed <scale>",
                            CmdSpineSpeed);
    console.RegisterCommand("spine.skin",
                            "Set the selected GPU-Spine instance's skin (Skeleton::setSkin). Usage: spine.skin "
                            "<name|index|none> [setup slots 0|1]",
                            CmdSpineSkin);
    console.RegisterCommand("spine.tintblack",
                            "GPU-Spine tint black (two-color slots). Usage: spine.tintblack <0|1>", CmdSpineTintBlack);
    console.RegisterCommand("spine.alpha", "Alpha of a GPU-Spine track's current entry. Usage: spine.alpha <track> <alpha>",
                            CmdSpineAlpha);
    console.RegisterCommand("umg.demo", "Toggle the UMG demo UserWidget. Usage: umg.demo [show|hide|toggle]", CmdUmgDemo);
    console.RegisterCommand("umg.save", "Serialize the UMG demo tree to a .zasset. Usage: umg.save [url]", CmdUmgSave);
    console.RegisterCommand("umg.load", "Load + show a UMG widget .zasset. Usage: umg.load [url]", CmdUmgLoad);

    // Common UE-style CVars (storage is internal until gameplay reads them).
    console.RegisterIntVariable("t.MaxFPS", "Target max frame rate (0 = uncapped)", 0, nullptr);
    console.RegisterBoolVariable("r.ShowFPS", "Log FPS every second when true", false, nullptr);

    // Cross-platform render path selection (Desktop deferred vs Mobile forward).
    // The RenderSystem applies a change at the next safe frame boundary.
    RenderPipelineSettings::RegisterConsoleVariables(console);
    LargeWorldCoordinatesSettings::RegisterConsoleVariables(console);
#if defined(Z_HAS_LUMEN)
    // Lumen (CPU distance-field GI): every knob is live (re-applied each frame).
    LumenSettings::RegisterConsoleVariables(console);
#endif
    // Nanite (CPU cluster build + culling): every knob is live (re-applied each frame).
    NaniteSettings::RegisterConsoleVariables(console);
}
