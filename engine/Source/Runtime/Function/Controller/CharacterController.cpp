#include "Runtime/Function/Controller/CharacterController.h"

#include "Runtime/Core/Math/TransformTRS.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

namespace
{
/// 半径的下限。Jolt 的 CapsuleShape 用一个非正的 convex radius 会直接断言，而
/// 一个"半径为 0 的角色"在物理上也没有意义 —— 它是一根线段，会从任何缝隙里漏过去。
constexpr float kMinCapsuleRadius = 0.001f;

// CollisionFlags 的三个位。值必须与 Unity 一致，且与 MotorComponent.h 里那个
// ZENUM 一一对应 —— 这里不能用那个枚举：MotorComponent.h include 了本头文件，
// 反过来引用就是循环依赖。两处靠注释和接线门（AA 组）对齐。
constexpr int32_t kFlagSides = 1;
constexpr int32_t kFlagAbove = 2;
constexpr int32_t kFlagBelow = 4;
}

CharacterController::~CharacterController()
{
    ReleaseOwnedObjects(/*keep_capsule=*/nullptr);
}

void CharacterController::ReleaseOwnedObjects(const Capsule* keep_capsule)
{
    // Resolve through the PPtr (-> ObjectManager table), not through a cached raw pointer: a PPtr
    // to an already-destroyed heap object resolves to null, so releasing twice is a no-op.
    if (RigidBodyShape* shape = m_RigidbodyShape)
        MemoryManager::DestroyObject(shape);
    m_RigidbodyShape = PPtr<RigidBodyShape>();

    // `keep_capsule` is the capsule about to be (re)adopted: when it IS the one already held,
    // destroying it here would free the very object Initialize() is about to use.
    Capsule* held = m_Capsule;
    if (held != nullptr && held != keep_capsule)
        MemoryManager::DestroyObject(held);
    m_Capsule = PPtr<Capsule>();
}

void CharacterController::Initialize(PPtr<Capsule> capsule)
{
    // Re-initialization (a prefab re-instantiate runs PostLoadResource again): whatever this
    // controller built before is ours to free.
    ReleaseOwnedObjects(capsule);
    m_Capsule = capsule;

    if (capsule == nullptr)
    {
        LOG_ERROR(ZPhysics, "CharacterController::Initialize: null capsule");
        return;
    }

    // m_RigidbodyShape is a PPtr, so it must point at a real Object registered
    // with ObjectManager -- assigning it a bare `new RigidBodyShape` (or nothing
    // at all, as this used to do) leaves the instance ID meaningless and the
    // first dereference crashes. `move()` routes through IsOverlap -> toShape,
    // which reads m_Type/m_Geometry/m_LocalTransform off this object.
    m_RigidbodyShape = GET_SYSTEM(ObjectManager)->NewObject<RigidBodyShape>();

    m_RigidbodyShape->m_Geometry = capsule;
    m_RigidbodyShape->m_Type = RigidBodyShapeType::capsule;

    Quaternion orientation;
    orientation.FromAngleAxis(Radian(Degree(90.f)), Vector3::UNIT_X);

    m_RigidbodyShape->m_LocalTransform =
        TransformTRS(Vector3(0, 0, capsule->m_HalfHeight + capsule->m_Radius), orientation, Vector3::UNIT_SCALE);
}

Vector3 CharacterController::move(const Vector3& current_position, const Vector3& displacement)
{
    // Initialize() bails out on a null capsule, and PostLoadResource only calls
    // it for ControllerType::physics. Guard so a misconfigured controller
    // degrades to unblocked movement instead of dereferencing a null shape.
    if (m_RigidbodyShape == nullptr)
    {
        return current_position + displacement;
    }

    WorldManager* world_manager = GET_SYSTEM(WorldManager);
    if (world_manager == nullptr)
    {
        return current_position + displacement;
    }

    std::shared_ptr<PhysicsScene> physics_scene = world_manager->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    return MoveInScene(physics_scene.get(), current_position, displacement);
}

