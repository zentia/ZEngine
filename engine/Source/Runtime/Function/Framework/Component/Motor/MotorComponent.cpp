#include "MotorComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Time/TimeState.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Controller/CharacterController.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/BaseClasses/Cast.h"



void MotorComponent::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;

    // PostLoadResource can run more than once on the same component (a prefab re-instantiate);
    // the previous controller (and the capsule / shape it owns) is ours to free before we build
    // another, or each reload leaks a whole set.
    if (m_Controller != nullptr)
    {
        delete m_Controller;
        m_Controller = nullptr;
        m_ControllerType = ControllerType::invalid;
    }

    // 按运行时类型分发，不要用 GetTypeString()：它是 static 的，展开成
    // PPtr<ControllerConfig>::GetTypeString() → "PPtr<ControllerConfig>"，
    // 跟实际指向的对象无关，跟 "PhysicsControllerConfig" 永远不相等。
    // 结果是每个 Motor 都掉进 invalid 分支，角色根本动不了。
    // （与 Jolt/Utils.cpp toShape() 踩的是同一个坑。）
    ControllerConfig* controller_config = m_MotorRes.m_ControllerConfig;
    if (Cast<PhysicsControllerConfig>(controller_config) != nullptr)
    {
        m_ControllerType = ControllerType::physics;
        PhysicsControllerConfig* physics_config = static_cast<PhysicsControllerConfig*>(controller_config);

        // The config keeps `Capsule m_CapsuleShape` as an INLINE value member, so
        // it was never registered with ObjectManager and has no meaningful
        // instance ID -- pointing a PPtr<Capsule> straight at it would resolve to
        // garbage. Clone it into a properly registered object and hand that over.
        Capsule* capsule = GET_SYSTEM(ObjectManager)->NewObject<Capsule>();
        capsule->m_Radius = physics_config->m_CapsuleShape.m_Radius;
        capsule->m_HalfHeight = physics_config->m_CapsuleShape.m_HalfHeight;

        CharacterController* character_controller = new CharacterController();
        character_controller->Initialize(capsule);
        m_Controller = character_controller;
    }
    else if (controller_config != nullptr)
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
    // Keyed on the pointer, not on m_ControllerType: the controller owns a capsule and a shape
    // (CharacterController's destructor frees them), and the pointer is the thing that says
    // whether there is anything to free. `delete nullptr` is a no-op.
    delete m_Controller;
    m_Controller = nullptr;
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
    {
        return;
    }

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

    // 位移算完必须过 CalculateTargetPosition 才会写进 m_TargetPosition：
    // 那里才按 ControllerType 分派（physics 走 CharacterController::move 做
    // 碰撞检测）。玩家路径原来少了这一步，m_TargetPosition 一直停在构造时的
    // 初始位置，于是 SetLocalPosition 每帧把角色写回原点 —— 位移算得再对
    // 也不动。AI 路径（TickAIMotor）有这一步，所以只有玩家控制会零位移。
    CalculateTargetPosition(transform_component->GetLocalPosition());

    CalculatedDesiredHorizontalMoveSpeed(command, delta_time);

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
// ===========================================================================
// Unity 的 CharacterController 面
// ===========================================================================
//
// 这一整块的共同前提：**没有活动物理场景时降级，而不是崩、也不是静默失败**。
// headless 测试、关卡切换的空档、以及没配 controller config 的 Motor 都走到
// 这条路上。

PhysicsScene* MotorComponent::ResolvePhysicsScene() const
{
    WorldManager* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return nullptr;
    }

    std::shared_ptr<PhysicsScene> scene = world_manager->GetCurrentActivePhysicsScene().lock();
    return scene.get();
}

CharacterController* MotorComponent::ResolveCharacterController() const
{
    if (m_ControllerType != ControllerType::physics || m_Controller == nullptr)
    {
        return nullptr;
    }

    // `m_Controller` 是 `Controller*`，而只有 PostLoadResource 会填它，且 physics
    // 分支下填的一定是 CharacterController。这里仍然用 dynamic_cast：将来加
    // 别的 Controller 实现时，一个 static_cast 会把新类型当成胶囊控制器去解引用。
    return dynamic_cast<CharacterController*>(m_Controller);
}

