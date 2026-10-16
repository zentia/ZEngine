#include "PlayerController.h"
#include "LocalPlayer.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Input/InputSystem.h"

PlayerController::PlayerController(LocalPlayer* owning_player)
    : m_OwningPlayer(owning_player)
{
}

PlayerController::~PlayerController() = default;

bool PlayerController::Possess(GObjectID object_id)
{
    if (object_id == k_invalid_gobject_id)
    {
        LOG_WARNING(ZEngine, "PlayerController::Possess called with an invalid object id");
        return false;
    }

    m_PossessedObjectId = object_id;
    LOG_INFO(ZEngine, "PlayerController possessed object {}", object_id);
    return true;
}

void PlayerController::UnPossess()
{
    if (m_PossessedObjectId == k_invalid_gobject_id)
    {
        return;
    }

    LOG_INFO(ZEngine, "PlayerController released object {}", m_PossessedObjectId);
    m_PossessedObjectId = k_invalid_gobject_id;
}

void PlayerController::Tick(float delta_time)
{
    UpdateInput();
    (void)delta_time;
}

void PlayerController::UpdateInput()
{
    // Legacy InputSystem 仍然是本引擎权威的命令来源：MotorComponent 与
    // CameraComponent 都从 getGameCommand() 取值。骨架阶段先对齐这条路径，
    // 迁移到 EnhancedInput 的 action 之后再 override 本函数。
    const auto* input_system = GET_SYSTEM(InputSystem);
    if (input_system != nullptr)
    {
        m_CurrentCommand = input_system->getGameCommand();
    }
}
