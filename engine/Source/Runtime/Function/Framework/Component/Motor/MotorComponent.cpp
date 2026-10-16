#include "MotorComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

IMPLEMENT_REGISTER_CLASS(MotorComponent)
IMPLEMENT_OBJECT_SERIALIZE(MotorComponent)

template<typename TransferFunction>
void MotorComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_MotorRes, "motor_res");
}

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(MotorComponent)

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
    if (m_IsAIDriven)
    {
        TickAIMotor(delta_time);
        return;
    }

    TickPlayerMotor(delta_time);
}

void MotorComponent::TickAIMotor(float delta_time)
{
    if (!m_ParentObject)
        return;

    Transform* transform_component = m_ParentObject->tryGetComponent<Transform>("Transform");
    if (transform_component == nullptr)
        return;

    // The intent is world-space while the displacement pipeline below works in
    // local space. For root-level agents (the normal case) the two coincide,
    // and for parented agents this keeps the pawn moving under its parent
    // exactly as the player path does.
    const Vector3 current_position = transform_component->GetLocalPosition();

    if (m_AIIntent.has_face_target)
    {
        TurnToward(m_AIIntent.face_target, delta_time);
    }

    if (m_AIIntent.has_move_target)
    {
        const Vector3 to_target = m_AIIntent.move_target - current_position;
        const float distance_to_target = to_target.length();

        if (distance_to_target <= m_AIArriveRadius)
        {
            // Arrived: decelerate instead of snapping, so momentum reads right.
            CalculatedDesiredHorizontalMoveSpeed(0, delta_time);
            m_DesiredHorizontalMoveDirection = Vector3::ZERO;
        }
        else
        {
            Vector3 direction = to_target;
            direction.normalise();

            // Drive the speed ratio straight to the requested fraction of the
            // authored max. AI has no "hold W to accelerate" ramp, and going
            // through the player acceleration curve made agents feel sluggish.
            const float target_ratio = Math::Clamp(m_AIIntent.speed_ratio, 0.f, 1.f) *
                                       m_MotorRes.m_MaxMoveSpeedRatio;
            m_MoveSpeedRatio = target_ratio;
            m_DesiredHorizontalMoveDirection = direction;
        }
    }
    else
    {
        CalculatedDesiredHorizontalMoveSpeed(0, delta_time);
        m_DesiredHorizontalMoveDirection = Vector3::ZERO;
    }

    // AI never jumps: m_JumpHeight-driven vertical motion stays a player input
    // concern until the AI layer grows a jump action.
    m_VerticalMoveSpeed = 0.f;
    m_JumpState = JumpState::idle;

    CalculateDesiredDisplacement(delta_time);
    CalculateTargetPosition(std::move(current_position));

    transform_component->SetLocalPosition(m_TargetPosition);
}

bool MotorComponent::TurnToward(const Vector3& world_position, float delta_time)
{
    if (!m_ParentObject)
        return false;

    Transform* transform_component = m_ParentObject->tryGetComponent<Transform>("Transform");
    if (transform_component == nullptr)
        return false;

    const Vector3 current_position = transform_component->GetLocalPosition();
    Vector3 to_target = world_position - current_position;
    to_target.z = 0.f;  // yaw only: agents should not pitch toward a target

    if (to_target.squaredLength() <= 0.0001f)
        return true;

    // Build the turn from the engine's own forward convention (-Y, see
    // CalculatedDesiredMoveDirection) rather than extracting a yaw angle.
    // Going through cross/axis-angle keeps this correct regardless of which
    // axis "yaw" happens to be, and is what stops agents spinning the long way
    // round when a project flips its up axis.
    const Quaternion current_rotation = transform_component->GetLocalRotation();

    Vector3 current_forward = current_rotation * Vector3::NEGATIVE_UNIT_Y;
    current_forward.z = 0.f;
    if (current_forward.squaredLength() <= 0.000001f)
    {
        current_forward = Vector3::NEGATIVE_UNIT_Y;
    }
    current_forward.normalise();
    to_target.normalise();

    const float alignment = Math::Clamp(current_forward.dotProduct(to_target), -1.f, 1.f);
    Vector3 turn_axis = current_forward.crossProduct(to_target);

    // A near-zero cross product means the agent is either already facing the
    // target or facing exactly away -- and cross magnitude cannot tell those
    // two apart, so a plain atan2(len, dot) would read the 180-degree case as
    // "already aligned" and leave the agent staring backwards. Disambiguate on
    // the sign of the dot product instead.
    float angle = 0.f;
    if (turn_axis.squaredLength() > 0.000001f)
    {
        turn_axis.normalise();
        angle = Math::atan2(turn_axis.length(), alignment).valueRadians();
    }
    else if (alignment < 0.f)
    {
        // Facing exactly away: pick a deterministic perpendicular (Z is up here)
        // so the agent always turns the same way instead of stalling.
        turn_axis = Vector3::UNIT_Z;
        angle = Math_PI;
    }
    else
    {
        return true;
    }

    const float max_step = Math::DegreesToRadians(m_AITurnSpeedDegrees) * delta_time;
    if (Math::abs(angle) <= max_step)
    {
        Quaternion final_rotation = Quaternion(Radian(angle), turn_axis) * current_rotation;
        final_rotation.normalise();
        transform_component->SetLocalRotation(final_rotation);
        return true;
    }

    const float step = (angle > 0.f ? 1.f : -1.f) * max_step;
    Quaternion new_rotation = Quaternion(Radian(step), turn_axis) * current_rotation;
    new_rotation.normalise();
    transform_component->SetLocalRotation(new_rotation);

    return false;
}

void MotorComponent::ApplyAIMoveIntent(const AIMoveIntent& intent, float delta_time)
{
    m_IsAIDriven = true;
    m_AIIntent = intent;
}

void MotorComponent::ClearAIControl()
{
    m_IsAIDriven = false;
    m_AIIntent = AIMoveIntent {};
    m_DesiredHorizontalMoveDirection = Vector3::ZERO;
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