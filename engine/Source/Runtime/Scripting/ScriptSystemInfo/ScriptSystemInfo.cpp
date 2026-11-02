#include "ScriptSystemInfo.h"

#include "Runtime/Core/Base/Platform.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderSystem.h"

#include <EASTL/string.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

// 平台查询只在这一个 .cpp 里 —— 门面头文件保持干净（脚本侧只需要 eastl::string
// 与那三个枚举），平台头文件（windows.h / sysctl.h）不泄漏进 ScriptBinding。
#if defined(Z_PLATFORM_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS)
    #include <sys/sysctl.h>
    #include <sys/utsname.h>
    #include <unistd.h>
#else
    #include <sys/utsname.h>
    #include <unistd.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    #include <intrin.h>
    #define ZENGINE_CPUID_X86 1
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    #include <cpuid.h>
    #define ZENGINE_CPUID_X86 1
#endif

namespace
{
    // ---- 统一"拿不到"的哨兵值 ----
    //
    // 全部集中在这里，是为了让"这一项没数据"这件事在代码里只有一处定义。
    // 散在各处的话，某个函数迟早会返回 1 而不是 0，而 1 会被脚本当成真值用。
    constexpr const char* kUnknown       = "Unknown";
    constexpr int         kUnknownSize   = 0;
    constexpr float       kUnknownLevel  = -1.0f;

