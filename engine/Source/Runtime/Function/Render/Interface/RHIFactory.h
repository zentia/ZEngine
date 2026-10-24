#pragma once

// ============================================================================
// RHIFactory — RHI backend registry (audit #4 refactor)
// ----------------------------------------------------------------------------
// 历史：本文件原先配对的 RHIFactory.mm 是一个硬编码的 switch：
//   - 只被 Apple 编译（.mm），其它平台根本没有这个翻译单元；
//   - CreateRHI() 的 default 分支 `return CreateRHI(GetDefaultAPI(), ...)`
//     在 api 不可用时调用自身 —— 无限递归；
//   - 新增后端必须改这个 switch，无法自注册。
// 现在改成跨平台的 RegisterBackend 注册表：每个后端在自己的翻译单元里用
// RHIBackendRegistrar 静态自注册，工厂查表构造。ZRuntime 已经 whole-archive
// 链接（见 Runtime/CMakeLists.txt 的 --whole-archive / /WHOLEARCHIVE），所以
// 这些静态注册器会在 main 之前跑，工厂查表即可命中。
//
// 新增一个后端因此只需两步，不必动工厂：
//   1. 后端类实现 RHI；
//   2. 后端 .cpp/.mm 末尾放一个 RHIBackendRegistrar 静态实例。
// ============================================================================

#include "Runtime/Function/Render/RenderType.h"

#include <functional>
#include <map>

class RHI;
class WindowSystem;

class RHIFactory
{
public:
    // 构造一个后端实例。window_system 透传给后端（当前各后端构造都不用它，
    // 保留形参以便未来需要在创建期拿到窗口表面）。api 未注册时退回
    // GetDefaultAPI()；默认后端也未注册时返回 nullptr（不再无限递归）。
    static RHI* CreateRHI(GraphicsAPI api, WindowSystem* window_system);

    // 当前平台首选的后端（Windows 默认 DX12、Linux/Android/OHOS Vulkan、
    // macOS/iOS Metal、Web WebGL2）。这是"平台偏好"决策，集中在一处，
    // 与"能否构造"（注册表）分开。
    static GraphicsAPI GetDefaultAPI();

    // 该后端是否在本平台注册可用（注册表里有没有它）。
    static bool IsAPIAvailable(GraphicsAPI api);

    // 注册一个后端构造器。幂等：同一 api 后注册者覆盖前者。
    // 由各后端的 RHIBackendRegistrar 静态实例在加载期调用。
    static void RegisterBackend(GraphicsAPI api, std::function<RHI*(WindowSystem*)> creator);

private:
    struct Entry
    {
        std::function<RHI*(WindowSystem*)> creator;
    };

    // 函数内静态（Meyers 单例）：首次访问时构造，多线程安全；静态注册器
    // 在主线程顺序执行，不会与注册产生数据竞争。
    static std::map<GraphicsAPI, Entry>& Registry();
};

// ---------------------------------------------------------------------------
// RHIBackendRegistrar — 后端自注册助手
// ----------------------------------------------------------------------------
// 在后端翻译单元末尾放一个匿名命名空间的静态实例即可：
//
//   namespace {
//       struct VulkanRHIBackendRegistrar {
//           VulkanRHIBackendRegistrar() {
//               RHIFactory::RegisterBackend(GraphicsAPI::Vulkan,
//                   [](WindowSystem* ws) { return new VulkanRHI(); });
//           }
//       } s_vulkan_rhi_registrar;
//   }
//
// 之所以用"局部静态实例"而不是全局函数指针：静态初始化顺序跨翻译单元不确定，
// 而这里把注册动作放进一个明确的构造函数，且该实例本身是匿名命名空间静态存储
// 期对象，同样在 main 之前、单线程地完成注册。
class RHIBackendRegistrar
{
public:
    RHIBackendRegistrar(GraphicsAPI api, std::function<RHI*(WindowSystem*)> creator)
    {
        RHIFactory::RegisterBackend(api, std::move(creator));
    }
};
