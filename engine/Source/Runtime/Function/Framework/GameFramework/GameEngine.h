#pragma once

// -----------------------------------------------------------------------------
// UE UGameEngine 对应物：独立游戏进程（打包后的游戏）的引擎实例。
//
// 与编辑器的边界：
//   编辑器有自己的 Editor::Run()。本类服务的是"独立进程"这一种形态 ——
//   没有编辑器 UI、没有 Content Browser、没有 Undo/Redo，只有游戏本身。
//   编辑器内 PIE 是下一批工作，届时由 PIE 复用本类的 GameInstance/LocalPlayer，
//   只是主循环仍挂在 Editor 上。
//
// 复用而非重写：本类继承 Application。Application 已经是
// Object + IEngineSystem 且带完整主循环（Run / TickOneFrame / CalculateFPS /
// CalculateDeltaTime），GameEngine 只补上游戏层的三件事：
//   1. 拥有 GameInstance（跨关卡存活）
//   2. 拥有 LocalPlayer 列表（分屏）
//   3. 关卡加载后把 PlayerController 接到世界的实体上
//
// 继承而不是持有一个 Application 指针：Application::Run() 的循环不便从外部
// 插入游戏层 Tick，而 CalculateDeltaTime() 是 protected。继承让 GameEngine
// 直接复用同一份帧计时，避免出现两套 delta。
// -----------------------------------------------------------------------------

#include "Runtime/Application/Application.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Framework/GameFramework/GameInstance.h"
#include "Runtime/Function/Framework/GameFramework/LocalPlayer.h"

#include <memory>
#include <string>
#include <vector>

/// 驱动独立游戏进程的游戏层。
///
/// 注册为 IEngineSystem（Gameplay 阶段），依赖 WorldManager 与 InputSystem。
/// 初始化顺序：WorldManager 加载好关卡之后，GameEngine 才 Possess 角色。
class GameEngine : public Application
{
public:
    GameEngine();
    ~GameEngine() override;

    std::string GetName() const override { return "GameEngine"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }
    std::vector<std::type_index> GetDependencies() const override;

    bool Initialize() override;
    void Shutdown() override;

    /// 每个逻辑帧调用。由 Application::LogicalTick 之后的钩子驱动。
    void Tick(float delta_time);

    /// 进入主循环直到窗口关闭。独立进程入口（ZGame）调用这个。
    int Run();

    /// 请求退出主循环。
    void RequestQuit() { m_RequestQuit = true; }

    GameInstance* GetGameInstance() const { return m_GameInstance.get(); }

    /// 创建 num_players 个本地玩家（1 = 单人，>1 = 分屏）。
    bool CreateLocalPlayers(int num_players);
    LocalPlayer* GetLocalPlayer(int index) const;
    int GetLocalPlayerCount() const { return static_cast<int>(m_LocalPlayers.size()); }

    /// 加载关卡并把它接到 GameInstance / PlayerController 上。
    bool LoadLevel(const std::string& level_url);

    bool IsInitialized() const { return m_IsInitialized; }

    /// 用当前活动关卡里的默认角色建立接管关系。
    /// 骨架阶段的策略：取 Level::getCurrentActiveCharacter() 指向的 GameObject。
    bool AutoPossessActiveCharacter();

    // -------------------------------------------------------------------------
    // PIE（编辑器内播放）
    //
    // 独立进程走 Run()，主循环在 GameEngine 手里；PIE 反过来 —— 主循环仍在
    // Editor::Run()，游戏层只是每帧被 Tick 一次。编辑器只调下面两个方法，
    // 不碰 GameInstance / LocalPlayer 的内部。
    //
    // 当前实现是 UE 正统的"独立 Play World"：进入时由 WorldManager 把编辑器
    // 关卡复制出第二份（<project>/<saved>/PIE/ 下的临时 .scene）并切成活动
    // 关卡，游戏逻辑只改那一份，编辑器世界分毫未动，因此停止时直接丢弃、
    // 无需重载，也不会产生"未保存改动"。
    // 复制失败时才退化为同世界播放，那种情况下必须由调用方重载恢复。
    // -------------------------------------------------------------------------

    /// 进入 PIE：建 GameInstance / LocalPlayer，并把 PlayerController 接到
    /// 当前活动关卡的角色上。已有活动关卡是前提。
    bool BeginPIE();

    /// 退出 PIE：解绑实体引用、拆掉游戏层，并丢弃 Play World（若用过）。
    /// 退化路径下本方法不动世界 —— 由调用方（Editor）负责重载。
    void EndPIE();

    bool IsInPIE() const { return m_IsInPIE; }

    /// 本次 PIE 是否成功建立了独立 Play World。
    /// false 表示退化成了同世界播放，调用方必须在停止后重载关卡；
    /// true 表示编辑器世界从未被改过，重载反而会丢掉用户的未保存改动。
    bool IsPlayWorldPIE() const { return m_PieUsedPlayWorld; }

protected:
    /// 卸载关卡前释放所有 PlayerController 持有的实体引用。
    void ReleaseAllPossessions();

    /// 把 GameInstance 绑到当前活动关卡（记录 URL、建 GameMode、接管角色）。
    /// Initialize() 与 BeginPIE() 共用：前者处理"启动关卡已在系统就绪前加载"，
    /// 后者处理"PIE 开始时关卡才刚被编辑器打开"。关卡不存在时什么也不做。
    void BindToActiveLevel();

    bool m_IsInitialized {false};
    bool m_RequestQuit {false};
    bool m_IsInPIE {false};
    /// 本次 PIE 是否真的用了独立 Play World（false = 退化成同世界播放）。
    bool m_PieUsedPlayWorld {false};

    std::unique_ptr<GameInstance> m_GameInstance;
    std::vector<std::unique_ptr<LocalPlayer>> m_LocalPlayers;
};