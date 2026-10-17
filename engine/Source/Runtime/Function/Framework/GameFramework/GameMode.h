#pragma once

// -----------------------------------------------------------------------------
// UE AGameMode 对应物：关卡规则层。
//
// 与 UE 的分层保持一致：
//   GameInstance  一份到底（跨关卡存活）
//     └─ GameMode   每关一份（关卡加载时创建，卸载时销毁）
//     └─ LocalPlayer → PlayerController
//
// 职责边界（与 UE 一致，避免与 GameInstance 混淆）：
//   - GameMode 只持有"本关"的规则与状态：默认角色、重生、胜负条件。
//     任何需要跨关卡存活的东西属于 GameInstance，不属于这里。
//   - 只在单机/服务端存在。网络复制后 GameState 承担状态同步，
//     GameMode 保留规则裁决权（本引擎暂无网络，先留接口与生命周期钩子）。
//
// 为什么不是 IEngineSystem：GameMode 的生命周期绑定关卡而不是进程，
// 由 GameInstance 在 OnLevelLoaded / OnLevelUnloaded 里创建与销毁，
// 走系统注册表反而要在每次关卡切换时注册/注销。
// -----------------------------------------------------------------------------

#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <string>

class GameMode
{
public:
    explicit GameMode(std::string level_url);
    virtual ~GameMode();

    /// 关卡加载完成后由 GameInstance::OnLevelLoaded 调用。
    /// 默认实现：把 GetDefaultCharacterName() 指名的 GameObject 设为待接管角色。
    virtual bool Initialize();

    /// 关卡卸载前由 GameInstance::OnLevelUnloaded 调用。
    virtual void Shutdown();

    /// 每个逻辑帧调用，顺序在 GameInstance::Tick 之后、LocalPlayer::Tick 之前。
    virtual void Tick(float delta_time);

    bool IsInitialized() const { return m_IsInitialized; }
    const std::string& GetLevelUrl() const { return m_LevelUrl; }

    /// 本关的默认角色名（对应 Level 的 character_name）。空串表示不由
    /// GameMode 指定，沿用关卡自己配置的那个。
    const std::string& GetDefaultCharacterName() const { return m_DefaultCharacterName; }
    void SetDefaultCharacterName(std::string name) { m_DefaultCharacterName = std::move(name); }

    /// 本关选中的待接管实体。k_invalid_gobject_id 表示还没有。
    /// 由 Initialize() 解析，供 GameEngine::AutoPossessActiveCharacter 消费。
    GObjectID GetPendingPossessTarget() const { return m_PendingPossessTarget; }

protected:
    /// 从活动关卡里解析默认角色对应的 GameObject。派生类可 override
    /// 换成按队伍/出生点挑选。
    virtual bool ResolveDefaultCharacter();

    std::string m_LevelUrl;
    std::string m_DefaultCharacterName;
    GObjectID m_PendingPossessTarget {k_invalid_gobject_id};
    bool m_IsInitialized {false};
};
