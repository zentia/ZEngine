// ============================================================================
// RHIFactoryRegistrySmokeTest — 阶段一·夯地基 第六落点（#1 + #4）运行期验证
// ----------------------------------------------------------------------------
// 证明两件事：
//
//  #4（RHIFactory 注册表重构）：
//    - 每个编译进来的后端都通过 RHIBackendRegistrar 自注册进了工厂注册表；
//    - IsAPIAvailable() 反映真实注册情况；GetDefaultAPI() 给出本平台首选；
//    - CreateRHI() 查表构造出正确后端，未知 api 会退回默认后端（不再无限递归）。
//
//  #1（Linux 注册 RHI）：在 Linux 上 GetDefaultAPI()==Vulkan 且
//    IsAPIAvailable(Vulkan)==true —— 过去 Linux 落在 RegisterRuntime.cpp 的
//    #else 空分支，GET_SYSTEM(RHI)==nullptr。本测试在 Linux 上跑即验证该修复
//    （本机 Windows 无法替 Linux 验证，只保证 Windows 双后端矩阵正确）。
//
// 为什么能无头实例化后端：各后端构造函数都是默认的（VkInstance/D3D 指针默认
// nullptr），析构要么是空桩（Vulkan ~VulkanRHI 是 TODO）、要么按成员默认析构
// （DX12RHI 无自定义析构）。因此 new + getGraphicsAPI() + delete 无需真实 GPU
// 即可完成，与 SpineSkinningParityTest 的无头手法一致。
//
// 平台矩阵（本测试按编译期宏断言，避免在错误平台上误判）：
//   - Windows       : DX12 与 Vulkan 都应注册（ZENGINE_USE_VULKAN 默认 ON）
//   - Linux         : 只 Vulkan
//   - macOS/iOS     : 只 Metal
//   - Android/OHOS  : 只 Vulkan
//   - Emscripten    : 只 WebGL2
//
// 构建：
//   cmake -B build -DZENGINE_BUILD_RHI_REGISTRY_SMOKE_TEST=ON
//   cmake --build build --target RHIFactoryRegistrySmokeTest --config Debug
// ============================================================================

#include "Runtime/Function/Render/Interface/RHIFactory.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <cstdio>
#include <string>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }

    const char* ApiName(GraphicsAPI api)
    {
        switch (api)
        {
            case GraphicsAPI::Vulkan:     return "Vulkan";
            case GraphicsAPI::DirectX12:  return "DirectX12";
            case GraphicsAPI::Metal:      return "Metal";
            case GraphicsAPI::WebGL2:     return "WebGL2";
            default:                      return "<?>";
        }
    }

    // 平台首选后端（与 RHIFactory::GetDefaultAPI 的平台分支保持一致，用来在
    // 测试里交叉核对，而不是无条件假设）。
    GraphicsAPI ExpectDefaultAPI()
    {
#if defined(__EMSCRIPTEN__)
        return GraphicsAPI::WebGL2;
#elif defined(__APPLE__)
        return GraphicsAPI::Metal;
#elif defined(_WIN32) || defined(Z_PLATFORM_WINDOWS)
        return GraphicsAPI::DirectX12;
#elif defined(__ANDROID__) || defined(Z_PLATFORM_ANDROID)
        return GraphicsAPI::Vulkan;
#elif defined(__OHOS__) || defined(Z_PLATFORM_OHOS)
        return GraphicsAPI::Vulkan;
#elif defined(__linux__) || defined(Z_PLATFORM_LINUX)
        return GraphicsAPI::Vulkan;
#else
        return GraphicsAPI::Vulkan;
#endif
    }

    // 无头构造 + 校验 getGraphicsAPI()，随后 delete。返回是否通过。
    bool CreateAndCheck(GraphicsAPI api, bool expect_success)
    {
        RHI* rhi = RHIFactory::CreateRHI(api, nullptr);
        if (expect_success)
        {
            if (rhi == nullptr)
            {
                std::printf("         CreateRHI(%s) returned nullptr (expected a backend)\n",
                            ApiName(api));
                return false;
            }
            const GraphicsAPI got = rhi->getGraphicsAPI();
            delete rhi;
            if (got != api)
            {
                std::printf("         CreateRHI(%s) yielded backend %s (api mismatch)\n",
                            ApiName(api), ApiName(got));
                return false;
            }
            return true;
        }
        else
        {
            // 该平台未注册此后端：CreateRHI 会退回默认后端（而非返回 null / 递归）。
            const GraphicsAPI def = RHIFactory::GetDefaultAPI();
            if (rhi == nullptr)
            {
                std::printf("         CreateRHI(%s) returned nullptr (expected fallback to %s)\n",
                            ApiName(api), ApiName(def));
                return false;
            }
            const GraphicsAPI got = rhi->getGraphicsAPI();
            delete rhi;
            if (got != def)
            {
                std::printf("         CreateRHI(%s) fell back to %s, expected default %s\n",
                            ApiName(api), ApiName(got), ApiName(def));
                return false;
            }
            return true;
        }
    }
}  // namespace