int32_t MotorComponent::Move(const Vector3& motion)
{
    if (m_ParentObject == nullptr)
    {
        return static_cast<int32_t>(CollisionFlags::none);
    }

    Transform* transform_component = m_ParentObject->tryGetComponent<Transform>("Transform");
    if (transform_component == nullptr)
    {
        return static_cast<int32_t>(CollisionFlags::none);
    }

    CharacterController* controller = ResolveCharacterController();
    if (controller == nullptr)
    {
        // 没配 controller config：这个 Motor 本来就走"无碰撞"那条路（见
        // CalculateTargetPosition 的 default 分支）。这里保持一致 —— 一个
        // 未配置的角色不该因为脚本调了 Move 就获得碰撞能力，也不该被钉死在原地。
        const Vector3 start = transform_component->GetLocalPosition();
        transform_component->SetLocalPosition(start + motion);
        m_IsMoving = motion.squaredLength() > 0.f;
        return static_cast<int32_t>(CollisionFlags::none);
    }

    const Vector3 start = transform_component->GetLocalPosition();
    Vector3 landed = start;
    const int32_t flags = controller->MoveWithFlags(ResolvePhysicsScene(), start, motion,
                                                    m_DetectCollisions, m_MinMoveDistance, landed);

    transform_component->SetLocalPosition(landed);

    // velocity 是"实际位移 / 时间"，不是请求的速度：撞墙之后两者不一样，而脚本
    // 想知道的是"我到底动没动、多快"。dt 为 0（编辑器暂停）时不能除 —— 那会写出
    // inf，而 inf 会顺着动画 blend 一路扩散；此时保持上一次的速度。
    constexpr float kEpsilon = 1.0e-5f;
    const float delta_time = TimeState::Get().GetDeltaTime();
    if (delta_time > kEpsilon)
    {
        m_Velocity = (landed - start) / delta_time;
    }

    m_IsMoving = (landed - start).squaredLength() > 0.f;
    return flags;
}

void MotorComponent::SimpleMove(const Vector3& speed)
{
    if (m_ParentObject == nullptr)
    {
        return;
    }

    const float delta_time = TimeState::Get().GetDeltaTime();
    if (delta_time <= 0.f)
    {
        // 暂停中：不给位移，也不要去改累积的下落速度 —— 否则恢复播放的那一帧
        // 会攒出一个巨大的下落量，角色直接穿过地板。
        return;
    }

    float gravity = 9.81f;
    if (PhysicsScene* scene = ResolvePhysicsScene(); scene != nullptr)
    {
        gravity = scene->getGravity().length();
    }

    // `speed` 的单位是 m/s 且**不**需要调用方乘 dt —— 这是 Unity 的 SimpleMove
    // 与 Move 最容易混错的一处：给 SimpleMove 再乘一次 dt，角色速度就会随帧率
    // 平方衰减。
    const Vector3 motion = CharacterController::ComputeSimpleMoveMotion(
        speed, gravity, delta_time, IsGrounded(), m_SimpleMoveFallSpeed);

    Move(motion);
}

bool MotorComponent::IsGrounded() const
{
    CharacterController* controller = ResolveCharacterController();
    PhysicsScene* scene = ResolvePhysicsScene();
    if (controller == nullptr || scene == nullptr || m_ParentObject == nullptr)
    {
        return false;
    }

    const Transform* transform_component = m_ParentObject->tryGetComponentConst(Transform);
    if (transform_component == nullptr)
    {
        return false;
    }

    return controller->ProbeGrounded(scene, transform_component->GetLocalPosition(), m_SkinWidth);
}

float MotorComponent::GetHeight() const
{
    const CharacterController* controller = ResolveCharacterController();
    return (controller != nullptr) ? controller->GetCapsuleHeight() : 0.f;
}

void MotorComponent::SetHeight(float height)
{
    if (CharacterController* controller = ResolveCharacterController(); controller != nullptr)
    {
        controller->SetCapsuleHeight(height);
    }
}

float MotorComponent::GetRadius() const
{
    const CharacterController* controller = ResolveCharacterController();
    return (controller != nullptr) ? controller->GetCapsuleRadius() : 0.f;
}

void MotorComponent::SetRadius(float radius)
{
    if (CharacterController* controller = ResolveCharacterController(); controller != nullptr)
    {
        controller->SetCapsuleRadius(radius);
    }
}

Vector3 MotorComponent::GetCenter() const
{
    const CharacterController* controller = ResolveCharacterController();
    return (controller != nullptr) ? controller->GetCapsuleCenter() : Vector3::ZERO;
}

void MotorComponent::SetCenter(const Vector3& center)
{
    if (CharacterController* controller = ResolveCharacterController(); controller != nullptr)
    {
        controller->SetCapsuleCenter(center);
    }
}
