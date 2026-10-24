// ============================================================================
// RHIFactory — RHI backend registry implementation (audit #4 refactor)
// ----------------------------------------------------------------------------
// 取代原先的 RHIFactory.mm（硬编码 switch + 无限递归 default）。跨平台编译，
// 后端通过 RHIBackendRegistrar 自注册进 Registry()。
// ============================================================================

#include "RHIFactory.h"

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <map>

#if defined(__EMSCRIPTEN__)
    #define ZENGINE_RHI_PLATFORM_WEBGL2 1
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #define ZENGINE_RHI_PLATFORM_APPLE 1
#elif defined(_WIN32) || defined(Z_PLATFORM_WINDOWS)
    #define ZENGINE_RHI_PLATFORM_WINDOWS 1
#elif defined(__ANDROID__) || defined(Z_PLATFORM_ANDROID)
    #define ZENGINE_RHI_PLATFORM_ANDROID 1
#elif defined(__OHOS__) || defined(Z_PLATFORM_OHOS)
    #define ZENGINE_RHI_PLATFORM_OHOS 1
#elif defined(__linux__) || defined(Z_PLATFORM_LINUX)
    #define ZENGINE_RHI_PLATFORM_LINUX 1
#endif

std::map<GraphicsAPI, RHIFactory::Entry>& RHIFactory::Registry()
{
    // Meyers 单例：C++11 起函数内静态局部变量的初始化线程安全。
    static std::map<GraphicsAPI, Entry> s_registry;
    return s_registry;
}

void RHIFactory::RegisterBackend(GraphicsAPI api, std::function<RHI*(WindowSystem*)> creator)
{
    if (!creator)
    {
        return;
    }
    Registry()[api] = Entry{ std::move(creator) };
}

bool RHIFactory::IsAPIAvailable(GraphicsAPI api)
{
    return Registry().find(api) != Registry().end();
}

GraphicsAPI RHIFactory::GetDefaultAPI()
{
    // 平台"首选后端"决策——与"能否构造"（注册表）分开。即便某平台没注册对应
    // 后端（例如 Windows 关掉 Vulkan 的 slim 构建），这里仍返回该平台偏好，
    // CreateRHI() 会再查注册表、查不到才退回 nullptr。
#if defined(ZENGINE_RHI_PLATFORM_WEBGL2)
    return GraphicsAPI::WebGL2;
#elif defined(ZENGINE_RHI_PLATFORM_APPLE)
    return GraphicsAPI::Metal;
#elif defined(ZENGINE_RHI_PLATFORM_WINDOWS)
    return GraphicsAPI::DirectX12;
#elif defined(ZENGINE_RHI_PLATFORM_ANDROID) || defined(ZENGINE_RHI_PLATFORM_OHOS)
    return GraphicsAPI::Vulkan;
#elif defined(ZENGINE_RHI_PLATFORM_LINUX)
    return GraphicsAPI::Vulkan;
#else
    return GraphicsAPI::Vulkan;
#endif
}

RHI* RHIFactory::CreateRHI(GraphicsAPI api, WindowSystem* window_system)
{
    // 请求的后端没注册（例如该平台未编译它）→ 退回平台默认。
    auto it = Registry().find(api);
    if (it == Registry().end())
    {
        api = GetDefaultAPI();
        it = Registry().find(api);
    }
    if (it == Registry().end())
    {
        // 连默认后端都没注册——说明本平台一个 RHI 都没编进来。返回 nullptr，
        // 由调用方（RegisterSystem）决定如何处理，绝不递归。
        return nullptr;
    }
    return it->second.creator(window_system);
}
