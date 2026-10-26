#pragma once

// =============================================================================
// ZPlayer — 与平台无关的 Player 壳（P0-1 移动端出包链）
// -----------------------------------------------------------------------------
// 桌面 ZGame 与移动端（Android / iOS）共用同一套启动序列。区别只在于"谁驱动
// 主循环"：
//
//   桌面：GameEngine::Run() 里的 while 循环（窗口关闭或 RequestQuit 才退出）
//   移动端：宿主（GameActivity / UIKit）每帧回调 ZPlayer_Tick()，引擎不拥有
//           主循环，也不拥有窗口
//
// 所以这里暴露的是一组"生命周期 + 每帧"的 C 入口，让两端的宿主代码各自只写
// 平台胶水（JNI / ObjC），启动逻辑只有一份。
//
// 接口为什么是 C 而不是 C++：
//   * JNI 与 Swift/ObjC 桥接只需要一个稳定的 ABI；C++ 的名字改编与 STL 容器
//     跨编译器不可靠。
//   * 移动端最终 .so / App 是"引擎静态库 + 薄宿主"的形态，C 边界让宿主这一层
//     可以完全不用 C++（iOS 侧 App 壳可以是纯 ObjC/Swift）。
//
// 设计约束（照 GameEngine::Run() 抄，别自己发明）：
//   一帧 = Application::TickOneFrame(delta) 然后 GameEngine::Tick(delta)。
//   顺序不能反：Run() 的注释写明游戏层 Tick 必须晚于本帧的输入与物理。
// =============================================================================

#include <stdint.h>

// Windows 静态库不需要 dllexport（桌面 ZPlayer 是静态库，移动端也是被链进
// 最终 .so 的静态库），只有 Clang/GCC 需要显式放开可见性 —— 移动端构建开了
// -fvisibility=hidden（见 engine/CMakeLists.txt 的 _ZENGINE_MOBILE_PLATFORM
// 分支），不放开的话宿主 .so 里根本看不到这些符号。
#if defined(__GNUC__) || defined(__clang__)
    #define ZPLAYER_API __attribute__((visibility("default")))
#else
    #define ZPLAYER_API
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/// ZPlayer_Boot / ZPlayer_Tick 的返回值。0 表示成功/继续。
typedef enum ZPlayerResult
{
    ZPlayer_OK                 = 0,
    ZPlayer_NotBooted          = 1,  ///< 在未 Boot 的状态下调了需要 Boot 的接口
    ZPlayer_AlreadyBooted      = 2,  ///< 重复 Boot（引擎系统只能起一次）
    ZPlayer_BadArgument        = 3,  ///< desc 为空或没给 project_path
    ZPlayer_ProjectNotFound    = 4,  ///< project_path 不存在
    ZPlayer_WorkingDirFailed   = 5,  ///< current_path() 切不过去
    ZPlayer_GameEngineMissing  = 6,  ///< START_SYSTEM() 之后拿不到 GameEngine
    ZPlayer_StartupLevelFailed = 7,  ///< .zproject 配了 startup_level 但打不开
} ZPlayerResult;

/// 与 RegisterRuntime.h 的 PreferredRHI 一一对应（刻意重复一份：这个头文件
/// 要能被纯 C 宿主包含，不能引入那个枚举）。
typedef enum ZPlayerPreferredRHI
{
    ZPlayer_RHI_Default = 0,
    ZPlayer_RHI_DX12    = 1,
    ZPlayer_RHI_Vulkan  = 2,
} ZPlayerPreferredRHI;

typedef struct ZPlayerBootDesc
{
    /// 项目目录，或 .zproject 文件的路径（UTF-8）。必填。
    const char* project_path;

    /// 覆盖 .zproject 的 startup_level。可为空。
    const char* level_override;

    /// 覆盖启动 cvar 文件（默认 <project>/Config/ConsoleVariables.ini）。可为空。
    const char* cvar_file;

    /// 宿主给的可写目录（Android 的 getExternalFilesDir / iOS 的 Documents）。
    /// 打包后的游戏把日志与存档写这里；可为空表示用当前工作目录。
    const char* writable_dir;

    /// 宿主拥有的绘制表面：Android 是 ANativeWindow*，iOS 是 UIView*。
    /// 可为空 —— Android 上窗口往往在 surfaceCreated 回调里才到，那时用
    /// ZPlayer_SetNativeWindow() 补。引擎侧不拥有它，也不负责释放。
    void* native_window;

    /// Android 的 AAssetManager*（读 APK 内 assets 用）。其他平台传空。
    void* asset_manager;

    /// ZPlayerPreferredRHI 之一。
    int preferred_rhi;

    /// 初始表面尺寸；0 表示"宿主还没给"，由后续 ZPlayer_Resize() 决定。
    int surface_width;
    int surface_height;
} ZPlayerBootDesc;

/// 启动引擎并打开启动关卡。
///
/// 等价于桌面 ZGame 的 main()：装崩溃处理器 → 切工作目录 →
/// RegisterGameRuntime() → START_SYSTEM() → 应用启动 cvar → 挂载打包内容 →
/// 打开 startup_level。**不进入主循环** —— 主循环由宿主驱动 ZPlayer_Tick()。
ZPLAYER_API ZPlayerResult ZPlayer_Boot(const ZPlayerBootDesc* desc);

