#pragma once

#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/AI/AIMoveIntent.h"
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
    REGISTER_CLASS(MotorComponent);
    DECLARE_OBJECT_SERIALIZE();

public:
    MotorComponent() = default;

    void PostLoadResource(GameObject* parent_object) override;

    ~MotorComponent() override;

    void Tick(float delta_time) override;
    void TickPlayerMotor(float delta_time);
    void TickAIMotor(float delta_time);

    /// Framework-level entry point for the AI layer.
    ///
    /// AIControllerComponent calls this once per frame with the intent its
    /// behaviour tree produced. This is deliberately a framework-level seam:
    /// scripts configure AI (profile, ranges) the same way a blueprint wires a
    /// behaviour tree, but they never drive movement themselves.
    void ApplyAIMoveIntent(const AIMoveIntent& intent, float delta_time);

    /// Yaw-only turn toward a point, rate limited by m_AITurnSpeedDegrees.
    /// Returns true once the agent is facing the target within tolerance.
    bool TurnToward(const Vector3& world_position, float delta_time);

    const Vector3& getTargetPosition() const { return m_TargetPosition; }

    float getSpeedRatio() const { return m_MoveSpeedRatio; }
    bool getIsMoving() const { return m_IsMoving; }
    bool IsAIDriven() const { return m_IsAIDriven; }

    /// Hand control back to player input. Called when the AIController is
    /// disabled or removed so a possessed pawn does not keep drifting.
    void ClearAIControl();

    void SetAITurnSpeedDegrees(float degrees) { m_AITurnSpeedDegrees = degrees; }
    float GetAITurnSpeedDegrees() const { return m_AITurnSpeedDegrees; }

    void SetAIArriveRadius(float radius) { m_AIArriveRadius = radius; }
    float GetAIArriveRadius() const { return m_AIArriveRadius; }

    /// Mutable access for importers/migrators. The controller config is stored
    /// inline in legacy JSON but held by PPtr at runtime, so it has to be
    /// patched in after the component's own Transfer has run.
    MotorComponentRes& GetMotorResMutable() { return m_MotorRes; }

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

    // ---- AI driving state ---------------------------------------------------

    /// True while an AIControllerComponent is feeding this motor. Player input
    /// is ignored while set, so a possessed pawn cannot fight its own AI.
    bool m_IsAIDriven {false};
    AIMoveIntent m_AIIntent;

    float m_AITurnSpeedDegrees {360.f};
    /// Stop moving once within this distance of the target; prevents the
    /// jitter you get when a seek loop keeps overshooting a point.
    float m_AIArriveRadius {0.1f};
};
