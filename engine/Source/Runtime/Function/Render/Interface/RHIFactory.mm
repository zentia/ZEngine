#include "RHIFactory.h"

#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <EASTL/functional.h>
#include <EASTL/string.h>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

#if defined(Z_HAS_VULKAN)
    #include "Runtime/Function/Render/Interface/Vulkan/VulkanRHI.h"
#endif

#ifdef _WIN32
    #include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"
#endif

#ifdef __APPLE__
    #include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
#endif

#ifdef __EMSCRIPTEN__
    #include "Runtime/Function/Render/Interface/WebGL2/WebGL2RHI.h"
#endif

#include "Runtime/Core/Base/Factory.h"

#include <memory>

RHI* RHIFactory::CreateRHI(GraphicsAPI api, WindowSystem* window_system)
{
    switch (api)
    {
#if defined(Z_HAS_VULKAN)
        case GraphicsAPI::Vulkan:
        {
            return new VulkanRHI();
        }
#endif
#ifdef _WIN32
        case GraphicsAPI::DirectX12:
        {
            return new DX12RHI();
        }
#endif
#ifdef __APPLE__
        case GraphicsAPI::Metal:
        {
            return new MetalRHI();
        }
#endif
#ifdef __EMSCRIPTEN__
        case GraphicsAPI::WebGL2:
        {
            return new ZEngine::WebGL2::WebGL2RHI();
        }
#endif
        default:
            return CreateRHI(GetDefaultAPI(), window_system);
    }
}

GraphicsAPI RHIFactory::GetDefaultAPI()
{
#ifdef __EMSCRIPTEN__
    return GraphicsAPI::WebGL2;
#elif defined(__APPLE__)
    return GraphicsAPI::Metal;
#elif defined(_WIN32)
    return GraphicsAPI::DirectX12;
#elif defined(Z_HAS_VULKAN)
    return GraphicsAPI::Vulkan;
#else
    return GraphicsAPI::Vulkan;
#endif
}

bool RHIFactory::IsAPIAvailable(GraphicsAPI api)
{
    switch (api)
    {
        case GraphicsAPI::Vulkan:
#if defined(Z_HAS_VULKAN)
            return true;
#else
            return false;
#endif
#ifdef _WIN32
        case GraphicsAPI::DirectX12:
            return true;  // DX12 is available on Windows
#endif
#ifdef __APPLE__
        case GraphicsAPI::Metal:
            return true;  // Metal is available on macOS/iOS
#endif
#ifdef __EMSCRIPTEN__
        case GraphicsAPI::WebGL2:
            return true;  // WebGL2 is available under Emscripten
#endif
        default:
            return false;
    }
}
