#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <EASTL/string.h>

#include "ScriptSystemInfo.generated.h"

// 脚本侧的机器 / 图形设备信息（Unity 的 `UnityEngine.SystemInfo` 静态类）。
//
// 为什么这个类的返回值**大多数可能拿不到真值**
// ------------------------------------------------
// Unity 的 `SystemInfo` 是"播放器对自己寄主的描述"：它读 OS 的系统调用、读图形
// 驱动的查询接口，全部在**有窗口、有设备**的前提下才成立。ZEngine 的 Runtime
// 还要能在 headless（CI、cooker、专用服务器）里跑 —— 那里没有窗口，也没有 RHI，
// `GET_SYSTEM(RenderSystem)` 返回 nullptr、设备属性根本无从问起。
//
// 这里的取舍是：**一律返回"拿不到"的哨兵值，不编造 plausible 的假值**。
//   * 字符串 => `"Unknown"`
//   * 整数尺寸 / 条数 => 0
//   * `batteryLevel` => -1（Unity 在"不支持"时也是 -1）
//   * `supportsXxx` => false（宁可说"我不知道"，也不说"我支持"）
//
// 判据：脚本最常见的用法是 `if (SystemInfo.supportsInstancing)`、`if
// (SystemInfo.graphicsMemorySize > 512)`。给假值会让这类判断在真机上静默走错
// 分支 —— 那比"这个 API 在这台机器上不可用"难查得多。
//
// 没有 `graphicsMemorySize`
// -------------------------
// Unity 的这一项读显存总量。RHI 抽象层只有 `GetPhysicalDeviceProperties()`，
// **没有** `GetPhysicalDeviceMemoryProperties()`（见
// `Function/Render/Interface/RHI.h`）—— Vulkan 那边本来是有的
// （`vkGetPhysicalDeviceMemoryProperties`），只是没往上抬。所以这一项恒 0。
// 要补的时候从给 RHI 加 `GetPhysicalDeviceMemoryProperties` 开始，而不是在
// 这里靠 `limits` 猜一个。
//
// 静态属性：与 `Screen.width` / `Application.dataPath` 同一套机制
// ---------------------------------------------------------------
// Unity 的这些全是静态属性，走 pesapi 的 `Variables` 表装到构造器上。Get* 方法
// 在 C++ 里保留（ZSCRIPTPROPERTY 的 getter 本来就得是个 ZFUNCTION），但**不在
// .d.ts 里** —— 静态访问器会被 codegen 从方法表摘掉，否则 MSVC 把同一个
// MethodCallback 编进两个 COMDAT 段（LNK1179）。

// Unity 的 `UnityEngine.DeviceType`。取值与 Unity 一致，便于搬代码。
ZENUM()
enum class DeviceType : int
{
    Unknown  = 0,
    Handheld = 1,
    Console  = 2,
    Desktop  = 3,
};

// Unity 的 `UnityEngine.OperatingSystemFamily`。
ZENUM()
enum class OperatingSystemFamily : int
{
    Other   = 0,
    MacOSX  = 1,
    Windows = 2,
    Linux   = 3,
};

// Unity 的 `UnityEngine.BatteryStatus`。
ZENUM()
enum class BatteryStatus : int
{
    Unknown     = 0,
    Charging    = 1,
    Discharging = 2,
    NotCharging = 3,
    Full        = 4,
};

ZCLASS()
class SystemInfo : public Object
{
    ZGENERATED_BODY(SystemInfo)

public:
    // ---- 设备 ----

    /// 机器名（Windows 的计算机名 / POSIX 的 hostname）。
    /// 拿不到时返回 `"Unknown"`。Unity 的 `SystemInfo.deviceName`。
    ZFUNCTION()
    static eastl::string GetDeviceName();
    ZSCRIPTPROPERTY(Name = "deviceName", Getter = "GetDeviceName")

    /// 设备型号。
    ///
    /// ⚠️ 桌面平台上**恒为 `"Unknown"`**：Unity 在这一项上给的是主板 / 机型
    /// 标识（`"MacBookPro16,1"` 那种），要读它需要 SMBIOS（Windows）或
    /// `hw.model` sysctl（macOS）—— 两条路都不在现在的 Runtime 里。宁可留空
    /// 也不拿 hostname 冒充：后者会让"按机型做画质分档"的代码静默错判。
    ZFUNCTION()
    static eastl::string GetDeviceModel();
    ZSCRIPTPROPERTY(Name = "deviceModel", Getter = "GetDeviceModel")

