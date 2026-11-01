#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Log/LogSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "Application.generated.h"

// `ResolveGizmoSelection` 的返回值只需要指针大小，不需要完整类型 —— 真正在
// 用它们的地方是 Application.cpp（那里才有 Level / GameObject 的定义）。
class GameObject;
class Level;

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

    /// `SetGizmoSelection` 存的那份。进程级静态（`Selection` 是全局概念，不是
    /// per-Application），与 `TimeState` / `Gizmos.color` 同一类。
    static std::vector<std::size_t> s_GizmoSelection;

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

    // ---- Unity 的应用层消息（OnApplicationFocus / Pause）----------------
    //
    // 做成公开入口而不是"WindowSystem 回调内部直接派发"，是因为这条信号有
    // **三个**来源：WindowSystem 的焦点回调（真机上唯一来源）、编辑器自己的
    // 窗口循环、以及测试。只留内部回调的话，整条链只能靠真的去点一下窗口来
    // 验证 —— 而 CI 里没有窗口可点，于是这条链永远没人跑过。

    /// 报告一次窗口焦点变化，派发 `OnApplicationFocus` + `OnApplicationPause`。
    ZFUNCTION()
    static void NotifyApplicationFocus(bool has_focus);

    /// 从"有没有焦点"和"失焦时是否继续跑"推导"应用是否暂停"。
    ///
    /// 单独抽成一个纯函数，是因为这是整条链上唯一**有判断**的一步（其余都是
    /// 转发），而它偏偏是唯一没法通过派发结果观测到的一步：单元测试里没有已
    /// 加载的世界，`BroadcastAppEvent` 走空。藏在 `NotifyApplicationFocus` 里
    /// 的话它就没有断言能碰到。
    ///
    /// ⚠️ 没有"上一次的值"这回事，因此**不去重**：每次焦点变化都会派发一条
    /// `OnApplicationPause(推导值)`。用 m_LastAppPaused 去重能少发一次，但那
    /// 是一块谁也看不见的状态 —— 而"失焦但 runInBackground=true"时它会把
    /// 一条真值（false）吞掉，变成"状态机说没变，实际上值是对的但没人知道"。
    static bool DeriveApplicationPaused(bool has_focus, bool run_in_background)
    {
        return !has_focus && !run_in_background;
    }

    // ---- Unity 的 gizmo 帧（OnDrawGizmos / OnDrawGizmosSelected）--------
    //
    // 做成公开入口，理由与上面那条 `NotifyApplicationFocus` 一模一样：这条链
    // 上唯一"真的在点窗口之外"的驱动点是编辑器，而 headless 测试里没有编辑器
    // 也没有已加载的世界。留成内部细节的话，`WorldManager::DrawGizmos` 这条
    // 路由永远只被"某一天有人手动打开编辑器画一条线"验证过。

    /// 当前选中的对象（Unity 的 `Selection`）。
    ///
    /// ⚠️ 用**实例 ID** 而不是裸指针：关卡一卸载指针立刻变野，而一个找不到的
    /// ID 只是"没选中"。编辑器在 Selection 变化时调一次，之后每帧复用。
    static void SetGizmoSelection(const std::vector<std::size_t>& selected_ids);
    static const std::vector<std::size_t>& GetGizmoSelection();

    /// 把选中的 ID 解析成指针。
    ///
    /// ⚠️ 单独提出来（而不是塞在 `DrawGizmosFrame` 里）是因为：这是整条链上
    /// 唯一**有判断**的一步（两张对象表、解析不到就跳过），而 `DrawGizmosFrame`
    /// 剩下的部分在没有活动关卡的世界里是走空的。藏起来的话，headless 下没有
    /// 任何一个断言能碰到它 —— 于是"运行时表忘了查"这种错永远测不到。
    static std::vector<GameObject*> ResolveGizmoSelection(Level* level);

    /// 一帧的 gizmo 派发。由 `LogicalTick` 末尾调用；单独提出来是为了让测试能
    /// 在没有完整主循环的情况下验证"这条路由在"。
    static void DrawGizmosFrame();

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