    eastl::string Format(const char* fmt, ...)
    {
        char buffer[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        return eastl::string(buffer);
    }

    // RHI 在 headless（CI / cooker / 专用服务器）里根本不存在 —— 那里
    // GetSystem 返回 nullptr。每次都判空比"假设它在"安全：后者会在没有渲染
    // 系统的进程里解引用空指针。
    RHI* TryGetRHI()
    {
        RenderSystem* render_system = GET_SYSTEM(RenderSystem);
        if (render_system == nullptr)
        {
            return nullptr;
        }
        return render_system->GetRHI();
    }

    // 拿不到就返回 false 的 props 查询。调用方只看 bool，不用再判 nullptr。
    bool TryGetProps(RHIPhysicalDeviceProperties& out_props)
    {
        RHI* rhi = TryGetRHI();
        if (rhi == nullptr)
        {
            return false;
        }
        out_props = RHIPhysicalDeviceProperties {};
        rhi->GetPhysicalDeviceProperties(&out_props);
        return true;
    }

    const char* ArchSuffix()
    {
        return sizeof(void*) == 8 ? "64bit" : "32bit";
    }

    // PCI vendor ID -> 厂商名。只列 Unity 用户真正会遇到的几家；其余交给
    // GetGraphicsDeviceVendor 拼 "Unknown (0xXXXX)"。
    const char* VendorNameFromID(uint32_t vendor_id)
    {
        switch (vendor_id)
        {
            case 0x1002:
                return "AMD";
            case 0x1010:
                return "ImgTec";
            case 0x10DE:
                return "NVIDIA";
            case 0x13B5:
                return "ARM";
            case 0x5143:
                return "Qualcomm";
            case 0x8086:
                return "Intel";
            default:
                return nullptr;
        }
    }

    // Vulkan 的 apiVersion 是 VK_MAKE_VERSION 打包的：高 10 位 major、中 10 位
    // minor、低 12 位 patch。直接打原始数字没人看得懂。
    eastl::string VulkanVersionString(uint32_t api_version)
    {
        const uint32_t major = (api_version >> 22) & 0x3FF;
        const uint32_t minor = (api_version >> 12) & 0x3FF;
        const uint32_t patch = api_version & 0xFFF;
        return Format("Vulkan %u.%u.%u", major, minor, patch);
    }

    // 主机信息（hostname / 计算机名）。
    eastl::string QueryHostName()
    {
#if defined(Z_PLATFORM_WINDOWS)
        char  buffer[256];
        DWORD size = static_cast<DWORD>(sizeof(buffer));
        if (GetComputerNameA(buffer, &size) && size > 0)
        {
            return eastl::string(buffer);
        }
        return eastl::string(kUnknown);
#else
        char buffer[256];
        if (::gethostname(buffer, sizeof(buffer)) == 0)
        {
            buffer[sizeof(buffer) - 1] = '\0';
            return eastl::string(buffer);
        }
        return eastl::string(kUnknown);
#endif
    }

    int QuerySystemMemoryMB()
    {
#if defined(Z_PLATFORM_WINDOWS)
        MEMORYSTATUSEX state;
        state.dwLength = sizeof(state);
        if (GlobalMemoryStatusEx(&state))
        {
            return static_cast<int>(state.ullTotalPhys / (1024 * 1024));
        }
        return kUnknownSize;
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS)
        int64_t    memory = 0;
        std::size_t size  = sizeof(memory);
        if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0 && memory > 0)
        {
            return static_cast<int>(memory / (1024 * 1024));
        }
        return kUnknownSize;
#else
        const long page_size  = sysconf(_SC_PAGESIZE);
        const long page_count = sysconf(_SC_PHYS_PAGES);
        if (page_size > 0 && page_count > 0)
        {
            // 用 64 位中间量：page_size * page_count 在 32 位 long 上会溢出
            // （8GB 机器在 32 位 Linux 上就是典型受害者）。
            const int64_t bytes = static_cast<int64_t>(page_size) * static_cast<int64_t>(page_count);
            return static_cast<int>(bytes / (1024 * 1024));
        }
        return kUnknownSize;
#endif
    }

    eastl::string QueryProcessorBrand()
    {
#if defined(ZENGINE_CPUID_X86)
        // CPUID 的 brand string 是 3 组 4 个 32 位寄存器，共 48 字节，厂商自己
        // 用空格补齐 —— 所以末尾要手动收尾。
        int   regs[4] = {0, 0, 0, 0};
        char  brand[49] = {0};
    #if defined(_MSC_VER)
        __cpuid(regs, 0x80000000);
        if (static_cast<unsigned>(regs[0]) < 0x80000004u)
        {
            return eastl::string(kUnknown);
        }
        __cpuid(reinterpret_cast<int*>(brand) + 0, 0x80000002);
        __cpuid(reinterpret_cast<int*>(brand) + 4, 0x80000003);
        __cpuid(reinterpret_cast<int*>(brand) + 8, 0x80000004);
    #else
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (!__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) || eax < 0x80000004u)
        {
            return eastl::string(kUnknown);
        }
        __get_cpuid(0x80000002u, reinterpret_cast<unsigned int*>(brand) + 0, &ebx, &ecx, &edx);
        __get_cpuid(0x80000003u, reinterpret_cast<unsigned int*>(brand) + 4, &ebx, &ecx, &edx);
        __get_cpuid(0x80000004u, reinterpret_cast<unsigned int*>(brand) + 8, &ebx, &ecx, &edx);
    #endif
        brand[48] = '\0';
        eastl::string result(brand);
        // 去掉前导空格（AMD 的串左边常带空格）。
        std::size_t first = result.find_first_not_of(' ');
        if (first == eastl::string::npos)
        {
            return eastl::string(kUnknown);
        }
        return result.substr(first);
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS)
        char       brand[256] = {0};
        std::size_t size      = sizeof(brand);
        if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0 && brand[0] != '\0')
        {
            return eastl::string(brand);
        }
        return eastl::string(kUnknown);
#else
        // ARM Linux / Android / 鸿蒙：/proc/cpuinfo 的格式各内核不一样，没有
        // 稳定可解析的字段。留空好过猜。
        return eastl::string(kUnknown);
#endif
    }

    eastl::string QueryOperatingSystem()
    {
#if defined(Z_PLATFORM_WINDOWS)
        return Format("Windows (%s)", ArchSuffix());
#else
        utsname info {};
        if (::uname(&info) == 0)
        {
            return Format("%s %s (%s)", info.sysname, info.release, ArchSuffix());
        }
        return Format("POSIX (%s)", ArchSuffix());
#endif
    }
}  // namespace