/// 推进一帧。
///
/// delta_seconds > 0 时用它；<= 0 时 Player 自己用 steady_clock 算（宿主只有
/// 时间戳、不想自己算差值时传 0 即可）。
///
/// 返回 1 = 继续跑；0 = 该退出了（TickOneFrame 返回 false 或 IsQuit()）。
/// 未 Boot / 已暂停时返回 1 且不推进任何东西 —— 宿主可以无脑每帧调用。
ZPLAYER_API int ZPlayer_Tick(float delta_seconds);

/// 补上或替换宿主窗口（Android surfaceCreated / surfaceDestroyed 传 nullptr）。
ZPLAYER_API void ZPlayer_SetNativeWindow(void* native_window);

/// 表面尺寸变化（旋转、分屏、iOS layout）。#17 会用它触发 swapchain 重建。
ZPLAYER_API void ZPlayer_Resize(int width, int height);

/// 触摸事件类型，与 WindowSystem::TouchAction 一一对应（刻意重复一份：这个
/// 头文件要能被纯 C / ObjC 宿主包含）。
typedef enum ZPlayerTouchAction
{
    ZPlayer_Touch_Down   = 0,
    ZPlayer_Touch_Move   = 1,
    ZPlayer_Touch_Up     = 2,
    /// 系统接管了这根手指（来电、下拉通知栏）。必须当作 Up 处理，否则引擎会
    /// 一直以为手指还按着，回到前台表现为"角色自己在走"。
    ZPlayer_Touch_Cancel = 3,
} ZPlayerTouchAction;

/// 推入一个触摸事件。
///
/// pointer_id：同一根手指的 down/move/up 必须用同一个 id（Android 用
/// AMotionEvent_getPointerId；iOS 的 UITouch 没有稳定整数 id，宿主自己分配）。
/// x / y：像素坐标，与鼠标光标同一个坐标空间 —— UI 命中测试直接可用。
///
/// 未 Boot 时是 no-op（宿主可能在引擎起来之前就送事件）。
ZPLAYER_API void ZPlayer_Touch(int pointer_id, int action, float x, float y);

/// 焦点变化。移动端进后台应传 0（配合 ZPlayer_Pause）。
ZPLAYER_API void ZPlayer_SetFocus(int focused);

/// 进后台：停止推进逻辑与渲染，但不拆引擎（Android onPause / iOS
/// applicationWillResignActive）。
ZPLAYER_API void ZPlayer_Pause(void);

/// 回前台：恢复推进，并重置帧计时，避免第一帧 delta 巨大把物理炸掉。
ZPLAYER_API void ZPlayer_Resume(void);

/// 请求退出。移动端没有"窗口关闭"这回事，退出只能靠这个。
ZPLAYER_API void ZPlayer_RequestQuit(void);

/// 拆掉整个引擎（SHUTDOWN_SYSTEM）。宿主销毁 Activity / App 时调用。
ZPLAYER_API void ZPlayer_Shutdown(void);

ZPLAYER_API int ZPlayer_IsRunning(void);
ZPLAYER_API int ZPlayer_IsPaused(void);

/// 给 RHI 层取当前表面（#16 Android 的 vkCreateAndroidSurfaceKHR、
/// #17 iOS 的 CAMetalLayer 都从这里拿）。
ZPLAYER_API void* ZPlayer_GetNativeWindow(void);
ZPLAYER_API void  ZPlayer_GetSurfaceSize(int* out_width, int* out_height);

/// Android 的 AAssetManager*（#16 用它把 APK 内 assets 接到 FileSystem）。
/// 非 Android 平台恒为空。
ZPLAYER_API void* ZPlayer_GetAssetManager(void);

/// 宿主给的可写目录（UTF-8，C 字符串，静态存储）；没给则是空串。
ZPLAYER_API const char* ZPlayer_GetWritableDir(void);

#ifdef __cplusplus
}  // extern "C"
#endif

// =============================================================================
// C++ 侧共享启动步骤
//
// 桌面 ZGame 与移动端 Player 壳共用，避免两份 MountPackagedContent /
// OpenStartupLevel 各自漂移。C++ 接口是因为它们要碰 ConsoleManager /
// AssetBundleManager / GameEngine，纯 C 宿主用不到。
// =============================================================================
#ifdef __cplusplus
namespace zengine::player
{
    /// 应用启动 cvar 文件。必须在 START_SYSTEM() 之后 —— 所有 cvar 注册完才
    /// 加载得进去。cvar_file 为空表示用默认路径。
    void ApplyStartupConsoleVariables(const char* cvar_file, bool has_cvar_file);

    /// 挂载 <project>/Build/<Platform>/Bundles 下的 .zbundle。
    /// 必须在打开启动关卡之前：关卡本身也是资源，晚挂载会让它按空目录解析。
    void MountPackagedContent();

    /// 解析并打开启动关卡：level_override 优先，其次 .zproject 的 startup_level。
    /// 没配关卡不算失败（GameEngine 照常起来，只是没有角色可接管）。
    bool OpenStartupLevel(const char* level_override);
}  // namespace zengine::player
#endif