Vector3 CharacterController::MoveInScene(PhysicsScene* scene,
                                        const Vector3& current_position,
                                        const Vector3& displacement)
{
    if (m_RigidbodyShape == nullptr || scene == nullptr)
    {
        // 没有形状 / 没有场景 = 无从判定重叠。退化成"无碰撞直移"：比把角色钉死在
        // 原地安全（钉死看起来像引擎挂了），也和一个没配 controller config 的
        // Motor 此前的表现一致。
        return current_position + displacement;
    }

    const Vector3 final_position = current_position + displacement;

    const TransformTRS final_transform =
        TransformTRS(final_position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);

    if (scene->IsOverlap(*m_RigidbodyShape, final_transform.getMatrix()))
    {
        return current_position;
    }

    return final_position;
}

int32_t CharacterController::MoveWithFlags(PhysicsScene* scene,
                                           const Vector3& from,
                                           const Vector3& motion,
                                           bool detect_collisions,
                                           float min_move_distance,
                                           Vector3& out_position)
{
    out_position = from;

    // minMoveDistance：整段丢弃，连 out_position 都不写 —— 否则"没动"和"被抖掉了"
    // 在调用方眼里分不出来。
    if (motion.length() <= min_move_distance)
    {
        return 0;
    }

    constexpr float kEpsilon = 1.0e-5f;

    // 顺序：先竖直再水平。竖直优先的理由是角色最常压在地上 —— 先解竖直能拿到
    // 干净的"站住了"，再解水平时起点已经在地面之上，不会因为脚底埋进地里而把
    // 水平位移也一起取消掉（合并判定就会）。
    Vector3 position = from;
    int32_t flags = 0;

    if (std::fabs(motion.z) > kEpsilon)
    {
        const Vector3 stepped =
            detect_collisions ? MoveInScene(scene, position, Vector3(0.f, 0.f, motion.z))
                              : position + Vector3(0.f, 0.f, motion.z);
        // 判据是"实际走得比请求的短"，用绝对值比：向下被挡时 stepped.z - from.z
        // 是 0 而 motion.z 是 -0.5 —— 直接写 `moved < motion.z` 会得出
        // "0 < -0.5" = false，也就是**向下撞地不报 Below**（AA20 抓到的）。
        // 方向只用来决定报哪一个位。
        const float moved = stepped.z - position.z;
        if (std::fabs(moved) < std::fabs(motion.z) - kEpsilon)
        {
            // 向下被挡 = 踩到了东西（Below）；向上被挡 = 撞到了顶（Above）。
            flags |= (motion.z < 0.f) ? kFlagBelow : kFlagAbove;
        }
        position = stepped;
    }

    for (int axis = 0; axis < 2; ++axis)
    {
        const Vector3 axis_motion =
            (axis == 0) ? Vector3(motion.x, 0.f, 0.f) : Vector3(0.f, motion.y, 0.f);
        const float requested = (axis == 0) ? motion.x : motion.y;
        if (std::fabs(requested) <= kEpsilon)
        {
            continue;
        }

        const Vector3 stepped = detect_collisions ? MoveInScene(scene, position, axis_motion)
                                                 : position + axis_motion;
        const float moved = (axis == 0) ? (stepped.x - position.x) : (stepped.y - position.y);
        if (std::fabs(moved) < std::fabs(requested) - kEpsilon)
        {
            flags |= kFlagSides;
        }
        position = stepped;
    }

    out_position = position;
    return flags;
}

Vector3 CharacterController::ComputeSimpleMoveMotion(const Vector3& speed,
                                                    float gravity,
                                                    float delta_time,
                                                    bool grounded,
                                                    float& inout_fall_speed)
{
    // Unity：speed 的竖直分量被忽略。引擎是 Z-up，所以丢掉的是 z。
    const Vector3 horizontal = Vector3(speed.x, speed.y, 0.f);

    if (grounded)
    {
        inout_fall_speed = 0.f;
        return horizontal * delta_time;
    }

    inout_fall_speed -= gravity * delta_time;
    return horizontal * delta_time + Vector3(0.f, 0.f, inout_fall_speed * delta_time);
}