// =============================================================================
// 设备
// =============================================================================

eastl::string SystemInfo::GetDeviceName()
{
    return QueryHostName();
}

eastl::string SystemInfo::GetDeviceModel()
{
    // 桌面上 Unity 给主板 / 机型标识，ZEngine 现在读不到（见头文件注释）。
    return eastl::string(kUnknown);
}

DeviceType SystemInfo::GetDeviceType()
{
#if defined(Z_PLATFORM_WINDOWS) || defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_LINUX)
    return DeviceType::Desktop;
#else
    return DeviceType::Handheld;
#endif
}

eastl::string SystemInfo::GetDeviceUniqueIdentifier()
{
    const char* platform = "UnknownPlatform";
#if defined(Z_PLATFORM_WINDOWS)
    platform = "Windows";
#elif defined(Z_PLATFORM_MACOS)
    platform = "MacOS";
#elif defined(Z_PLATFORM_LINUX)
    platform = "Linux";
#elif defined(Z_PLATFORM_ANDROID)
    platform = "Android";
#elif defined(Z_PLATFORM_IOS)
    platform = "iOS";
#elif defined(Z_PLATFORM_OHOS)
    platform = "HarmonyOS";
#endif
    return Format("%s-%s-%d", platform, QueryHostName().c_str(), GetProcessorCount());
}

eastl::string SystemInfo::GetOperatingSystem()
{
    return QueryOperatingSystem();
}

OperatingSystemFamily SystemInfo::GetOperatingSystemFamily()
{
#if defined(Z_PLATFORM_WINDOWS)
    return OperatingSystemFamily::Windows;
#elif defined(Z_PLATFORM_MACOS) || defined(Z_PLATFORM_IOS)
    return OperatingSystemFamily::MacOSX;
#elif defined(Z_PLATFORM_LINUX) || defined(Z_PLATFORM_ANDROID) || defined(Z_PLATFORM_OHOS)
    return OperatingSystemFamily::Linux;
#else
    return OperatingSystemFamily::Other;
#endif
}

// =============================================================================
// CPU / 内存
// =============================================================================

eastl::string SystemInfo::GetProcessorType()
{
    return QueryProcessorBrand();
}

int SystemInfo::GetProcessorCount()
{
    const unsigned int count = std::thread::hardware_concurrency();
    // hardware_concurrency 在实现拿不到时返回 0 —— 那是"不知道"，不是"单核"。
    return static_cast<int>(count);
}

int SystemInfo::GetSystemMemorySize()
{
    return QuerySystemMemoryMB();
}

// =============================================================================
// 图形设备
// =============================================================================

eastl::string SystemInfo::GetGraphicsDeviceName()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return eastl::string(kUnknown);
    }
    // deviceName 是定长 char 数组，驱动不保证以 '\0' 结尾（Vulkan 规定
    // VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 内**必须**有终止符，但 DX12 的
    // adapter description 走的是另一条路）。手动限长最稳。
    char name[RHI_MAX_PHYSICAL_DEVICE_NAME_SIZE + 1] = {0};
    std::strncpy(name, props.deviceName, RHI_MAX_PHYSICAL_DEVICE_NAME_SIZE);
    if (name[0] == '\0')
    {
        return eastl::string(kUnknown);
    }
    return eastl::string(name);
}

eastl::string SystemInfo::GetGraphicsDeviceVendor()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return eastl::string(kUnknown);
    }
    if (const char* known = VendorNameFromID(props.vendorID))
    {
        return eastl::string(known);
    }
    // 不认识的厂商：保留十六进制 ID 而不是丢掉 —— 上报日志里 0x1414 这种
    // 值往往比 "Unknown" 更有用（0x1414 = Microsoft 基本渲染驱动）。
    return Format("Unknown (0x%04X)", static_cast<unsigned>(props.vendorID));
}

int SystemInfo::GetGraphicsDeviceVendorID()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return kUnknownSize;
    }
    return static_cast<int>(props.vendorID);
}

