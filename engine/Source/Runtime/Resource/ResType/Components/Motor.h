#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"
#include "Motor.generated.h"

enum class ControllerType : unsigned char
{
    none,
    physics,
    invalid
};

class ControllerConfig : public Object
{
    REGISTER_CLASS(ControllerConfig)

public:
    virtual ~ControllerConfig() {}
};

ZCLASS(NoSuperTransfer)
class PhysicsControllerConfig : public ControllerConfig
{
    ZGENERATED_BODY(PhysicsControllerConfig)

public:
    PhysicsControllerConfig() {}
    ~PhysicsControllerConfig() {}
    ZPROPERTY()
    Capsule m_CapsuleShape;
};

class MotorComponentRes
{
public:
    DECLARE_SERIALIZE(MotorComponentRes)

    MotorComponentRes() = default;
    ~MotorComponentRes();

    float m_MoveSpeed {0.f};
    float m_JumpHeight {0.f};
    float m_MaxMoveSpeedRatio {0.f};
    float m_MaxSprintSpeedRatio {0.f};
    float m_MoveAcceleration {0.f};
    float m_SprintAcceleration {0.f};

    PPtr<ControllerConfig> m_ControllerConfig;
};

template<typename TransferFunction>
void MotorComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_MoveSpeed, "move_speed");
    transfer.Transfer(m_JumpHeight, "jump_height");
    transfer.Transfer(m_MaxMoveSpeedRatio, "max_move_speed_ratio");
    transfer.Transfer(m_MaxSprintSpeedRatio, "max_sprint_speed_ratio");
    transfer.Transfer(m_MoveAcceleration, "move_acceleration");
    transfer.Transfer(m_SprintAcceleration, "sprint_acceleration");
    transfer.Transfer(m_ControllerConfig, "controller_config");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(MotorComponentRes, , Transfer, void)
