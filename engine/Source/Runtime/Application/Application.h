#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Log/LogSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

#include "Application.generated.h"

extern bool g_isPlaying;
extern std::unordered_set<std::string> g_editorTickComponentTypes;

// 编辑模式下由 ZEditor 置 true（和 g_isPlaying 一样的机制：Runtime 不能依赖
// Editor 模块，所以只能是一个全局量 + 编辑器侧写入）。Player 壳不碰它，恒为
// false。脚本侧读的是 `Application.GetIsEditor()`。
extern bool g_isEditor;

// 脚本可见的运行平台。Unity 的 `RuntimePlatform`，取值顺序与之一致 —— 这样
// 从 Unity 搬过来的 `if (Application.platform == RuntimePlatform.WindowsEditor)`
// 这类判断不用改。
//
// "Editor" 与 "Player" 分开：ZEngine 的编辑器和 Player 是同一个 Runtime 库，
// 编译期区分不了，所以由 `g_isEditor` 在运行时选。
ZENUM()
enum class RuntimePlatform : int
{
    Unknown       = 0,
    WindowsEditor = 1,
    WindowsPlayer = 2,
    OSXEditor     = 3,
    OSXPlayer     = 4,
    LinuxEditor   = 5,
    LinuxPlayer   = 6,
    Android       = 7,
    IPhonePlayer  = 8,
    WebGLPlayer   = 9,
    /// Unity 没有鸿蒙这一档（它的 RuntimePlatform 里没有），ZEngine 有，所以
    /// 值排在 Unity 之后，不影响搬过来的代码。
    HarmonyOSPlayer = 10,
};

// ZCLASS 挂在已有的引擎 Application 上（而不是另建一个 `class Application`）：
// 脚本侧要的就是 `Application.dataPath` 这类拼写，而同名 C++ 类已经存在。
// 两个同名的类放进同一个二进制会违反 ODR —— 链接器挑一个定义，另一处静默用错
// 布局。ZHeaderTool 取基类列表的第一项（parser.py 在顶层逗号处断开），所以
// `public Object, public IEngineSystem` 里的第二个基类不影响反射。
ZCLASS()
class Application : public Object, public IEngineSystem
{
    ZGENERATED_BODY(Application)

public:
    friend class Editor;

    static const float s_Fpsalpha;

public:
    std::string GetName() const override { return GET_CLASS_NAME(Application); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Core; }
    std::vector<std::type_index> GetDependencies() const override;
    bool Initialize() override;
    void Shutdown() override;

    bool IsQuit() const { return m_Isquit; }
    void RequestQuit() { m_Isquit = true; }
    void Run();
    bool TickOneFrame(float deltaTime);
    bool TickOneFrame(float simulationDeltaTime, float frameDeltaTime, bool shouldUpdateLogic);

    int GetFps() const { return m_Fps; }
    bool MayUpdate() const { return m_DisabllowUpdating == 0; }

    // =====================================================================
    // 脚本门面（Unity 的 `Application` 静态类）
    // =====================================================================
    //
    // 全部是 static：脚本侧写 `Application.Quit()`，没有实例可拿。
    //
    // 为什么现在是 `dataPath` 而不是 `GetDataPath()`
    // ----------------------------------------------
    // 以前 ZSCRIPTPROPERTY 的 getter 不允许 static（zht/parser.py 直接报错），
    // Unity 的静态属性一律只能落成 Get/Set 方法对。`Color.red` 那一档把静态属性
    // 做出来了（走 pesapi 的 `Variables` 表，装到构造器上），于是这里换回 Unity
    // 的拼写。Get* 方法在 C++ 里照旧保留（ZSCRIPTPROPERTY 的 getter 本来就得是
    // 个 ZFUNCTION），但**不在 .d.ts 里** —— 静态访问器会被 codegen 从方法表
    // 摘掉，否则 MSVC 把同一个 MethodCallback 编进两个 COMDAT 段（LNK1179）。
    //
    // 名字冲突注意
    // ------------
    // 静态成员函数与同参数列表的非静态成员函数不能重载（C++ 禁止），所以这里
    // 没有 `GetFps()` / `GetName()` —— 它们已经被实例方法占了。脚本要帧率用
    // `Time.GetFps()`。

    /// 请求退出。**不是立即退出**：只置位，当前帧照常跑完（等同 Unity 在帧末
    /// 退出）。Player 主循环下一轮 `ShouldClose()` 为真才真正返回。
    ZFUNCTION()
    static void Quit();

    /// 是否正在 Play。编辑器里 Enter Play 之后为 true，编辑态为 false；
    /// Player 壳恒为 true。Unity 的 `Application.isPlaying`。
    ZFUNCTION()
    static bool GetIsPlaying() { return g_isPlaying; }
    ZSCRIPTPROPERTY(Name = "isPlaying", Getter = "GetIsPlaying")

    /// 是否在编辑器里运行。Unity 的 `Application.isEditor`。
    ZFUNCTION()
    static bool GetIsEditor() { return g_isEditor; }
    ZSCRIPTPROPERTY(Name = "isEditor", Getter = "GetIsEditor")

    /// 工程资源目录（`<project>/<content_dir>`，通常是 Assets）。
    /// Unity 的 `Application.dataPath`。
    ///
    /// 没有打开工程时返回空串 —— 这是"独立 Player 用 exe 名当产品名"那一条的
    /// 同一支：无工程启动是合法状态，让调用方自己判空比返回个假路径好。
    ZFUNCTION()
    static std::string GetDataPath();
    ZSCRIPTPROPERTY(Name = "dataPath", Getter = "GetDataPath")

