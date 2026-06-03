#pragma once

#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Resource/ResType/Components/Motor.h"

enum class MotorState : unsigned char
{
    moving,
    jumping
};

enum class JumpState : unsigned char
{
    idle,
    rising,
    falling
};

class MotorComponent : public Component
{
public:
    MotorComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;

    ~MotorComponent() override;

    void Tick(float delta_time) override;
    void TickPlayerMotor(float delta_time);

    const Vector3& getTargetPosition() const { return m_TargetPosition; }

    float getSpeedRatio() const { return m_MoveSpeedRatio; }
    bool getIsMoving() const { return m_IsMoving; }

    void GetOffStuckDead();

private:
    void CalculatedDesiredHorizontalMoveSpeed(unsigned int command, float delta_time);
    void CalculatedDesiredVerticalMoveSpeed(unsigned int command, float delta_time);
    void CalculatedDesiredMoveDirection(unsigned int command, const Quaternion& object_rotation);
    void CalculateDesiredDisplacement(float delta_time);
    void CalculateTargetPosition(const Vector3&& current_position);

    MotorComponentRes m_MotorRes;

    float m_MoveSpeedRatio {0.f};
    float m_VerticalMoveSpeed {0.f};
    float m_JumpHorizontalSpeedRatio {0.f};

    Vector3 m_DesiredDisplacement;
    Vector3 m_DesiredHorizontalMoveDirection;
    Vector3 m_JumpInitialVelocity;
    Vector3 m_TargetPosition;

    MotorState m_MotorState {MotorState::moving};
    JumpState m_JumpState {JumpState::idle};

    ControllerType m_ControllerType {ControllerType::none};
    Controller* m_Controller {nullptr};

    bool m_IsMoving {false};
};