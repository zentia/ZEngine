#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

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

class PhysicsControllerConfig : public ControllerConfig
{
public:
    DECLARE_SERIALIZE(PhysicsControllerConfig)

    PhysicsControllerConfig() {}
    ~PhysicsControllerConfig() {}
    Capsule m_CapsuleShape;
};

class MotorComponentRes
{
public:
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