    /// 工程根目录（.zproject 所在目录）。无工程时为空。
    ZFUNCTION()
    static std::string GetProjectPath();

    /// 只读的随包资源目录（`<dataPath>/StreamingAssets`）。
    /// Unity 的 `Application.streamingAssetsPath`。
    ///
    /// ⚠️ 只返回路径，**不保证目录存在** —— 工程没有建这个目录时它就是一串
    /// 拼出来的路径。要"存在"得自己 mkdir 或改打包流程。
    ZFUNCTION()
    static std::string GetStreamingAssetsPath();
    ZSCRIPTPROPERTY(Name = "streamingAssetsPath", Getter = "GetStreamingAssetsPath")

    /// 可写的持久化目录（跨启动保留，卸载才清）。
    /// Unity 的 `Application.persistentDataPath`。
    ///
    /// 取自 UserPreferences 的用户目录（Windows 是
    /// `%LOCALAPPDATA%/<Company>/<Product>`，macOS 是
    /// `~/Library/Application Support/<Product>`）—— 与偏好设置文件同一个
    /// 地方，这样"存档"和"设置"不会各占一个目录。
    ZFUNCTION()
    static std::string GetPersistentDataPath();
    ZSCRIPTPROPERTY(Name = "persistentDataPath", Getter = "GetPersistentDataPath")

    /// 产品名。Unity 的 `Application.productName`。
    ZFUNCTION()
    static std::string GetProductName();
    ZSCRIPTPROPERTY(Name = "productName", Getter = "GetProductName")

    /// 公司名。Unity 的 `Application.companyName`。
    ZFUNCTION()
    static std::string GetCompanyName();
    ZSCRIPTPROPERTY(Name = "companyName", Getter = "GetCompanyName")

    /// 版本号字符串。Unity 的 `Application.version`。
    ZFUNCTION()
    static std::string GetVersion();
    ZSCRIPTPROPERTY(Name = "version", Getter = "GetVersion")

    /// 引擎版本。Unity 的 `Application.unityVersion`（对位 `engineVersion`）。
    ZFUNCTION()
    static std::string GetEngineVersion();
    /// Unity 里叫 `unityVersion`。ZEngine 没有 "Unity"，所以 .d.ts 里用
    /// `engineVersion` 这个更诚实的名字，但两边都指向同一个 getter 的**不同
    /// 转发函数**（一个静态访问器只能挂一个属性，见 ScriptTime.h 的
    /// `GetRealtimeSinceStartup`）。
    ZFUNCTION()
    static std::string GetEngineVersionAlias() { return GetEngineVersion(); }
    ZSCRIPTPROPERTY(Name = "unityVersion", Getter = "GetEngineVersionAlias")

    /// 当前运行平台。Unity 的 `Application.platform`。
    ZFUNCTION()
    static RuntimePlatform GetPlatform();
    ZSCRIPTPROPERTY(Name = "platform", Getter = "GetPlatform")

    /// 平台名（"Windows" / "MacOS" / "Linux" / "Android" / "iOS" / "Web"）。
    /// 打日志 / 上报时用，`GetPlatform()` 判分支用。
    ZFUNCTION()
    static std::string GetPlatformName();

    /// 目标帧率。<= 0 表示不限（跟随垂直同步）。
    /// Unity 的 `Application.targetFrameRate`。
    ///
    /// 只在 `Run()` 里生效（Player 主循环）。编辑器有自己的驱动方式，
    /// 那里不受这个值约束 —— 限帧是"出货行为"，不是"编辑行为"。
    ZFUNCTION()
    static int GetTargetFrameRate();
    /// Unity 的 `Application.targetFrameRate` 可写。
    ZSCRIPTPROPERTY(Name = "targetFrameRate", Getter = "GetTargetFrameRate",
                    Setter = "SetTargetFrameRate")

    /// 设置目标帧率。<= 0 关闭限帧。
    ZFUNCTION()
    static void SetTargetFrameRate(int frame_rate);

    /// 失焦时是否继续跑。Unity 的 `Application.runInBackground`。
    ZFUNCTION()
    static bool GetRunInBackground();
    /// Unity 的 `Application.runInBackground` 也可写。
    ZSCRIPTPROPERTY(Name = "runInBackground", Getter = "GetRunInBackground",
                    Setter = "SetRunInBackground")

    /// 设置失焦行为。false 时窗口失焦就停止推进世界（但消息泵照跑，否则
    /// 窗口会变成"未响应"）。
    ZFUNCTION()
    static void SetRunInBackground(bool run);

protected:
    void LogicalTick(float deltaTime);
    bool RendererTick(float delta_time);

    /// 按 `m_TargetFrameRate` 睡掉本帧剩余预算。`frame_start` 是本帧开始时刻
    /// （限帧算的是"整帧成本"，不只是逻辑）。<= 0 时直接返回。
    void LimitFrameRate(std::chrono::steady_clock::time_point frame_start);

    void CalculateFPS(float delta_time);

    /**
     *  Each frame can only be called once
     */
    float CalculateDeltaTime();

    bool m_Isquit {false};

    std::chrono::steady_clock::time_point m_LastTickTimePoint {std::chrono::steady_clock::now()};

    float m_AverageDuration {0.f};
    int m_FrameCount {0};
    int m_Fps {0};
    int m_DisabllowUpdating {0};

    // 脚本门面的两块状态（Get/SetTargetFrameRate / Get/SetRunInBackground）。
    // 放在实例上而不是文件静态量：Application 是系统，只有一个实例，而实例
    // 成员能让"退出 Play 时重置"这件事有明确的归属。
    int  m_TargetFrameRate {-1};
    bool m_RunInBackground {true};
};
