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
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3.h>
    #include <GLFW/glfw3native.h>
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

            GLFWwindow* window = window_system->GetWindow();
            if (window == nullptr)
            {
                return nullptr;
            }

    #if defined(_WIN32)
            return glfwGetWin32Window(window);
    #else
            return nullptr;
    #endif
        }
    }  // namespace

    void RenderDoc::Init()
    {
    #if defined(_WIN32)
        Z::RenderDocInitParams params;
        if (auto* command_system = GET_SYSTEM(CommandSystem))
        {
            params.load_module = command_system->GetFlag("load-renderdoc");
            if (auto dll_path = command_system->GetOption("renderdoc-dll"))
            {
                if (!dll_path->empty())
                {
                    params.dll_path_override = dll_path->c_str();
                }
            }
        }

        Z::RenderDocAPI::Init(params);
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

        api->EndFrameCapture(device, window);

        if (!api->IsRemoteAccessConnected())
        {
            api->LaunchReplayUI(true, "");
        }

        LOG_INFO(ZEditor, "RenderDoc: frame capture saved");
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
