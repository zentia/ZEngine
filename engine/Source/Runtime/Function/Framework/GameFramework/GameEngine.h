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

protected:
    /// 卸载关卡前释放所有 PlayerController 持有的实体引用。
    void ReleaseAllPossessions();

    bool m_IsInitialized {false};
    bool m_RequestQuit {false};

    std::unique_ptr<GameInstance> m_GameInstance;
    std::vector<std::unique_ptr<LocalPlayer>> m_LocalPlayers;
};