int SystemInfo::GetGraphicsDeviceID()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return kUnknownSize;
    }
    return static_cast<int>(props.deviceID);
}

eastl::string SystemInfo::GetGraphicsDeviceVersion()
{
    RHI* rhi = TryGetRHI();
    if (rhi == nullptr)
    {
        return eastl::string(kUnknown);
    }
    RHIPhysicalDeviceProperties props {};
    rhi->GetPhysicalDeviceProperties(&props);

    switch (rhi->getGraphicsAPI())
    {
        case GraphicsAPI::Vulkan:
            return VulkanVersionString(props.apiVersion);
        case GraphicsAPI::DirectX12:
            return eastl::string("Direct3D 12");
        case GraphicsAPI::Metal:
            return eastl::string("Metal");
        case GraphicsAPI::WebGL2:
            return eastl::string("WebGL 2.0");
        default:
            return eastl::string(kUnknown);
    }
}

int SystemInfo::GetGraphicsMemorySize()
{
    // RHI 抽象层没有 GetPhysicalDeviceMemoryProperties，见头文件注释。
    return kUnknownSize;
}

int SystemInfo::GetGraphicsShaderLevel()
{
    RHI* rhi = TryGetRHI();
    if (rhi == nullptr)
    {
        return kUnknownSize;
    }
    // Unity 用着色器模型的两位数字（50 = SM5.0）。ZEngine 的着色器是
    // GLSL/HLSL 双份，实际能力由后端决定，这里给的是"这一档后端等价于
    // Unity 的哪一档"。
    switch (rhi->getGraphicsAPI())
    {
        case GraphicsAPI::DirectX12:
            return 51;
        case GraphicsAPI::Vulkan:
        case GraphicsAPI::Metal:
            return 50;
        case GraphicsAPI::WebGL2:
            return 30;
        default:
            return kUnknownSize;
    }
}

// =============================================================================
// 能力上限
// =============================================================================

int SystemInfo::GetMaxTextureSize()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return kUnknownSize;
    }
    return static_cast<int>(props.limits.maxImageDimension2D);
}

int SystemInfo::GetMaxCubemapSize()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return kUnknownSize;
    }
    return static_cast<int>(props.limits.maxImageDimensionCube);
}

int SystemInfo::GetMaxTextureArraySize()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return kUnknownSize;
    }
    return static_cast<int>(props.limits.maxImageArrayLayers);
}

// =============================================================================
// 能力开关
// =============================================================================

bool SystemInfo::GetSupportsInstancing()
{
    // 所有现代后端都支持实例化。判据是"RHI 在不在"而不是永远 true：headless
    // 下画不了东西，"支持"是句空话。
    return TryGetRHI() != nullptr;
}

bool SystemInfo::GetSupportsShadows()
{
    return TryGetRHI() != nullptr;
}

bool SystemInfo::GetSupportsComputeShaders()
{
    RHI* rhi = TryGetRHI();
    if (rhi == nullptr)
    {
        return false;
    }
    return rhi->getGraphicsAPI() != GraphicsAPI::WebGL2;
}

bool SystemInfo::GetSupports3DTextures()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return false;
    }
    return props.limits.maxImageDimension3D > 0;
}

bool SystemInfo::GetSupportsTextureArrays()
{
    RHIPhysicalDeviceProperties props {};
    if (!TryGetProps(props))
    {
        return false;
    }
    return props.limits.maxImageArrayLayers > 1;
}

// =============================================================================
// 电池
// =============================================================================

float SystemInfo::GetBatteryLevel()
{
    // 桌面平台没有电池遥测接入。返回 Unity 的"不支持"哨兵 -1，而不是 0 或 1
    // —— 0 会被当成"没电了"，1 会被当成"满电"，两者都是假信息。
    return kUnknownLevel;
}

BatteryStatus SystemInfo::GetBatteryStatus()
{
    return BatteryStatus::Unknown;
}