bool CharacterController::ProbeGrounded(PhysicsScene* scene,
                                       const Vector3& current_position,
                                       float skin_width) const
{
    if (m_Capsule == nullptr || scene == nullptr)
    {
        return false;
    }

    const float half_extent = m_Capsule->m_HalfHeight + m_Capsule->m_Radius;
    const float skin = (skin_width > 0.f) ? skin_width : 0.f;

    // 从胶囊**中心**起跳，别从脚底起：射线起点正好贴着地面的话，浮点上它有可能
    // 落在地面那一侧，于是永远打不到 —— 表现是"站着却读不到 isGrounded"。
    const Vector3 origin = current_position + Vector3::UNIT_Z * half_extent;

    std::vector<PhysicsHitInfo> hits;
    return scene->Raycast(origin, Vector3::NEGATIVE_UNIT_Z, half_extent + skin, hits);
}

float CharacterController::GetCapsuleHeight() const
{
    if (m_Capsule == nullptr)
    {
        return 0.f;
    }
    return 2.0f * (m_Capsule->m_HalfHeight + m_Capsule->m_Radius);
}

void CharacterController::SetCapsuleHeight(float height)
{
    if (m_Capsule == nullptr)
    {
        return;
    }
    // 含帽总高不可能是负的，也不能小于一个球的直径 —— 后者意味着圆柱段是"反的",
    // Jolt 会断言。夹到 2*radius，退化成一个球。
    const float min_height = 2.0f * m_Capsule->m_Radius;
    const float clamped = (height > min_height) ? height : min_height;
    m_Capsule->m_HalfHeight = 0.5f * clamped - m_Capsule->m_Radius;
    RefreshShapeTransform();
}

float CharacterController::GetCapsuleRadius() const
{
    return (m_Capsule != nullptr) ? m_Capsule->m_Radius : 0.f;
}

void CharacterController::SetCapsuleRadius(float radius)
{
    if (m_Capsule == nullptr)
    {
        return;
    }
    m_Capsule->m_Radius = (radius > kMinCapsuleRadius) ? radius : kMinCapsuleRadius;
    RefreshShapeTransform();
}

float CharacterController::GetCapsuleHalfHeight() const
{
    return (m_Capsule != nullptr) ? m_Capsule->m_HalfHeight : 0.f;
}

void CharacterController::SetCapsuleHalfHeight(float half_height)
{
    if (m_Capsule == nullptr)
    {
        return;
    }
    m_Capsule->m_HalfHeight = (half_height > 0.f) ? half_height : 0.f;
    RefreshShapeTransform();
}

Vector3 CharacterController::GetCapsuleCenter() const
{
    if (m_Capsule == nullptr)
    {
        return Vector3::ZERO;
    }
    return Vector3(0.f, 0.f, m_Capsule->m_HalfHeight + m_Capsule->m_Radius);
}

void CharacterController::SetCapsuleCenter(const Vector3& center)
{
    if (m_Capsule == nullptr)
    {
        return;
    }
    // 中心要落在 center.z，半径不变 ⇒ 半高就是剩下那段。比半径还低就夹住：
    // 否则会算出负半高，胶囊翻过来。
    const float wanted = center.z - m_Capsule->m_Radius;
    m_Capsule->m_HalfHeight = (wanted > 0.f) ? wanted : 0.f;
    RefreshShapeTransform();
}

void CharacterController::RefreshShapeTransform()
{
    if (m_RigidbodyShape == nullptr || m_Capsule == nullptr)
    {
        return;
    }

    // 与 Initialize 里那次构造完全一致：绕 X 转 90° 把引擎的 Z-up 胶囊摆成
    // Jolt 要的 Y-up，再抬高 half_height + radius（原点在脚底）。
    Quaternion orientation;
    orientation.FromAngleAxis(Radian(Degree(90.f)), Vector3::UNIT_X);

    m_RigidbodyShape->m_LocalTransform = TransformTRS(
        Vector3(0, 0, m_Capsule->m_HalfHeight + m_Capsule->m_Radius), orientation, Vector3::UNIT_SCALE);
}