    /// 设备大类。按编译期平台判定（桌面 / 手持），不查硬件。
    ZFUNCTION()
    static DeviceType GetDeviceType();
    ZSCRIPTPROPERTY(Name = "deviceType", Getter = "GetDeviceType")

    /// 一个尽量稳定的本机标识串。
    ///
    /// ⚠️ **不是** Unity 语义上的"设备唯一 ID" —— Unity 手上的是平台给的
    /// 硬件标识（IDFV / ANDROID_ID 一类）。这里给的是
    /// `<平台名>-<机器名>-<逻辑处理器数>` 的拼接：它跨进程稳定、跨重启稳定，
    /// 但**同型号同名的两台机器会得到同一个值**。
    /// 用在"存档绑机器"这类防伪场景之前先看清楚这一点；给崩溃上报做个分组键
    /// 是它正当的用途。
    ZFUNCTION()
    static eastl::string GetDeviceUniqueIdentifier();
    ZSCRIPTPROPERTY(Name = "deviceUniqueIdentifier", Getter = "GetDeviceUniqueIdentifier")

    // ---- 操作系统 ----

    /// 操作系统描述串，例如 `"Windows (64bit)"` / `"Linux 6.6 (64bit)"`。
    /// Unity 的 `SystemInfo.operatingSystem`。
    ZFUNCTION()
    static eastl::string GetOperatingSystem();
    ZSCRIPTPROPERTY(Name = "operatingSystem", Getter = "GetOperatingSystem")

    /// 操作系统族。按编译期平台判定。
    ZFUNCTION()
    static OperatingSystemFamily GetOperatingSystemFamily();
    ZSCRIPTPROPERTY(Name = "operatingSystemFamily", Getter = "GetOperatingSystemFamily")

    // ---- CPU / 内存 ----

    /// CPU 品牌串（x86 的 CPUID brand string，Apple Silicon 的
    /// `machdep.cpu.brand_string`）。拿不到时 `"Unknown"`。
    ZFUNCTION()
    static eastl::string GetProcessorType();
    ZSCRIPTPROPERTY(Name = "processorType", Getter = "GetProcessorType")

    /// 逻辑处理器数。拿不到时 0（不是 1 —— 1 会被当成"单核机器"用）。
    ZFUNCTION()
    static int GetProcessorCount();
    ZSCRIPTPROPERTY(Name = "processorCount", Getter = "GetProcessorCount")

    /// 物理内存总量（MB）。拿不到时 0。Unity 的 `SystemInfo.systemMemorySize`。
    ZFUNCTION()
    static int GetSystemMemorySize();
    ZSCRIPTPROPERTY(Name = "systemMemorySize", Getter = "GetSystemMemorySize")

    // ---- 图形设备 ----

    /// 图形设备名（Vulkan / DX12 的 deviceName）。无 RHI 时 `"Unknown"`。
    ZFUNCTION()
    static eastl::string GetGraphicsDeviceName();
    ZSCRIPTPROPERTY(Name = "graphicsDeviceName", Getter = "GetGraphicsDeviceName")

    /// 图形设备厂商名。由 vendorID 查一张已知 ID 的表；不认识时返回
    /// `"Unknown (0xXXXX)"` —— 保留十六进制比丢掉信息有用。
    ZFUNCTION()
    static eastl::string GetGraphicsDeviceVendor();
    ZSCRIPTPROPERTY(Name = "graphicsDeviceVendor", Getter = "GetGraphicsDeviceVendor")

    /// PCI vendor ID（0x10DE = NVIDIA 等）。无 RHI 时 0。
    ZFUNCTION()
    static int GetGraphicsDeviceVendorID();
    ZSCRIPTPROPERTY(Name = "graphicsDeviceVendorID", Getter = "GetGraphicsDeviceVendorID")

    /// PCI device ID。无 RHI 时 0。
    ZFUNCTION()
    static int GetGraphicsDeviceID();
    ZSCRIPTPROPERTY(Name = "graphicsDeviceID", Getter = "GetGraphicsDeviceID")

