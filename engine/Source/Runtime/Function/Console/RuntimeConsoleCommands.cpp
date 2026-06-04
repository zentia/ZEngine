#include "RuntimeConsoleCommands.h"

#include "ConsoleManager.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/LargeWorldCoordinatesSettings.h"
#include "Runtime/Function/Render/Pipeline/RenderPipelineSettings.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UMG/Asset/UMGAssetIO.h"
#include "Runtime/UMG/Core/UMGViewport.h"
#include "Runtime/UMG/Core/UUserWidget.h"
#include "Runtime/UMG/Demo/UMGDemoWidget.h"

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
}