int main()
{
    std::printf("RHIFactoryRegistrySmokeTest (#1 + #4)\n");

    const GraphicsAPI def = RHIFactory::GetDefaultAPI();
    std::printf("  platform default API = %s\n", ApiName(def));

    // (1) 默认后端一定注册可用。
    Check(RHIFactory::IsAPIAvailable(def), "GetDefaultAPI() is registered (IsAPIAvailable)");

    // (2) GetDefaultAPI 与编译期平台预期一致。
    Check(def == ExpectDefaultAPI(), "GetDefaultAPI() matches compile-time platform expectation");

    // (3) 用默认 api 构造，拿回的后端类型与 api 相符。
    Check(CreateAndCheck(def, /*expect_success=*/true),
          "CreateRHI(default) yields the default backend");

    // (4) 平台矩阵：按编译期宏断言哪些后端应注册、哪些不应。
    // Emscripten 必须排在 Linux 前面：emcc 从 CMake 的 UNIX 分支继承了
    // Z_PLATFORM_LINUX=1（DefaultAPI() 也是这个顺序）。原先 Web 分支排在 Linux 后面，
    // web 构建永远走 Linux 分支去断言 Vulkan，Web 断言一次都没执行过。
#if defined(__EMSCRIPTEN__)
    Check(RHIFactory::IsAPIAvailable(GraphicsAPI::WebGL2), "Web: WebGL2 registered");
    Check(!RHIFactory::IsAPIAvailable(GraphicsAPI::Vulkan), "Web: Vulkan NOT registered");
    Check(CreateAndCheck(GraphicsAPI::WebGL2, true), "Web: CreateRHI(WebGL2)");
#elif defined(_WIN32) || defined(Z_PLATFORM_WINDOWS)
    // Windows：DX12 与 Vulkan 都在（ZENGINE_USE_VULKAN 默认 ON）。
    Check(RHIFactory::IsAPIAvailable(GraphicsAPI::DirectX12), "Windows: DX12 registered");
    Check(RHIFactory::IsAPIAvailable(GraphicsAPI::Vulkan),    "Windows: Vulkan registered");
    Check(CreateAndCheck(GraphicsAPI::DirectX12, true),       "Windows: CreateRHI(DX12)");
    Check(CreateAndCheck(GraphicsAPI::Vulkan, true),          "Windows: CreateRHI(Vulkan)");
    // 未知/别家后端（Metal）应退回默认（DX12），不递归、不返回 null。
    Check(CreateAndCheck(GraphicsAPI::Metal, /*expect_success=*/false),
          "Windows: unknown API (Metal) falls back to default, no recursion");
#elif defined(__linux__) || defined(Z_PLATFORM_LINUX)
    // audit #1：Linux 只 Vulkan。
    Check(RHIFactory::IsAPIAvailable(GraphicsAPI::Vulkan), "Linux(#1): Vulkan registered");
    Check(!RHIFactory::IsAPIAvailable(GraphicsAPI::DirectX12), "Linux: DX12 NOT registered");
    Check(CreateAndCheck(GraphicsAPI::Vulkan, true), "Linux(#1): CreateRHI(Vulkan)");
#elif defined(__APPLE__)
    Check(RHIFactory::IsAPIAvailable(GraphicsAPI::Metal), "Apple: Metal registered");
    Check(CreateAndCheck(GraphicsAPI::Metal, true), "Apple: CreateRHI(Metal)");
#elif defined(__ANDROID__) || defined(__OHOS__)
    Check(RHIFactory::IsAPIAvailable(GraphicsAPI::Vulkan), "Mobile: Vulkan registered");
    Check(CreateAndCheck(GraphicsAPI::Vulkan, true), "Mobile: CreateRHI(Vulkan)");
#else
    std::printf("  [WARN] unhandled platform; only default-API check ran\n");
#endif

    // (5) 幂等性：重复注册同一后端不崩、注册表仍可用。
    RHIFactory::RegisterBackend(def, [def](WindowSystem*) -> RHI* { return RHIFactory::CreateRHI(def, nullptr); });
    Check(RHIFactory::IsAPIAvailable(def), "re-registering default backend is idempotent");

    if (g_Failures == 0)
    {
        std::printf("RHIFactoryRegistrySmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("RHIFactoryRegistrySmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
