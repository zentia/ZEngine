#include "RenderDoc.h"

#include "RenderDocAPI.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <cstdint>

#if defined(_WIN32)
    #include "Runtime/Function/Render/Platform/Generic/GenericWindow.h"

    #include <filesystem>
    #include <string>
    #include <windows.h>
#endif

namespace Runtime
{
    namespace
    {
        enum class CaptureState : uint8_t
        {
            Idle,
            BeginNextFrame,
            Capturing,
        };

        CaptureState g_capture_state = CaptureState::Idle;
        bool g_capture_path_template_set = false;

        std::filesystem::path ResolveRepoRootForCaptures()
        {
            if (const char* env_root = std::getenv("ZENGINE_ROOT"))
            {
                if (env_root[0] != '\0')
                {
                    return std::filesystem::path(env_root);
                }
            }

            wchar_t exe_path[MAX_PATH] = {};
            if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) != 0)
            {
                std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
                const std::filesystem::path bin_dir = exe_dir.parent_path();
                if (bin_dir.filename() == "bin")
                {
                    return bin_dir.parent_path();
                }
            }

            return std::filesystem::current_path();
        }

        void EnsureCapturePathTemplate(RENDERDOC_API_1_0_0* api)
        {
            if (g_capture_path_template_set || api == nullptr)
            {
                return;
            }

            std::error_code ec;
            const std::filesystem::path captures_dir = ResolveRepoRootForCaptures() / "Captures";
            std::filesystem::create_directories(captures_dir, ec);

            // RenderDoc appends _frameNNN.rdc to this template path.
            const std::string template_utf8 = (captures_dir / "zeditor").string();
            api->SetCaptureFilePathTemplate(template_utf8.c_str());
            g_capture_path_template_set = true;
            LOG_INFO(ZEditor, "RenderDoc: capture template set to {}", template_utf8);
        }

        void LogLatestCapturePath(RENDERDOC_API_1_0_0* api)
        {
            if (api == nullptr)
            {
                return;
            }

            const uint32_t capture_count = api->GetNumCaptures();
            if (capture_count == 0)
            {
                LOG_WARNING(ZEditor, "RenderDoc: capture finished but GetNumCaptures() returned 0");
                return;
            }

            char path_buffer[1024] = {};
            uint32_t path_length = static_cast<uint32_t>(sizeof(path_buffer));
            if (api->GetCapture(capture_count - 1, path_buffer, &path_length, nullptr) == 0)
            {
                LOG_WARNING(ZEditor, "RenderDoc: failed to query latest capture path");
                return;
            }

            LOG_INFO(ZEditor, "RenderDoc: capture saved to {}", path_buffer);
        }

        void* ResolveNativeDevicePointer()
        {
            auto* render_system = GET_SYSTEM(RenderSystem);
            if (render_system == nullptr)
            {
                return nullptr;
            }

            auto* rhi = render_system->GetRHI();
            if (rhi == nullptr || rhi->getGraphicsAPI() != GraphicsAPI::DirectX12)
            {
                return nullptr;
            }

            auto* dx12_rhi = dynamic_cast<DX12RHI*>(rhi);
            if (dx12_rhi == nullptr)
            {
                return nullptr;
            }

            return dx12_rhi->getDevice();
        }

        void* ResolveNativeWindowHandle()
        {
            auto* window_system = GET_SYSTEM(WindowSystem);
            if (window_system == nullptr)
            {
                return nullptr;
            }

            GenericWindow* window = window_system->GetMainWindow();
            if (window == nullptr)
            {
                return nullptr;
            }

    #if defined(_WIN32)
            return window->GetNativeHandle();
    #else
            return nullptr;
    #endif
        }
    }  // namespace

    void RenderDoc::Init()
    {
    #if defined(_WIN32)
        // RenderDocLoader (PreInit) loads renderdoc.dll before D3D12 device creation.
        // Keep this hook for callers that expect Editor-time initialization side effects.
        if (Z::RenderDocAPI::Get() == nullptr)
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
        }
    #endif
    }

    void RenderDoc::Load()
    {
    #if defined(_WIN32)
        Z::RenderDocAPI::LoadModule();
    #endif
    }

    bool RenderDoc::IsInstalled()
    {
    #if defined(_WIN32)
        return Z::RenderDocAPI::IsInstalled();
    #else
        return false;
    #endif
    }

    bool RenderDoc::IsLoaded()
    {
    #if defined(_WIN32)
        return Z::RenderDocAPI::Get() != nullptr;
    #else
        return false;
    #endif
    }

    bool RenderDoc::IsSupported()
    {
    #if defined(_WIN32)
        if (!IsLoaded())
        {
            return false;
        }

        return ResolveNativeDevicePointer() != nullptr;
    #else
        return false;
    #endif
    }

    void RenderDoc::RequestCapture()
    {
    #if defined(_WIN32)
        if (!IsLoaded())
        {
            Load();
        }

        if (!IsSupported())
        {
            LOG_WARNING(ZEditor, "RenderDoc capture requested but RenderDoc/DX12 is not available");
            return;
        }

        g_capture_state = CaptureState::BeginNextFrame;
        if (auto* api = Z::RenderDocAPI::Get())
        {
            EnsureCapturePathTemplate(api);
        }
        LOG_INFO(ZEditor, "RenderDoc: frame capture scheduled for next frame");
    #endif
    }

    void RenderDoc::BeginFrameCapture()
    {
    #if defined(_WIN32)
        auto* api = Z::RenderDocAPI::Get();
        if (api == nullptr)
        {
            return;
        }

        void* device = ResolveNativeDevicePointer();
        void* window = ResolveNativeWindowHandle();
        if (device == nullptr || window == nullptr)
        {
            LOG_WARNING(ZEditor, "RenderDoc: BeginFrameCapture skipped (missing DX12 device or window handle)");
            return;
        }

        api->StartFrameCapture(device, window);
    #endif
    }

    void RenderDoc::EndFrameCapture()
    {
    #if defined(_WIN32)
        auto* api = Z::RenderDocAPI::Get();
        if (api == nullptr)
        {
            return;
        }

        void* device = ResolveNativeDevicePointer();
        void* window = ResolveNativeWindowHandle();
        if (device == nullptr || window == nullptr)
        {
            return;
        }

        const uint32_t captured = api->EndFrameCapture(device, window);
        if (!captured)
        {
            LOG_WARNING(ZEditor,
                        "RenderDoc: EndFrameCapture failed (ensure renderdoc.dll loaded before D3D12 init; "
                        "see renderdoc.status)");
            return;
        }

        LogLatestCapturePath(api);

        if (!api->IsRemoteAccessConnected())
        {
            api->LaunchReplayUI(true, "");
        }
    #endif
    }

    void RenderDoc::OnPreFrame()
    {
    #if defined(_WIN32)
        if (g_capture_state != CaptureState::BeginNextFrame)
        {
            return;
        }

        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            render_system->FlushRenderingCommands();
        }

        BeginFrameCapture();
        g_capture_state = CaptureState::Capturing;
    #endif
    }

    void RenderDoc::OnPostFrame()
    {
    #if defined(_WIN32)
        if (g_capture_state != CaptureState::Capturing)
        {
            return;
        }

        if (auto* render_system = GET_SYSTEM(RenderSystem))
        {
            render_system->FlushRenderingCommands();
        }

        EndFrameCapture();
        g_capture_state = CaptureState::Idle;
    #endif
    }
}  // namespace Runtime