    /// 图形 API 版本串，如 `"Vulkan 1.3.275"` / `"Direct3D 12"`。
    ///
    /// ⚠️ 是**驱动报告的 API 版本**，不是着色器模型。要判能力用
    /// `graphicsShaderLevel` 或 `supportsXxx`。
    ZFUNCTION()
    static eastl::string GetGraphicsDeviceVersion();
    ZSCRIPTPROPERTY(Name = "graphicsDeviceVersion", Getter = "GetGraphicsDeviceVersion")

    /// 显存总量（MB）。**恒为 0** —— RHI 抽象层没有
    /// `GetPhysicalDeviceMemoryProperties`（见文件头的说明）。
    ZFUNCTION()
    static int GetGraphicsMemorySize();
    ZSCRIPTPROPERTY(Name = "graphicsMemorySize", Getter = "GetGraphicsMemorySize")

    /// 着色器能力等级，用 Unity 的同一套数字：50 = SM5.0，51 = SM5.1。
    /// 无 RHI 时 0。
    ZFUNCTION()
    static int GetGraphicsShaderLevel();
    ZSCRIPTPROPERTY(Name = "graphicsShaderLevel", Getter = "GetGraphicsShaderLevel")

    // ---- 图形能力上限 ----

    /// 2D 纹理最大边长。无 RHI 时 0。
    ZFUNCTION()
    static int GetMaxTextureSize();
    ZSCRIPTPROPERTY(Name = "maxTextureSize", Getter = "GetMaxTextureSize")

    /// Cubemap 最大边长。
    ZFUNCTION()
    static int GetMaxCubemapSize();
    ZSCRIPTPROPERTY(Name = "maxCubemapSize", Getter = "GetMaxCubemapSize")

    /// 数组纹理最大层数。
    ZFUNCTION()
    static int GetMaxTextureArraySize();
    ZSCRIPTPROPERTY(Name = "maxTextureArraySize", Getter = "GetMaxTextureArraySize")

    // ---- 能力开关 ----
    //
    // 全部"无 RHI 即为 false"，理由见文件头：不知道就说不知道。

    /// 是否支持 GPU 实例化。
    ZFUNCTION()
    static bool GetSupportsInstancing();
    ZSCRIPTPROPERTY(Name = "supportsInstancing", Getter = "GetSupportsInstancing")

    /// 是否支持阴影。引擎有阴影通路，但仍要求 RHI 在位 —— headless 下画不出
    /// 来，说 true 没有意义。
    ZFUNCTION()
    static bool GetSupportsShadows();
    ZSCRIPTPROPERTY(Name = "supportsShadows", Getter = "GetSupportsShadows")

    /// 是否支持 compute shader。WebGL2 上为 false。
    ZFUNCTION()
    static bool GetSupportsComputeShaders();
    ZSCRIPTPROPERTY(Name = "supportsComputeShaders", Getter = "GetSupportsComputeShaders")

    /// 是否支持 3D 纹理。
    ZFUNCTION()
    static bool GetSupports3DTextures();
    ZSCRIPTPROPERTY(Name = "supports3DTextures", Getter = "GetSupports3DTextures")

    /// 是否支持 2D 数组纹理。
    ZFUNCTION()
    static bool GetSupportsTextureArrays();
    ZSCRIPTPROPERTY(Name = "supportsTextureArrays", Getter = "GetSupportsTextureArrays")

    // ---- 电池 ----

    /// 电量 [0,1]。桌面平台**恒为 -1**（Unity 在"不支持"时也返回 -1，这样
    /// `if (batteryLevel < 0.2f)` 会被 -1 命中成"低电量"，所以判之前先看
    /// 符号 —— 这与 Unity 的坑一致，有意保留）。
    ZFUNCTION()
    static float GetBatteryLevel();
    ZSCRIPTPROPERTY(Name = "batteryLevel", Getter = "GetBatteryLevel")

    /// 电池状态。桌面平台恒 `Unknown`。
    ZFUNCTION()
    static BatteryStatus GetBatteryStatus();
    ZSCRIPTPROPERTY(Name = "batteryStatus", Getter = "GetBatteryStatus")
};
