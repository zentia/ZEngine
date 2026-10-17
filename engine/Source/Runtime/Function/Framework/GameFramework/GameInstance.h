#pragma once

// -----------------------------------------------------------------------------
// Game framework: UE-style layered game runtime.
//
// UE 分层（从外到内）：
//   UGameEngine          引擎实例，拥有 GameInstance，驱动主循环
//    └─ UGameInstance    跨关卡存活的全局对象（一个进程一份）
//        └─ ULocalPlayer 本地玩家（分屏时一人一份）
//             └─ UPlayerController  输入 → Pawn 的桥梁
//        └─ UWorld（本引擎对应 WorldManager + Level）
//             └─ AGameMode   关卡规则（只在服务端/单机存在）
//             └─ AGameState  关卡状态（复制到所有客户端）
//
// 本骨架先落地前四层（GameEngine / GameInstance / LocalPlayer /
// PlayerController），World 直接复用现有的 WorldManager + Level，
// GameMode / GameState 在本批次只留接口与生命周期钩子。
//
// 设计取舍：
//   - 不引入 Actor / Pawn。本引擎的实体是 GameObject（Level::m_Gobjects），
//     强行引入第二套实体体系会立刻产生两套对象生命周期与序列化路径。
//     PlayerController 通过 GObjectID 指向 Level 里的 GameObject，
//     与 UE 的 Possess(Pawn*) 语义等价但复用现有实体。
//   - GameEngine 不继承 Application。Application 是 Object + IEngineSystem
//     且有完整主循环与 FPS 统计；GameEngine 复用它的 Tick，不另起炉灶。
// -----------------------------------------------------------------------------

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Framework/GameFramework/GameMode.h"

#include <memory>
#include <string>
#include <vector>

/// UE UGameInstance 对应物：跨关卡存活的进程级全局对象。
///
/// 生命周期由 GameEngine 拥有：Initialize() 之后一直存活到 Shutdown()。
/// 关卡切换（WorldManager::OpenScene / LoadLevelAdditive）不会重建它，
/// 这是它与 GameMode 的分界线 —— GameMode 每关一份，GameInstance 一份到底。
///
/// 典型用途：存档槽位、全局设置、跨关卡的数据缓存、网络会话句柄。
class GameInstance
{
public:
    virtual ~GameInstance() = default;

    /// 进程启动后调用一次。此时 WorldManager 已注册但尚未加载关卡。
    virtual bool Initialize();

    /// 进程退出前调用一次。
    virtual void Shutdown();

    /// 每个逻辑帧调用。由 GameEngine 在 LogicalTick 阶段驱动。
    virtual void Tick(float delta_time);

    /// 关卡加载完成后调用。GameMode / LocalPlayer 在此建立对世界的引用。
    virtual void OnLevelLoaded(const std::string& level_url);

    /// 关卡卸载前调用。持有 GameObject 引用的一方必须在此释放。
    virtual void OnLevelUnloaded(const std::string& level_url);

    bool IsInitialized() const { return m_IsInitialized; }

    /// 本关的 GameMode。关卡加载时创建、卸载时销毁，因此切换间隙可能为 nullptr。
    /// 骨架阶段默认类型就是 GameMode；后续按关卡配置挑选子类时在此扩展。
    GameMode* GetGameMode() const { return m_GameMode.get(); }

    /// 已加载的关卡 URL（WorldManager 的当前活动关卡）。
    const std::string& GetCurrentLevelUrl() const { return m_CurrentLevelUrl; }
    void SetCurrentLevelUrl(const std::string& level_url) { m_CurrentLevelUrl = level_url; }

protected:
    /// 创建本关的 GameMode。派生类可 override 返回自定义 GameMode 子类。
    virtual std::unique_ptr<GameMode> CreateGameMode(const std::string& level_url);

    bool m_IsInitialized {false};
    std::string m_CurrentLevelUrl;
    std::unique_ptr<GameMode> m_GameMode;
};
