#pragma once

#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <string>

class LocalPlayer;

/// UE APlayerController 对应物：输入 → 被控实体的桥梁。
///
/// UE 里 PlayerController::Possess(APawn*) 接管一个实体，此后输入由
/// PlayerController 翻译成移动意图再喂给 Pawn。本引擎没有 Pawn / Actor，
/// 实体是 Level 里的 GameObject（Character 包装了其中一个），因此这里
/// Possess 的是 GObjectID —— 语义等价，实体体系仍然只有一套。
///
/// 生命周期：由 LocalPlayer 拥有（见 LocalPlayer::Initialize / Shutdown）。
/// 关卡切换时 GameEngine 调 UnPossess()，避免悬挂的 GObjectID 指向已卸载关卡。
class PlayerController
{
public:
    explicit PlayerController(LocalPlayer* owning_player);
    virtual ~PlayerController();

    /// 接管 level 里的一个实体。k_invalid_gobject_id 表示解除接管。
    bool Possess(GObjectID object_id);
    void UnPossess();

    GObjectID GetPossessedObjectId() const { return m_PossessedObjectId; }
    bool HasPossessed() const { return m_PossessedObjectId != k_invalid_gobject_id; }

    LocalPlayer* GetLocalPlayer() const { return m_OwningPlayer; }

    /// 每个逻辑帧调用一次，顺序在 InputSystem::Tick() 之后。
    virtual void Tick(float delta_time);

    /// 输入 → 意图。骨架阶段记录本帧命令，供后续对接 MotorComponent。
    unsigned int GetCurrentCommand() const { return m_CurrentCommand; }

protected:
    /// 从 InputSystem 采样本帧命令。派生类可 override 换成 EnhancedInput 的 action。
    virtual void UpdateInput();

    LocalPlayer* m_OwningPlayer {nullptr};
    GObjectID m_PossessedObjectId {k_invalid_gobject_id};
    unsigned int m_CurrentCommand {0};
};
