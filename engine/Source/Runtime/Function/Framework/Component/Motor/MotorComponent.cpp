#include "MotorComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

void MotorComponent::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;

    if (m_MotorRes.m_ControllerConfig.GetTypeString() == "PhysicsControllerConfig")
    {
        m_ControllerType = ControllerType::physics;
        PhysicsControllerConfig* controller_config =
            static_cast<PhysicsControllerConfig*>((ControllerConfig*)m_MotorRes.m_ControllerConfig);
        // m_Controller = new CharacterController();
    }
    else if (m_MotorRes.m_ControllerConfig != nullptr)
    {
        m_ControllerType = ControllerType::invalid;
        LOG_ERROR(ZMotor, "invalid controller type, not able to move");
    }

    const Transform* transform_component = parent_object->tryGetComponentConst(Transform);

    m_TargetPosition = transform_component->GetLocalPosition();
}

void MotorComponent::GetOffStuckDead()
{
    LOG_INFO(ZMotor, "Some get off stuck dead logic");
}
MotorComponent::~MotorComponent()
{
    if (m_ControllerType == ControllerType::physics)
    {
        delete m_Controller;
        m_Controller = nullptr;
    }
}

void MotorComponent::Tick(float delta_time)
{
    TickPlayerMotor(delta_time);
}

void MotorComponent::TickPlayerMotor(float delta_time)
{
    if (!m_ParentObject)
        return;

    Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    std::shared_ptr<Character> current_character = current_level->getCurrentActiveCharacter().lock();
    if (current_character == nullptr)
        return;

    if (current_character->GetObjectID() != m_ParentObject->GetID())
        return;

    Transform* transform_component =
        m_ParentObject->tryGetComponent<Transform>("Transform");

    Radian turn_angle_yaw = GET_SYSTEM(InputSystem)->m_CursorDeltaYaw;

    unsigned int command = GET_SYSTEM(InputSystem)->getGameCommand();

    if (command >= (unsigned int)GameCommand::invalid)
        return;

    CalculatedDesiredHorizontalMoveSpeed(command, delta_time);
    CalculatedDesiredVerticalMoveSpeed(command, delta_time);
    CalculatedDesiredMoveDirection(command, transform_component->GetLocalRotation());
    CalculateDesiredDisplacement(delta_time);
    CalculateTargetPosition(transform_component->GetLocalPosition());

    transform_component->SetLocalPosition(m_TargetPosition);
}

void MotorComponent::CalculatedDesiredHorizontalMoveSpeed(unsigned int command, float delta_time)
{
    bool has_move_command = ((unsigned int)GameCommand::forward | (unsigned int)GameCommand::backward |
                             (unsigned int)GameCommand::left | (unsigned int)GameCommand::right) &
                            command;
    has_move_command &= ((unsigned int)GameCommand::free_carema & command) == 0;
    bool has_sprint_command = (unsigned int)GameCommand::sprint & command;

    bool is_acceleration = false;
    float final_acceleration = m_MotorRes.m_MoveAcceleration;
    float min_speed_ratio = 0.f;
    float max_speed_ratio = 0.f;
    if (has_move_command)
    {
        is_acceleration = true;
        max_speed_ratio = m_MotorRes.m_MaxMoveSpeedRatio;
        if (m_MoveSpeedRatio >= m_MotorRes.m_MaxMoveSpeedRatio)
        {
            final_acceleration = m_MotorRes.m_SprintAcceleration;
            is_acceleration = has_sprint_command;
            min_speed_ratio = m_MotorRes.m_MaxMoveSpeedRatio;
            max_speed_ratio = m_MotorRes.m_MaxSprintSpeedRatio;
        }
    }
    else
    {
        is_acceleration = false;
        min_speed_ratio = 0.f;
        max_speed_ratio = m_MotorRes.m_MaxSprintSpeedRatio;
    }

    m_MoveSpeedRatio += (is_acceleration ? 1.0f : -1.0f) * final_acceleration * delta_time;
    m_MoveSpeedRatio = std::clamp(m_MoveSpeedRatio, min_speed_ratio, max_speed_ratio);
}

void MotorComponent::CalculatedDesiredVerticalMoveSpeed(unsigned int command, float delta_time)
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    if (m_MotorRes.m_JumpHeight == 0.f)
        return;

    const float gravity = physics_scene->getGravity().length();

    if (m_JumpState == JumpState::idle)
    {
        if ((unsigned int)GameCommand::jump & command)
        {
            m_JumpState = JumpState::rising;
            m_VerticalMoveSpeed = Math::sqrt(m_MotorRes.m_JumpHeight * 2 * gravity);
            m_JumpHorizontalSpeedRatio = m_MoveSpeedRatio;
        }
        else
        {
            m_VerticalMoveSpeed = 0.f;
        }
    }
    else if (m_JumpState == JumpState::rising || m_JumpState == JumpState::falling)
    {
        m_VerticalMoveSpeed -= gravity * delta_time;
        if (m_VerticalMoveSpeed <= 0.f)
        {
            m_JumpState = JumpState::falling;
        }
    }
}

void MotorComponent::CalculatedDesiredMoveDirection(unsigned int command, const Quaternion& object_rotation)
{
    if (m_JumpState == JumpState::idle)
    {
        Vector3 forward_dir = object_rotation * Vector3::NEGATIVE_UNIT_Y;
        Vector3 left_dir = object_rotation * Vector3::UNIT_X;

        if (command > 0)
        {
            m_DesiredHorizontalMoveDirection = Vector3::ZERO;
        }

        if ((unsigned int)GameCommand::forward & command)
        {
            m_DesiredHorizontalMoveDirection += forward_dir;
        }

        if ((unsigned int)GameCommand::backward & command)
        {
            m_DesiredHorizontalMoveDirection -= forward_dir;
        }

        if ((unsigned int)GameCommand::left & command)
        {
            m_DesiredHorizontalMoveDirection += left_dir;
        }

        if ((unsigned int)GameCommand::right & command)
        {
            m_DesiredHorizontalMoveDirection -= left_dir;
        }

        m_DesiredHorizontalMoveDirection.normalise();
    }
}

void MotorComponent::CalculateDesiredDisplacement(float delta_time)
{
    float horizontal_speed_ratio = m_JumpState == JumpState::idle ? m_MoveSpeedRatio : m_JumpHorizontalSpeedRatio;
    m_DesiredDisplacement =
        m_DesiredHorizontalMoveDirection * m_MotorRes.m_MoveSpeed * horizontal_speed_ratio * delta_time +
        Vector3::UNIT_Z * m_VerticalMoveSpeed * delta_time;
}

void MotorComponent::CalculateTargetPosition(const Vector3&& current_position)
{
    Vector3 final_position;

    switch (m_ControllerType)
    {
        case ControllerType::none:
            final_position = current_position + m_DesiredDisplacement;
            break;
        case ControllerType::physics:
            final_position = m_Controller->move(current_position, m_DesiredDisplacement);
            break;
        default:
            final_position = current_position;
            break;
    }

    // Z-hack: motor level simulating jump, character always above z-plane
    if (m_JumpState == JumpState::falling && final_position.z + m_DesiredDisplacement.z <= 0.f)
    {
        final_position.z = 0.f;
        m_JumpState = JumpState::idle;
    }

    m_IsMoving = (final_position - current_position).squaredLength() > 0.f;
    m_TargetPosition = final_position;
}