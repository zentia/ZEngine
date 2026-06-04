#include "RenderDocLoader.h"

#if defined(_WIN32)

#include "RenderDocAPI.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Command/CommandSystem.h"

#include <filesystem>

namespace
{
    void ConfigureCaptureOutputDirectory()
    {
        auto* api = Z::RenderDocAPI::Get();
        if (api == nullptr)
        {
            return;
        }

        auto* command_system = GET_SYSTEM(CommandSystem);
        if (command_system == nullptr)
        {
            return;
        }

        std::filesystem::path capture_root;
        const std::filesystem::path& working_dir = command_system->GetWorkingDir();
        if (!working_dir.empty())
        {
            capture_root = working_dir / "Intermediate" / "RenderDoc";
        }
        else
        {
            capture_root = std::filesystem::path(Z::RenderDocAPI::GetModulePath()).parent_path() / "RenderDocCaptures";
        }

        std::error_code ec;
        std::filesystem::create_directories(capture_root, ec);

        const std::filesystem::path capture_template = capture_root / "ZEditor_capture";
        api->SetCaptureFilePathTemplate(capture_template.string().c_str());
        LOG_INFO(ZRender, "RenderDocAPI: capture output template {}", capture_template.string());
    }
}  // namespace

bool RenderDocLoader::Initialize()
{
    Z::RenderDocInitParams params;
    if (auto* command_system = GET_SYSTEM(CommandSystem))
    {
        if (command_system->GetFlag("no-load-renderdoc"))
        {
            params.load_module = false;
        }
        if (auto dll_path = command_system->GetOption("renderdoc-dll"))
        {
            if (!dll_path->empty())
            {
                params.dll_path_override = dll_path->c_str();
            }
        }
    }

    Z::RenderDocAPI::Init(params);
    ConfigureCaptureOutputDirectory();
    return true;
}

#endif
