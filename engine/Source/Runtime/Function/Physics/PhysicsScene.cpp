#include "Runtime/Function/Physics/PhysicsScene.h"

#include "Runtime/Core/Math/TransformTRS.h"

#include "Jolt/Core/Factory.h"
#include "Jolt/Core/JobSystem.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Body/BodyLock.h"
#include "Jolt/Physics/Body/BodyLockMulti.h"
#include "Jolt/Physics/Collision/CastResult.h"
#include "Jolt/Physics/Collision/CollideShape.h"
#include "Jolt/Physics/Collision/CollisionCollectorImpl.h"
#include "Jolt/Physics/Collision/NarrowPhaseQuery.h"
#include "Jolt/Physics/Collision/RayCast.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/CompoundShapeVisitors.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "Jolt/Physics/Collision/ShapeCast.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Jolt/RegisterTypes.h"
#include "Runtime/Function/Physics/Jolt/Utils.h"
#include "Runtime/Function/Physics/PhysicsConfig.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Collision/ContactListener.h"
#include "Jolt/Physics/Constraints/DistanceConstraint.h"
#include "Jolt/Physics/Constraints/FixedConstraint.h"
#include "Jolt/Physics/Constraints/HingeConstraint.h"
#include "Jolt/Physics/Constraints/PointConstraint.h"
#include "Jolt/Physics/Constraints/SliderConstraint.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "core/base/Macro.h"

#include <unordered_map>
#include <utility>

namespace
{
    /// Any unit vector perpendicular to in_axis. Jolt wants a "normal axis"
    /// beside the hinge/slider axis purely to build a constraint frame, so the
    /// direction does not matter as long as it is not parallel.
    JPH::Vec3 MakePerpendicularAxis(const JPH::Vec3& in_axis)
    {
        const float y = in_axis.GetY();
        const JPH::Vec3 reference = (y < 0.9f && y > -0.9f) ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisX();
        return reference.Cross(in_axis).Normalized();
    }

    /// 最小质量（千克）。0 / 负质量会让倒数质量变成 inf，一步积分之后就是 NaN
    /// 位置，而一个 NaN 物体会把整个 broadphase 拖垮。
    constexpr float kMinMass = 0.0001f;

    /// MoveKinematic 用位移 / delta_time 反推速度，delta_time 为 0 会得到 inf。
    constexpr float kMinDeltaTime = 0.0001f;

    /// Jolt 的接触求解器每对接触问一次"这个接触用多少摩擦/弹性"。这两个函数就是
    /// 答案，必须与 `CombineSurfaceValues` 是同一套规则 —— 所以规则本身住在
    /// RigidBody.h 里的那个纯函数里，这里只负责把 Jolt 的 Body 翻译成它的参数。
    ///
    /// 值取 Body 自带的（Jolt 已经存了），组合模式取 user data（Jolt 没有这个
    /// 概念，只能搭车）。
    ///
    /// ⚠️ 这两个函数在 PhysicsSystem::Update 内部、可能并发地被调用，所以**不能**
    /// 查任何可变容器（PhysicsManager 的 body->GameObject 表正在被别处写就炸了）。
    /// 只读 Body 自身的成员是安全的：Jolt 保证求解期间 Body 不被修改。
    float JoltCombineFriction(const JPH::Body& in_body_1, const JPH::SubShapeID&,
                              const JPH::Body& in_body_2, const JPH::SubShapeID&)
    {
        return CombineSurfaceValues(in_body_1.GetFriction(),
                                    FrictionCombineFromUserData(in_body_1.GetUserData()),
                                    in_body_2.GetFriction(),
                                    FrictionCombineFromUserData(in_body_2.GetUserData()));
    }

    float JoltCombineRestitution(const JPH::Body& in_body_1, const JPH::SubShapeID&,
                                 const JPH::Body& in_body_2, const JPH::SubShapeID&)
    {
        return CombineSurfaceValues(in_body_1.GetRestitution(),
                                    BounceCombineFromUserData(in_body_1.GetUserData()),
                                    in_body_2.GetRestitution(),
                                    BounceCombineFromUserData(in_body_2.GetUserData()));
    }
}  // namespace

// -----------------------------------------------------------------------------
// Trigger (sensor) event collection
//
// Jolt calls the contact listener from inside PhysicsSystem::Update(), which
// means from worker threads, so everything shared with the game thread sits
// behind a mutex.
//
// The enter/exit pairing needs care because OnContactRemoved() is not allowed
// to touch the bodies at all (Jolt has them locked / half-destroyed by then).
// Instead of asking "is this body a sensor?" at removal time, we remember which
// pairs we reported an enter for and emit the matching exit when the pair goes
// away. A body that is deleted while overlapping is handled by FlushBody().
// -----------------------------------------------------------------------------
class PhysicsContactListener final : public JPH::ContactListener
{
public:
    void OnContactAdded(const JPH::Body& in_body1,
                        const JPH::Body& in_body2,
                        [[maybe_unused]] const JPH::ContactManifold& in_manifold,
                        [[maybe_unused]] JPH::ContactSettings& io_settings) override
    {
        const bool body1_is_sensor = in_body1.IsSensor();
        const bool body2_is_sensor = in_body2.IsSensor();

        if (!body1_is_sensor && !body2_is_sensor)
        {
            // Ordinary collision: not a trigger. This is the OnCollisionEnter
            // path -- it used to be dropped on the floor, which meant Unity's
            // collision callbacks could never exist no matter what the
            // framework layer did.
            Collide(in_body1, in_body2, in_manifold, /*queue=*/true);
            return;
        }

        const uint32_t trigger_id =
            (body1_is_sensor ? in_body1 : in_body2).GetID().GetIndexAndSequenceNumber();
        const uint32_t other_id = (body1_is_sensor ? in_body2 : in_body1).GetID().GetIndexAndSequenceNumber();

        Add(trigger_id, other_id);
    }

    void OnContactPersisted(const JPH::Body& in_body1,
                            const JPH::Body& in_body2,
                            [[maybe_unused]] const JPH::ContactManifold& in_manifold,
                            [[maybe_unused]] JPH::ContactSettings& io_settings) override
    {
        if (in_body1.IsSensor() || in_body2.IsSensor())
        {
            return;
        }

        // 只刷新接触几何：pair 已经在 m_Collisions 里（或马上会被
        // PollCollisionEvents 标记成 enter）。这里**不**排队 —— 否则每个子步
        // 都会挤进一条事件，一帧内 3 条 OnCollisionStay。
        Collide(in_body1, in_body2, in_manifold, /*queue=*/false);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& in_sub_shape_pair) override
    {
        const uint32_t body_a = in_sub_shape_pair.GetBody1ID().GetIndexAndSequenceNumber();
        const uint32_t body_b = in_sub_shape_pair.GetBody2ID().GetIndexAndSequenceNumber();
        Remove(body_a, body_b);
        RemoveCollision(body_a, body_b);
    }

    /// Hand the queued events to the caller and start a fresh queue.
    void Poll(std::vector<PhysicsTriggerEvent>& out_events)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        out_events.clear();
        out_events.reserve(m_Events.size() + m_Active.size());
        for (const PhysicsTriggerEvent& pending : m_Events)
        {
            out_events.push_back(pending);
        }
        m_Events.clear();

        // 补 stay：还重叠着、且上一次 poll 时就已经重叠的 pair。判据与碰撞
        // 那边的 stay 完全一致（见 PollCollisions 的注释）。
        std::unordered_set<uint64_t> active_now;
        for (const auto& entry : m_Active)
        {
            active_now.insert(entry.first);

            if (m_TriggerActiveLastPoll.find(entry.first) == m_TriggerActiveLastPoll.end())
            {
                continue;
            }
            PhysicsTriggerEvent stay_event = entry.second;
            stay_event.m_Phase = PhysicsContactPhase::stay;
            stay_event.m_IsEnter = false;
            out_events.push_back(stay_event);
        }
        m_TriggerActiveLastPoll.swap(active_now);
    }

    /// Hand this frame's collision events over, deriving `stay` for pairs that
    /// were already in contact at the previous poll.
    void PollCollisions(std::vector<PhysicsCollisionEvent>& out_events)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        out_events.clear();
        out_events.reserve(m_CollisionEvents.size() + m_Collisions.size());

        // 本帧新增 / 结束的先按到达顺序给出去。
        for (const PhysicsCollisionEvent& pending : m_CollisionEvents)
        {
            out_events.push_back(pending);
        }
        m_CollisionEvents.clear();

        // 然后补 stay：还接触着、且**上一次 poll 时就已经接触**的 pair。
        //
        // "上一次已经接触"而不是"上一次已经报过 stay"：新撞上的 pair 这一轮走
        // enter，下一轮才轮到 stay —— 否则"刚撞上"的那一帧会同时收到
        // OnCollisionEnter 和 OnCollisionStay，而 Unity 只发 Enter。
        std::unordered_set<uint64_t> active_now;
        for (const auto& entry : m_Collisions)
        {
            active_now.insert(entry.first);

            if (m_ActiveLastPoll.find(entry.first) == m_ActiveLastPoll.end())
            {
                continue;   // 本轮刚进来的，enter 已经在上面发出去了
            }
            PhysicsCollisionEvent stay_event = entry.second;
            stay_event.m_Phase = PhysicsContactPhase::stay;
            out_events.push_back(stay_event);
        }
        m_ActiveLastPoll.swap(active_now);
    }

    /// A body is going away: close out every pair it took part in so gameplay
    /// does not keep believing something is still touching it.
    void FlushBody(uint32_t in_body_id)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        for (auto it = m_Active.begin(); it != m_Active.end();)
        {
            const PhysicsTriggerEvent& active = it->second;

            if (active.m_TriggerBodyId == in_body_id || active.m_OtherBodyId == in_body_id)
            {
                PhysicsTriggerEvent exit_event = active;
                exit_event.m_IsEnter = false;
                exit_event.m_Phase = PhysicsContactPhase::exit;
                m_Events.push_back(exit_event);
                m_TriggerActiveLastPoll.erase(it->first);
                it = m_Active.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = m_Collisions.begin(); it != m_Collisions.end();)
        {
            const PhysicsCollisionEvent& active = it->second;
            if (active.m_BodyA == in_body_id || active.m_BodyB == in_body_id)
            {
                PhysicsCollisionEvent exit_event = active;
                exit_event.m_Phase = PhysicsContactPhase::exit;
                m_CollisionEvents.push_back(exit_event);
                m_ActiveLastPoll.erase(it->first);
                it = m_Collisions.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

private:
    /// Order-independent key for a body pair.
    static uint64_t MakeKey(uint32_t in_body_a, uint32_t in_body_b)
    {
        if (in_body_a > in_body_b)
        {
            std::swap(in_body_a, in_body_b);
        }

        return (static_cast<uint64_t>(in_body_a) << 32) | static_cast<uint64_t>(in_body_b);
    }

    void Add(uint32_t in_trigger_id, uint32_t in_other_id)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        const uint64_t key = MakeKey(in_trigger_id, in_other_id);
        if (m_Active.find(key) != m_Active.end())
        {
            // Already inside: Jolt can report the same pair again while the
            // contact persists on a different sub shape.
            return;
        }

        PhysicsTriggerEvent enter_event;
        enter_event.m_TriggerBodyId = in_trigger_id;
        enter_event.m_OtherBodyId = in_other_id;
        enter_event.m_IsEnter = true;
        enter_event.m_Phase = PhysicsContactPhase::enter;

        m_Active.emplace(key, enter_event);
        m_Events.push_back(enter_event);
    }

    void Remove(uint32_t in_body_a, uint32_t in_body_b)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        const uint64_t key = MakeKey(in_body_a, in_body_b);
        const auto found = m_Active.find(key);
        if (found == m_Active.end())
        {
            return;
        }

        PhysicsTriggerEvent exit_event = found->second;
        exit_event.m_IsEnter = false;
        exit_event.m_Phase = PhysicsContactPhase::exit;
        m_Events.push_back(exit_event);
        m_Active.erase(found);
        m_TriggerActiveLastPoll.erase(key);
    }

    // ---- collisions ----------------------------------------------------

    /// Record (or refresh) a non-sensor contact pair.
    ///
    /// `queue` 只在 `OnContactAdded` 时为 true —— 那是"新接触"，要发
    /// OnCollisionEnter。`OnContactPersisted` 每个子步都会来，那里只更新几何，
    /// 不排队（否则一帧内会冒出好几条 stay）。
    void Collide(const JPH::Body& in_body1,
                 const JPH::Body& in_body2,
                 const JPH::ContactManifold& in_manifold,
                 bool queue)
    {
        PhysicsCollisionEvent event;
        event.m_BodyA = in_body1.GetID().GetIndexAndSequenceNumber();
        event.m_BodyB = in_body2.GetID().GetIndexAndSequenceNumber();
        // 接触点是 RVec3（Jolt 的双精度世界坐标）；`toVec3` 只认单精度
        // JPH::Vec3，所以这里手工降精度。远离原点的大世界里这一步会丢精度，
        // 但接触点只用于"在哪儿冒火花"这类表现。
        const JPH::RVec3 contact_point = in_manifold.GetWorldSpaceContactPointOn1(0);
        event.m_Point = Vector3(static_cast<float>(contact_point.GetX()),
                                static_cast<float>(contact_point.GetY()),
                                static_cast<float>(contact_point.GetZ()));
        // Jolt 的法线定义是"把 body2 推出去的方向"，即从 1 指向 2，与这里的
        // A -> B 一致，不用取反。
        event.m_Normal = toVec3(in_manifold.mWorldSpaceNormal);
        // 沿法线的相对速度。两边的合速度可能很大但只是"擦着走"，所以只看投影
        // —— 撞得重不重取决于法向分量。
        const JPH::Vec3 delta_velocity = in_body1.GetLinearVelocity() - in_body2.GetLinearVelocity();
        event.m_RelativeSpeed = std::fabs(delta_velocity.Dot(in_manifold.mWorldSpaceNormal));
        event.m_Phase = PhysicsContactPhase::enter;

        std::lock_guard<std::mutex> guard(m_Mutex);

        const uint64_t key = MakeKey(event.m_BodyA, event.m_BodyB);
        const bool is_new = (m_Collisions.find(key) == m_Collisions.end());
        m_Collisions[key] = event;

        if (queue && is_new)
        {
            m_CollisionEvents.push_back(event);
        }
    }

    void RemoveCollision(uint32_t in_body_a, uint32_t in_body_b)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);

        const uint64_t key = MakeKey(in_body_a, in_body_b);
        const auto found = m_Collisions.find(key);
        if (found == m_Collisions.end())
        {
            return;
        }

        PhysicsCollisionEvent exit_event = found->second;
        exit_event.m_Phase = PhysicsContactPhase::exit;
        m_CollisionEvents.push_back(exit_event);
        m_Collisions.erase(found);
        m_ActiveLastPoll.erase(key);
    }

    std::mutex m_Mutex;
    std::unordered_map<uint64_t, PhysicsTriggerEvent> m_Active;
    std::vector<PhysicsTriggerEvent> m_Events;

    /// 上一次 poll 时还重叠着的触发器 pair —— stay 的判据。
    std::unordered_set<uint64_t> m_TriggerActiveLastPoll;

    /// 当前还在接触的碰撞 pair（几何持续被 OnContactPersisted 刷新）。
    std::unordered_map<uint64_t, PhysicsCollisionEvent> m_Collisions;
    /// 本帧待发出的 enter / exit。
    std::vector<PhysicsCollisionEvent> m_CollisionEvents;
    /// 上一次 poll 时还在接触的 pair —— stay 的判据。
    std::unordered_set<uint64_t> m_ActiveLastPoll;
};

PhysicsScene::PhysicsScene(const Vector3& gravity)
{
    static_assert(s_InvalidRigidbodyId == JPH::BodyID::cInvalidBodyID);
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    m_Physics.m_JoltPhysicsSystem = new JPH::PhysicsSystem();
    m_Physics.m_JoltBroadPhaseLayerInterface = new BPLayerInterfaceImpl();

    m_Physics.m_JoltJobSystem = new JPH::JobSystemThreadPool(
        m_Config.m_MaxJobCount, m_Config.m_MaxBarrierCount, static_cast<int>(m_Config.m_MaxConcurrentJobCount));

    // 16M temp memory
    m_Physics.m_TempAllocator = new JPH::TempAllocatorImpl(16 * 1024 * 1024);

    m_Physics.m_JoltPhysicsSystem->Init(m_Config.m_MaxBodyCount,
                                        m_Config.m_BodyMutexCount,
                                        m_Config.m_MaxBodyPairs,
                                        m_Config.m_MaxContactConstraints,
                                        *(m_Physics.m_JoltBroadPhaseLayerInterface),
                                        m_ObjectVsBroadPhaseLayerFilter,
                                        m_ObjectLayerPairFilter);
    // use the default setting
    m_Physics.m_JoltPhysicsSystem->SetPhysicsSettings(JPH::PhysicsSettings());

    m_Physics.m_JoltPhysicsSystem->SetGravity(toVec3(gravity));
    m_Config.m_Gravity = gravity;

    m_ContactListener = std::make_unique<PhysicsContactListener>();
    m_Physics.m_JoltPhysicsSystem->SetContactListener(m_ContactListener.get());

    // Surface combining has to be installed on the system, not per body: Jolt
    // asks "what is the combined friction for this pair" once per contact
    // manifold, and the answer depends on both bodies. Jolt's own defaults
    // (sqrt(f1*f2) for friction, max(r1,r2) for restitution) are close to
    // Unity's Average/Maximum but are not any of the four Unity modes, so a
    // level authored against Unity's rules would silently resolve differently.
    m_Physics.m_JoltPhysicsSystem->SetCombineFriction(&JoltCombineFriction);
    m_Physics.m_JoltPhysicsSystem->SetCombineRestitution(&JoltCombineRestitution);
}

PhysicsScene::~PhysicsScene()
{
    // Constraints first: they reference bodies, and the manager does not own
    // them, so each one has to be detached and released by hand.
    for (JPH::Constraint* joint : m_Joints)
    {
        if (joint == nullptr)
        {
            continue;
        }

        m_Physics.m_JoltPhysicsSystem->RemoveConstraint(joint);
        joint->Release();
    }
    m_Joints.clear();

    if (m_Physics.m_JoltPhysicsSystem != nullptr)
    {
        m_Physics.m_JoltPhysicsSystem->SetContactListener(nullptr);
    }
    m_ContactListener.reset();

    delete m_Physics.m_JoltPhysicsSystem;
    delete m_Physics.m_JoltJobSystem;
    delete m_Physics.m_TempAllocator;
    delete m_Physics.m_JoltBroadPhaseLayerInterface;

    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void PhysicsScene::setGravity(const Vector3& gravity)
{
    m_Config.m_Gravity = gravity;
    m_Physics.m_JoltPhysicsSystem->SetGravity(toVec3(gravity));
}

uint32_t PhysicsScene::CreateRigidBody(const TransformTRS& global_transform,
                                       const RigidBodyComponentRes& rigidbody_actor_res,
                                       int32_t unity_layer)
{
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();

    struct JPHShapeData
    {
        JPH::Shape* shape {nullptr};
        TransformTRS local_transform;
        Vector3 global_position;
        Vector3 global_scale;
        Quaternion global_rotation;
    };

    std::vector<JPHShapeData> jph_shapes;

    for (size_t shape_index = 0; shape_index < rigidbody_actor_res.m_Shapes.size(); shape_index++)
    {
        const RigidBodyShape* shape = rigidbody_actor_res.m_Shapes[shape_index];
        if (shape == nullptr)
        {
            continue;
        }

        const Matrix4x4 shape_global_transform = global_transform.getMatrix() * shape->m_LocalTransform.getMatrix();

        Vector3 global_position, global_scale;
        Quaternion global_rotation;

        shape_global_transform.Decomposition(global_position, global_scale, global_rotation);

        JPH::Shape* jph_shape = toShape(*shape, global_scale);

        if (jph_shape)
        {
            jph_shapes.push_back({jph_shape, shape->m_LocalTransform, global_position, global_scale, global_rotation});
        }
    }

    if (jph_shapes.empty())
    {
        LOG_ERROR(ZPhysics, "Create JPH Shapes Failed");
        return JPH::BodyID::cInvalidBodyID;
    }

    // The serialized actor type decides the Jolt motion type. Static is the
    // default (and what every body used to be hardcoded to), so existing levels
    // behave exactly as before; authoring a dynamic or kinematic body is now
    // what turns on velocity / force / gravity for it.
    const RigidBodyActorType actor_type = rigidbody_actor_res.GetActorType();
    const bool is_dynamic = actor_type == RigidBodyActorType::dynamic;
    JPH::EMotionType motion_type = JPH::EMotionType::Static;
    switch (actor_type)
    {
        case RigidBodyActorType::kinematic:
            motion_type = JPH::EMotionType::Kinematic;
            break;
        case RigidBodyActorType::dynamic:
            motion_type = JPH::EMotionType::Dynamic;
            break;
        default:
            break;
    }

    // The object layer IS the Unity layer: ObjectLayerPairFilterImpl resolves
    // collisions through TagLayerManager's matrix and every query masks on it,
    // so a body that kept the old NON_MOVING/MOVING pair would be invisible to
    // `Physics.Raycast(..., layerMask)` and immune to IgnoreLayerCollision.
    //
    // An out-of-range layer falls back to 0 ("Default") instead of being
    // clamped silently into some other layer: Jolt's BroadPhaseLayerInterface
    // asserts on an unknown object layer, and JPH_ENABLE_ASSERTS is on in
    // Debug, so passing 47 through would be a hard crash rather than a miss.
    JPH::ObjectLayer layer = (unity_layer >= 0 && unity_layer < kPhysicsLayerCount)
                                 ? static_cast<JPH::ObjectLayer>(unity_layer)
                                 : static_cast<JPH::ObjectLayer>(0);

    JPH::Ref<JPH::StaticCompoundShapeSettings> compund_shape_setting = new JPH::StaticCompoundShapeSettings;
    for (const JPHShapeData& shape_data : jph_shapes)
    {
        const Vector3d scaled_shape_position(
            shape_data.local_transform.m_Position.x * shape_data.global_scale.x,
            shape_data.local_transform.m_Position.y * shape_data.global_scale.y,
            shape_data.local_transform.m_Position.z * shape_data.global_scale.z);
        compund_shape_setting->AddShape(toVec3(scaled_shape_position.ToVector3()),
                                        toQuat(shape_data.local_transform.m_Rotation),
                                        shape_data.shape);
    }

    JPH::BodyCreationSettings body_settings(compund_shape_setting,
                                           toVec3(global_transform.m_Position.ToVector3()),
                                           toQuat(global_transform.m_Rotation),
                                           motion_type,
                                           layer);

    // Surface material. Jolt's own defaults (0.2 friction / 0 restitution) are
    // what every body used to get, and there was no way to see or change them;
    // taking them from the res is what makes a PhysicMaterial authorable at
    // all. The combine modes ride in user data because Jolt has no per-body
    // slot for them and the combine callback only gets the two Bodies.
    body_settings.mFriction = rigidbody_actor_res.m_Friction;
    body_settings.mRestitution = rigidbody_actor_res.m_Bounciness;
    body_settings.mUserData =
        PackCombineModes(rigidbody_actor_res.GetFrictionCombine(), rigidbody_actor_res.GetBounceCombine());

    // Authoring carries an inverse mass (0 == "unset"), so honour it when it is
    // meaningful: take the mass from it and let Jolt derive the inertia tensor
    // from the shape, scaled to that mass. Leaving it 0 keeps the shape-derived
    // density-based mass.
    if (is_dynamic && rigidbody_actor_res.m_InverseMass > 0.f)
    {
        body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        body_settings.mMassPropertiesOverride.mMass = 1.f / rigidbody_actor_res.m_InverseMass;
    }

    JPH::Body* jph_body = body_interface.CreateBody(body_settings);

    if (jph_body == nullptr)
    {
        LOG_ERROR(ZPhysics, "Create JPH Body Failed");
        for (const JPHShapeData& shape_data : jph_shapes)
        {
            delete shape_data.shape;
        }

        return JPH::BodyID::cInvalidBodyID;
    }

    body_interface.AddBody(jph_body->GetID(), JPH::EActivation::Activate);
    LOG_INFO(ZPhysics, "Add Body: {}", jph_body->GetID().GetIndexAndSequenceNumber());

    return jph_body->GetID().GetIndexAndSequenceNumber();
}

void PhysicsScene::RemoveRigidBody(uint32_t body_id)
{
    m_PendingRemoveBodies.push_back(body_id);
}

bool PhysicsScene::GetBodySurface(uint32_t body_id, float& out_friction, float& out_restitution) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    out_friction = body_lock.GetBody().GetFriction();
    out_restitution = body_lock.GetBody().GetRestitution();
    return true;
}

bool PhysicsScene::SetBodyFriction(uint32_t body_id, float friction)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    // BodyInterface locks internally, so this must NOT be called from inside a
    // BodyLockRead on the same body -- Jolt asserts on taking a lower-priority
    // lock while holding one, and in Debug that is a hard trap.
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    if (!body_interface.IsAdded(JPH::BodyID(body_id)))
    {
        return false;
    }

    body_interface.SetFriction(JPH::BodyID(body_id), friction);
    return true;
}

bool PhysicsScene::SetBodyRestitution(uint32_t body_id, float restitution)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    if (!body_interface.IsAdded(JPH::BodyID(body_id)))
    {
        return false;
    }

    body_interface.SetRestitution(JPH::BodyID(body_id), restitution);
    return true;
}

bool PhysicsScene::SetBodyCombineModes(uint32_t body_id,
                                       PhysicMaterialCombine friction_combine,
                                       PhysicMaterialCombine bounce_combine)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    if (!body_interface.IsAdded(JPH::BodyID(body_id)))
    {
        return false;
    }

    body_interface.SetUserData(JPH::BodyID(body_id), PackCombineModes(friction_combine, bounce_combine));
    return true;
}

bool PhysicsScene::GetBodyLayer(uint32_t body_id, int32_t& out_layer) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    out_layer = static_cast<int32_t>(body_lock.GetBody().GetObjectLayer());
    return true;
}

bool PhysicsScene::SetBodyLayer(uint32_t body_id, int32_t unity_layer)
{
    if (body_id == s_InvalidRigidbodyId || unity_layer < 0 || unity_layer >= kPhysicsLayerCount)
    {
        return false;
    }

    // Existence check only, and scoped so the read lock is released BEFORE
    // SetObjectLayer takes its write lock. Jolt asserts on a same-or-higher
    // priority lock taken while one is held ("this can create a deadlock") --
    // and it is right, so the two must not overlap.
    {
        JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
        if (!body_lock.Succeeded())
        {
            return false;
        }
    }

    // SetObjectLayer moves the body between broadphase layers too, so it has to
    // go through the interface rather than poking the Body directly -- a direct
    // write leaves the body in the old broadphase tree and it stops colliding
    // with anything that moved into the new one.
    m_Physics.m_JoltPhysicsSystem->GetBodyInterface().SetObjectLayer(
        JPH::BodyID(body_id), static_cast<JPH::ObjectLayer>(unity_layer));
    return true;
}

void PhysicsScene::UpdateRigidBodyGlobalTransform(uint32_t body_id, const TransformTRS& global_transform)
{
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();

    body_interface.SetPositionAndRotation(JPH::BodyID(body_id),
                                          toVec3(global_transform.m_Position.ToVector3()),
                                          toQuat(global_transform.m_Rotation),
                                          JPH::EActivation::Activate);
}

namespace
{
    JPH::EMotionType ToJoltMotionType(RigidBodyActorType actor_type)
    {
        switch (actor_type)
        {
            case RigidBodyActorType::kinematic:
                return JPH::EMotionType::Kinematic;
            case RigidBodyActorType::dynamic:
                return JPH::EMotionType::Dynamic;
            default:
                return JPH::EMotionType::Static;
        }
    }

    RigidBodyActorType FromJoltMotionType(JPH::EMotionType motion_type)
    {
        switch (motion_type)
        {
            case JPH::EMotionType::Kinematic:
                return RigidBodyActorType::kinematic;
            case JPH::EMotionType::Dynamic:
                return RigidBodyActorType::dynamic;
            default:
                return RigidBodyActorType::static_body;
        }
    }
}  // namespace

bool PhysicsScene::GetRigidBodyMotionType(uint32_t body_id, RigidBodyActorType& out_motion_type) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    out_motion_type = FromJoltMotionType(body_interface.GetMotionType(JPH::BodyID(body_id)));
    return true;
}

bool PhysicsScene::SetRigidBodyMotionType(uint32_t body_id, RigidBodyActorType motion_type)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetMotionType(JPH::BodyID(body_id), ToJoltMotionType(motion_type), JPH::EActivation::Activate);
    return true;
}

bool PhysicsScene::GetRigidBodyPosition(uint32_t body_id, Vector3& out_position) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    out_position = toVec3(body_interface.GetPosition(JPH::BodyID(body_id)));
    return true;
}

bool PhysicsScene::GetRigidBodyRotation(uint32_t body_id, Quaternion& out_rotation) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    out_rotation = toQuat(body_interface.GetRotation(JPH::BodyID(body_id)));
    return true;
}

bool PhysicsScene::GetLinearVelocity(uint32_t body_id, Vector3& out_velocity) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    // A static body has no motion properties at all; Jolt's GetLinearVelocity
    // already returns zero for it.
    out_velocity = toVec3(body_interface.GetLinearVelocity(JPH::BodyID(body_id)));
    return true;
}

bool PhysicsScene::SetLinearVelocity(uint32_t body_id, const Vector3& velocity)
{
    // Setting a velocity on a body whose motion type has none is an assert in
    // Jolt, so a static body is simply left alone.
    RigidBodyActorType motion_type {};
    if (!GetRigidBodyMotionType(body_id, motion_type) || motion_type == RigidBodyActorType::static_body)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetLinearVelocity(JPH::BodyID(body_id), toVec3(velocity));
    return true;
}

bool PhysicsScene::GetAngularVelocity(uint32_t body_id, Vector3& out_velocity) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    out_velocity = toVec3(body_interface.GetAngularVelocity(JPH::BodyID(body_id)));
    return true;
}

bool PhysicsScene::SetAngularVelocity(uint32_t body_id, const Vector3& velocity)
{
    RigidBodyActorType motion_type {};
    if (!GetRigidBodyMotionType(body_id, motion_type) || motion_type == RigidBodyActorType::static_body)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetAngularVelocity(JPH::BodyID(body_id), toVec3(velocity));
    return true;
}

bool PhysicsScene::AddForce(uint32_t body_id, const Vector3& force)
{
    return ApplyForceOrImpulse(body_id, force, true);
}

bool PhysicsScene::AddImpulse(uint32_t body_id, const Vector3& impulse)
{
    return ApplyForceOrImpulse(body_id, impulse, false);
}

bool PhysicsScene::ApplyForceOrImpulse(uint32_t body_id, const Vector3& value, bool is_force)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    // AddForce / AddImpulse live on JPH::Body, not on BodyInterface, so the
    // body needs a write lock.
    //
    // 但**唤醒必须在拿到写锁之前**：`BodyInterface::ActivateBody` 自己会再拿一
    // 次同优先级的写锁，而 Jolt 不允许"已持有同级锁时再取"（PhysicsLock 的
    // `inType > mutexes` 断言），随后就是同一线程重入同一把 mutex —— 直接死锁。
    // 之前这两行在锁里，而没有任何测试真正调用过 AddForce / AddImpulse，所以
    // 它一直没暴露（表现是"施力之后物理整个停住"，不报错）。
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    {
        JPH::BodyLockRead probe_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(),
                                     JPH::BodyID(body_id));
        if (!probe_lock.Succeeded())
        {
            return false;
        }

        // Only dynamic bodies integrate forces. Silently ignoring it keeps a
        // script from crashing the frame because a level author left a body
        // static.
        if (!probe_lock.GetBody().IsDynamic())
        {
            return false;
        }
    }
    body_interface.ActivateBody(JPH::BodyID(body_id));

    JPH::BodyLockWrite body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    // GetBody() returns a reference and Succeeded() above already proved it is
    // valid, so there is nothing left to null-check here.
    JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        return false;
    }

    if (is_force)
    {
        jph_body.AddForce(toVec3(value));
    }
    else
    {
        jph_body.AddImpulse(toVec3(value));
    }
    return true;
}

bool PhysicsScene::GetInverseMass(uint32_t body_id, float& out_inverse_mass) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    const JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        // A static / kinematic body is immovable: inverse mass 0.
        out_inverse_mass = 0.f;
        return false;
    }

    const JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        out_inverse_mass = 0.f;
        return false;
    }

    out_inverse_mass = motion_properties->GetInverseMass();
    return true;
}

bool PhysicsScene::GetMass(uint32_t body_id, float& out_mass) const
{
    float inverse_mass = 0.f;
    if (!GetInverseMass(body_id, inverse_mass) || inverse_mass <= 0.f)
    {
        out_mass = 0.f;
        return false;
    }
    out_mass = 1.f / inverse_mass;
    return true;
}

bool PhysicsScene::SetMass(uint32_t body_id, float mass)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    // 与 AddForce / AddImpulse 同一条规则：MotionProperties 挂在 Body 上，要写
    // 锁而不是 BodyInterface（后者会自己再拿一次写锁，Jolt 会断言"优先级不高
    // 于当前持有的锁"）。
    JPH::BodyLockWrite body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        return false;
    }
    JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        return false;
    }

    // 0 / 负质量会让倒数质量变成 inf，进积分就是 NaN 位置，而一个 NaN 物体会把
    // 整个 broadphase 拖垮。夹到一个极小正数，比"照单全收然后整场炸掉"好。
    const float safe_mass = (mass > kMinMass) ? mass : kMinMass;

    // ScaleToMass 把**质量和惯性张量一起**按新质量缩放：只改倒数质量的话，一个
    // 10kg 的箱子会带着 1kg 的转动惯量，推起来对了、转起来飞快。
    JPH::MassProperties mass_properties = jph_body.GetShape()->GetMassProperties();
    mass_properties.ScaleToMass(safe_mass);
    motion_properties->SetMassProperties(JPH::EAllowedDOFs::All, mass_properties);
    return true;
}

bool PhysicsScene::GetGravityFactor(uint32_t body_id, float& out_factor) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    out_factor = body_interface.GetGravityFactor(JPH::BodyID(body_id));
    return true;
}

bool PhysicsScene::SetGravityFactor(uint32_t body_id, float factor)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetGravityFactor(JPH::BodyID(body_id), factor);
    return true;
}

// damping 挂在 MotionProperties 上，而 BodyInterface 没有转发这两个访问器
// （它只转发了 gravity factor），所以下面四个方法各自拿一次 Body 锁。
bool PhysicsScene::GetLinearDamping(uint32_t body_id, float& out_damping) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    const JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        out_damping = 0.f;
        return false;
    }

    out_damping = jph_body.GetMotionProperties()->GetLinearDamping();
    return true;
}

bool PhysicsScene::SetLinearDamping(uint32_t body_id, float damping)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockWrite body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        return false;
    }

    jph_body.GetMotionProperties()->SetLinearDamping(damping);
    return true;
}

bool PhysicsScene::GetAngularDamping(uint32_t body_id, float& out_damping) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    const JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        out_damping = 0.f;
        return false;
    }

    out_damping = jph_body.GetMotionProperties()->GetAngularDamping();
    return true;
}

bool PhysicsScene::SetAngularDamping(uint32_t body_id, float damping)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockWrite body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    JPH::Body& jph_body = body_lock.GetBody();
    if (!jph_body.IsDynamic())
    {
        return false;
    }

    jph_body.GetMotionProperties()->SetAngularDamping(damping);
    return true;
}

bool PhysicsScene::AddTorque(uint32_t body_id, const Vector3& torque)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    // BodyInterface 的 AddTorque 会顺带唤醒睡着的物体，而 Body::AddTorque 不会
    // —— 脚本"给睡着的箱子一个力矩"当然希望它转起来。
    body_interface.AddTorque(JPH::BodyID(body_id), toVec3(torque));
    return true;
}

bool PhysicsScene::AddAngularImpulse(uint32_t body_id, const Vector3& angular_impulse)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.AddAngularImpulse(JPH::BodyID(body_id), toVec3(angular_impulse));
    return true;
}

bool PhysicsScene::AddForceAtPosition(uint32_t body_id, const Vector3& force, const Vector3& position)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.AddForce(JPH::BodyID(body_id), toVec3(force), toVec3(position));
    return true;
}

bool PhysicsScene::AddImpulseAtPosition(uint32_t body_id, const Vector3& impulse, const Vector3& position)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.AddImpulse(JPH::BodyID(body_id), toVec3(impulse), toVec3(position));
    return true;
}

bool PhysicsScene::SetRigidBodyPosition(uint32_t body_id, const Vector3& position)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetPosition(JPH::BodyID(body_id), toVec3(position), JPH::EActivation::Activate);
    return true;
}

bool PhysicsScene::SetRigidBodyRotation(uint32_t body_id, const Quaternion& rotation)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetRotation(JPH::BodyID(body_id), toQuat(rotation), JPH::EActivation::Activate);
    return true;
}

bool PhysicsScene::MoveKinematic(uint32_t body_id,
                                 const Vector3& target_position,
                                 const Quaternion& target_rotation,
                                 float delta_time)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    // deltaTime 为 0 时 Jolt 算出的速度是 inf（位移 / 0），所以夹到极小正数：
    // 调用方传 0 是"我没想清楚"，不是"我要无穷大的速度"。
    const float safe_delta = (delta_time > kMinDeltaTime) ? delta_time : kMinDeltaTime;

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.MoveKinematic(JPH::BodyID(body_id), toVec3(target_position), toQuat(target_rotation),
                                 safe_delta);
    return true;
}

bool PhysicsScene::IsSleeping(uint32_t body_id, bool& out_is_sleeping) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    // IsActive 而不是 IsSleeping：Jolt 的 BodyInterface 只有前者，而"不在模拟"
    // 正是 Unity 的 isSleeping 的含义。
    out_is_sleeping = !body_interface.IsActive(JPH::BodyID(body_id));
    return true;
}

bool PhysicsScene::SleepRigidBody(uint32_t body_id)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.DeactivateBody(JPH::BodyID(body_id));
    return true;
}

bool PhysicsScene::WakeRigidBody(uint32_t body_id)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.ActivateBody(JPH::BodyID(body_id));
    return true;
}

void PhysicsScene::Tick(float delta_time)
{
    const float time_step = 1.f / m_Config.m_UpdateFrequency;

    m_Physics.m_JoltPhysicsSystem->Update(
        time_step, m_Physics.m_CollisionSteps, m_Physics.m_TempAllocator, m_Physics.m_JoltJobSystem);

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    for (uint32_t body_id : m_PendingRemoveBodies)
    {
        LOG_INFO(ZPhysics, "Remove Body {}", body_id)

        // Close out trigger pairs before the body id can be recycled: a deleted
        // body never gets an OnContactRemoved for its last contacts.
        if (m_ContactListener != nullptr)
        {
            m_ContactListener->FlushBody(body_id);
        }

        body_interface.RemoveBody(JPH::BodyID(body_id));
        body_interface.DestroyBody(JPH::BodyID(body_id));
    }
    m_PendingRemoveBodies.clear();
}

bool PhysicsScene::SetRigidBodySensor(uint32_t body_id, bool is_sensor)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetIsSensor(JPH::BodyID(body_id), is_sensor);
    return true;
}

bool PhysicsScene::IsRigidBodySensor(uint32_t body_id, bool& out_is_sensor) const
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    if (!body_lock.Succeeded())
    {
        return false;
    }

    out_is_sensor = body_lock.GetBody().IsSensor();
    return true;
}

void PhysicsScene::PollTriggerEvents(std::vector<PhysicsTriggerEvent>& out_events)
{
    out_events.clear();

    if (m_ContactListener == nullptr)
    {
        return;
    }

    m_ContactListener->Poll(out_events);
}

void PhysicsScene::PollCollisionEvents(std::vector<PhysicsCollisionEvent>& out_events)
{
    out_events.clear();

    if (m_ContactListener == nullptr)
    {
        return;
    }

    m_ContactListener->PollCollisions(out_events);
}

uint32_t PhysicsScene::CreateJoint(const PhysicsJointDesc& in_desc)
{
    if (in_desc.m_BodyA == s_InvalidRigidbodyId || in_desc.m_BodyB == s_InvalidRigidbodyId ||
        in_desc.m_BodyA == in_desc.m_BodyB)
    {
        LOG_ERROR(ZPhysics, "CreateJoint: invalid body pair ({}, {})", in_desc.m_BodyA, in_desc.m_BodyB);
        return s_InvalidJointId;
    }

    // Constraint settings take world-space points and turn them into each
    // body's local frame while the constraint is created, which is why both
    // bodies have to be locked for the whole switch below.
    //
    // One BodyLockMultiWrite, not two BodyLockWrite: Jolt tracks lock order per
    // thread and both bodies are the same priority (EPhysicsLockTypes::PerBody),
    // so a second per-body lock trips "A lock of same or higher priority was
    // already taken, this can create a deadlock!" and then "Mutex was not
    // locked!" when the second one is released. That is not cosmetic -- two
    // independent locks on two bodies really can deadlock against another
    // thread doing the same in the opposite order. BodyLockMultiWrite takes the
    // mutex mask once and locks it in a consistent order.
    const JPH::BodyID body_ids[2] = {JPH::BodyID(in_desc.m_BodyA), JPH::BodyID(in_desc.m_BodyB)};
    JPH::BodyLockMultiWrite joint_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), body_ids, 2);

    JPH::Body* jph_body_a = joint_lock.GetBody(0);
    JPH::Body* jph_body_b = joint_lock.GetBody(1);
    if (jph_body_a == nullptr || jph_body_b == nullptr)
    {
        LOG_ERROR(ZPhysics, "CreateJoint: body not found ({}, {})", in_desc.m_BodyA, in_desc.m_BodyB);
        return s_InvalidJointId;
    }

    JPH::Constraint* jph_constraint = nullptr;

    switch (in_desc.m_Type)
    {
        case PhysicsJointType::fixed:
        {
            JPH::FixedConstraintSettings settings;
            settings.mAutoDetectPoint = in_desc.m_AutoDetectPoint;
            if (!in_desc.m_AutoDetectPoint)
            {
                settings.mPoint1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
                settings.mPoint2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            }
            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            break;
        }
        case PhysicsJointType::hinge:
        {
            JPH::HingeConstraintSettings settings;
            settings.mPoint1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
            settings.mPoint2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            settings.mHingeAxis1 = toVec3(in_desc.m_AxisA.normalisedCopy());
            settings.mHingeAxis2 = toVec3(in_desc.m_AxisB.normalisedCopy());
            // Hinge needs a normal that is perpendicular to the axis; any
            // perpendicular direction works, Jolt only uses it to build the
            // constraint frame.
            settings.mNormalAxis1 = MakePerpendicularAxis(settings.mHingeAxis1);
            settings.mNormalAxis2 = MakePerpendicularAxis(settings.mHingeAxis2);
            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)
            {
                settings.mLimitsMin = in_desc.m_MinLimit;
                settings.mLimitsMax = in_desc.m_MaxLimit;
            }
            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            break;
        }
        case PhysicsJointType::point:
        {
            JPH::PointConstraintSettings settings;
            settings.mPoint1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
            settings.mPoint2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            break;
        }
        case PhysicsJointType::slider:
        {
            JPH::SliderConstraintSettings settings;
            settings.mAutoDetectPoint = in_desc.m_AutoDetectPoint;
            settings.mPoint1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
            settings.mPoint2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            settings.mSliderAxis1 = toVec3(in_desc.m_AxisA.normalisedCopy());
            settings.mSliderAxis2 = toVec3(in_desc.m_AxisB.normalisedCopy());
            settings.mNormalAxis1 = MakePerpendicularAxis(settings.mSliderAxis1);
            settings.mNormalAxis2 = MakePerpendicularAxis(settings.mSliderAxis2);
            if (in_desc.m_MinLimit < 0.f && in_desc.m_MaxLimit > 0.f)
            {
                settings.mLimitsMin = in_desc.m_MinLimit;
                settings.mLimitsMax = in_desc.m_MaxLimit;
            }
            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            break;
        }
        case PhysicsJointType::distance:
        {
            JPH::DistanceConstraintSettings settings;
            settings.mPoint1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
            settings.mPoint2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            // Negative means "derive the range from the two pivots", which is
            // what an unconfigured desc asks for.
            if (in_desc.m_MinLimit >= 0.f && in_desc.m_MaxLimit >= in_desc.m_MinLimit)
            {
                settings.mMinDistance = in_desc.m_MinLimit;
                settings.mMaxDistance = in_desc.m_MaxLimit;
            }
            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            break;
        }
        default:
            break;
    }

    if (jph_constraint == nullptr)
    {
        LOG_ERROR(ZPhysics, "CreateJoint: Jolt refused to build the constraint");
        return s_InvalidJointId;
    }

    m_Physics.m_JoltPhysicsSystem->AddConstraint(jph_constraint);
    m_Joints.push_back(jph_constraint);

    // Ids are 1-based so that 0 can stay the "invalid" sentinel.
    return static_cast<uint32_t>(m_Joints.size());
}

bool PhysicsScene::RemoveJoint(uint32_t joint_id)
{
    if (joint_id == s_InvalidJointId || joint_id > m_Joints.size())
    {
        return false;
    }

    JPH::Constraint* jph_constraint = m_Joints[joint_id - 1];
    if (jph_constraint == nullptr)
    {
        return false;
    }

    m_Physics.m_JoltPhysicsSystem->RemoveConstraint(jph_constraint);
    jph_constraint->Release();
    m_Joints[joint_id - 1] = nullptr;

    return true;
}

void PhysicsScene::FilterHitsByLayer(std::vector<PhysicsHitInfo>& in_out_hits, uint32_t layer_mask)
{
    // The mask pass also *stamps* hit.layer, which is why it runs even when
    // layer_mask is "everything": an unfilled 0 would read as layer 0
    // ("Default") rather than "not resolved".
    std::vector<PhysicsHitInfo> kept;
    kept.reserve(in_out_hits.size());

    for (PhysicsHitInfo& hit : in_out_hits)
    {
        if (hit.body_id == s_InvalidRigidbodyId)
        {
            continue;
        }

        JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(),
                                    JPH::BodyID(hit.body_id));
        if (!body_lock.Succeeded())
        {
            // The body was removed between the query and this pass. Dropping it
            // is right: handing back a hit on a body that no longer exists is
            // worse than handing back one fewer hit.
            continue;
        }

        const int32_t layer = static_cast<int32_t>(body_lock.GetBody().GetObjectLayer());
        hit.layer = layer;

        if (PhysicsLayerMaskContains(layer_mask, layer))
        {
            kept.push_back(hit);
        }
    }

    in_out_hits.swap(kept);
}

bool PhysicsScene::Raycast(Vector3 ray_origin,
                           Vector3 ray_directory,
                           float ray_length,
                           std::vector<PhysicsHitInfo>& out_hits,
                           uint32_t layer_mask)
{
    out_hits.clear();

    const JPH::NarrowPhaseQuery& scene_query = m_Physics.m_JoltPhysicsSystem->GetNarrowPhaseQuery();

    JPH::RRayCast ray;
    ray.mOrigin = toVec3(ray_origin);
    ray.mDirection = toVec3(ray_directory.normalisedCopy() * ray_length);

    JPH::RayCastSettings raycast_setting;

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;

    scene_query.CastRay(ray, raycast_setting, collector);

    if (!collector.HadHit())
    {
        return false;
    }

    collector.Sort();

    std::vector<JPH::RayCastResult> raycast_results(collector.mHits.begin(), collector.mHits.end());

    out_hits.resize(raycast_results.size());

    for (size_t index = 0; index < raycast_results.size(); index++)
    {
        const JPH::RayCastResult& cast_result = raycast_results[index];

        PhysicsHitInfo& hit = out_hits[index];
        hit.hit_position = toVec3(ray.mOrigin + cast_result.mFraction * ray.mDirection);
        hit.hit_distance = cast_result.mFraction * ray_length;
        hit.body_id = cast_result.mBodyID.GetIndexAndSequenceNumber();

        // get hit normal
        JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), cast_result.mBodyID);
        const JPH::Body& hit_body = body_lock.GetBody();

        hit.hit_normal =
            toVec3(hit_body.GetWorldSpaceSurfaceNormal(cast_result.mSubShapeID2, toVec3(hit.hit_position)));
    }

    FilterHitsByLayer(out_hits, layer_mask);

    return !out_hits.empty();
}

bool PhysicsScene::Sweep(const RigidBodyShape& shape,
                         const Matrix4x4& shape_transform,
                         Vector3 sweep_direction,
                         float sweep_length,
                         std::vector<PhysicsHitInfo>& out_hits,
                         uint32_t layer_mask)
{
    out_hits.clear();

    const JPH::NarrowPhaseQuery& scene_query = m_Physics.m_JoltPhysicsSystem->GetNarrowPhaseQuery();

    const Matrix4x4 shape_global_transform = shape_transform * shape.m_LocalTransform.getMatrix();

    Vector3 global_position, global_scale;
    Quaternion global_rotation;

    shape_global_transform.Decomposition(global_position, global_scale, global_rotation);

    JPH::Shape* jph_shape = toShape(shape, global_scale);

    if (jph_shape == nullptr)
    {
        return false;
    }

    JPH::RShapeCast shape_cast(jph_shape,
                               JPH::Vec3::sReplicate(1.f),
                               toMat44(shape_global_transform),
                               toVec3(sweep_direction.normalisedCopy() * sweep_length));

    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    scene_query.CastShape(shape_cast, JPH::ShapeCastSettings(), JPH::RVec3Arg(), collector);
    if (!collector.HadHit())
    {
        return false;
    }

    collector.Sort();

    std::vector<JPH::ShapeCastResult> sweep_results(collector.mHits.begin(), collector.mHits.end());

    out_hits.resize(sweep_results.size());

    for (size_t index = 0; index < sweep_results.size(); index++)
    {
        const JPH::ShapeCastResult& sweep_result = sweep_results[index];

        PhysicsHitInfo& hit = out_hits[index];
        hit.hit_position = toVec3(sweep_result.mContactPointOn2);
        hit.hit_normal = toVec3(sweep_result.mPenetrationAxis.Normalized());
        hit.hit_distance = sweep_result.mFraction * sweep_length;
        hit.body_id = sweep_result.mBodyID2.GetIndexAndSequenceNumber();
    }

    FilterHitsByLayer(out_hits, layer_mask);

    return !out_hits.empty();
}

bool PhysicsScene::IsOverlap(const RigidBodyShape& shape,
                             const Matrix4x4& global_transform,
                             uint32_t layer_mask)
{
    // An "is there anything at all" query must NOT pay for the full result set,
    // so this stays on the AnyHit collector and simply resolves the mask
    // afterwards -- if the one body it found is masked out we cannot know
    // whether another one would have passed, so fall back to the full query.
    // That is the only correct answer and it is rare (a mask that excludes the
    // first hit but not everything).
    const JPH::NarrowPhaseQuery& scene_query = m_Physics.m_JoltPhysicsSystem->GetNarrowPhaseQuery();

    const Matrix4x4 shape_global_transform = global_transform * shape.m_LocalTransform.getMatrix();

    Vector3 global_position, global_scale;
    Quaternion global_rotation;

    shape_global_transform.Decomposition(global_position, global_scale, global_rotation);

    JPH::Shape* jph_shape = toShape(shape, global_scale);

    if (jph_shape == nullptr)
    {
        return false;
    }

    JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> collector;
    scene_query.CollideShape(jph_shape,
                             JPH::Vec3::sReplicate(1.0f),
                             toMat44(shape_global_transform),
                             JPH::CollideShapeSettings(),
                             JPH::Vec3(),
                             collector);

    if (!collector.HadHit())
    {
        return false;
    }

    if (layer_mask == kPhysicsAllLayers)
    {
        return true;
    }

    const JPH::BodyID hit_body_id = collector.mHit.mBodyID2;
    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), hit_body_id);
    if (body_lock.Succeeded() &&
        PhysicsLayerMaskContains(layer_mask, static_cast<int32_t>(body_lock.GetBody().GetObjectLayer())))
    {
        return true;
    }

    // The single hit was masked out: redo the query collecting everything so a
    // second, qualifying body is not missed.
    std::vector<PhysicsHitInfo> all_hits;
    return OverlapAll(shape, global_transform, all_hits, layer_mask);
}

bool PhysicsScene::OverlapAll(const RigidBodyShape& shape,
                              const Matrix4x4& global_transform,
                              std::vector<PhysicsHitInfo>& out_hits,
                              uint32_t layer_mask)
{
    out_hits.clear();

    const JPH::NarrowPhaseQuery& scene_query = m_Physics.m_JoltPhysicsSystem->GetNarrowPhaseQuery();

    const Matrix4x4 shape_global_transform = global_transform * shape.m_LocalTransform.getMatrix();

    Vector3 global_position, global_scale;
    Quaternion global_rotation;

    shape_global_transform.Decomposition(global_position, global_scale, global_rotation);

    JPH::Shape* jph_shape = toShape(shape, global_scale);

    if (jph_shape == nullptr)
    {
        return false;
    }

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    scene_query.CollideShape(jph_shape,
                             JPH::Vec3::sReplicate(1.0f),
                             toMat44(shape_global_transform),
                             JPH::CollideShapeSettings(),
                             JPH::Vec3(),
                             collector);

    if (!collector.HadHit())
    {
        return false;
    }

    out_hits.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult& overlap_result : collector.mHits)
    {
        PhysicsHitInfo hit;
        hit.hit_position = toVec3(overlap_result.mContactPointOn2);
        hit.hit_normal = toVec3(overlap_result.mPenetrationAxis.Normalized());
        // An overlap has no direction, so there is no meaningful distance.
        hit.hit_distance = 0.f;
        hit.body_id = overlap_result.mBodyID2.GetIndexAndSequenceNumber();
        out_hits.push_back(hit);
    }

    FilterHitsByLayer(out_hits, layer_mask);

    return !out_hits.empty();
}

bool PhysicsScene::CastPrimitive(const PhysicsPrimitiveDesc& desc,
                                 const Matrix4x4& world_transform,
                                 Vector3 sweep_direction,
                                 float sweep_length,
                                 std::vector<PhysicsHitInfo>& out_hits,
                                 uint32_t layer_mask)
{
    out_hits.clear();

    JPH::Shape* jph_shape = MakePrimitiveShape(desc);
    if (jph_shape == nullptr)
    {
        return false;
    }

    const JPH::NarrowPhaseQuery& scene_query = m_Physics.m_JoltPhysicsSystem->GetNarrowPhaseQuery();

    JPH::RShapeCast shape_cast(jph_shape,
                               JPH::Vec3::sReplicate(1.f),
                               toMat44(world_transform),
                               toVec3(sweep_direction.normalisedCopy() * sweep_length));

    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    scene_query.CastShape(shape_cast, JPH::ShapeCastSettings(), JPH::RVec3Arg(), collector);

    if (collector.HadHit())
    {
        collector.Sort();

        out_hits.reserve(collector.mHits.size());
        for (const JPH::ShapeCastResult& sweep_result : collector.mHits)
        {
            PhysicsHitInfo hit;
            hit.hit_position = toVec3(sweep_result.mContactPointOn2);
            hit.hit_normal = toVec3(sweep_result.mPenetrationAxis.Normalized());
            hit.hit_distance = sweep_result.mFraction * sweep_length;
            hit.body_id = sweep_result.mBodyID2.GetIndexAndSequenceNumber();
            out_hits.push_back(hit);
        }
    }

    // Owned by us, not by a RefTarget: Jolt did not take a reference.
    delete jph_shape;

    FilterHitsByLayer(out_hits, layer_mask);

    return !out_hits.empty();
}

bool PhysicsScene::OverlapPrimitive(const PhysicsPrimitiveDesc& desc,
                                    const Matrix4x4& world_transform,
                                    std::vector<PhysicsHitInfo>& out_hits,
                                    uint32_t layer_mask)
{
    out_hits.clear();

    JPH::Shape* jph_shape = MakePrimitiveShape(desc);
    if (jph_shape == nullptr)
    {
        return false;
    }

    const JPH::NarrowPhaseQuery& scene_query = m_Physics.m_JoltPhysicsSystem->GetNarrowPhaseQuery();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    scene_query.CollideShape(jph_shape,
                             JPH::Vec3::sReplicate(1.f),
                             toMat44(world_transform),
                             JPH::CollideShapeSettings(),
                             JPH::Vec3(),
                             collector);

    if (collector.HadHit())
    {
        out_hits.reserve(collector.mHits.size());
        for (const JPH::CollideShapeResult& overlap_result : collector.mHits)
        {
            PhysicsHitInfo hit;
            hit.hit_position = toVec3(overlap_result.mContactPointOn2);
            hit.hit_normal = toVec3(overlap_result.mPenetrationAxis.Normalized());
            hit.hit_distance = 0.f;
            hit.body_id = overlap_result.mBodyID2.GetIndexAndSequenceNumber();
            out_hits.push_back(hit);
        }
    }

    delete jph_shape;

    FilterHitsByLayer(out_hits, layer_mask);

    return !out_hits.empty();
}

void PhysicsScene::GetShapeBoundingBoxes(uint32_t body_id, std::vector<AxisAlignedBox>& out_bounding_boxes) const
{
    JPH::BodyLockRead body_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), JPH::BodyID(body_id));
    const JPH::Body& body = body_lock.GetBody();

    JPH::TransformedShape body_transformed_shape = body.GetTransformedShape();

    struct Collector : JPH::TransformedShapeCollector
    {
        virtual void AddHit(const ResultType& inResult) override { mShapes.push_back(inResult); }

        std::vector<JPH::TransformedShape> mShapes;
    };

    Collector collector;
    body_transformed_shape.CollectTransformedShapes(body_transformed_shape.GetWorldSpaceBounds(), collector);

    for (const JPH::TransformedShape& ts : collector.mShapes)
    {
        const JPH::Shape* shape = ts.mShape;

        assert(shape->GetType() == JPH::EShapeType::Convex);

        // RigidBodyShape* rigid_body_shape = GET_SYSTEM(ObjectManager)->NewObject<RigidBodyShape>();

        JPH::AABox jph_bounding_box = ts.GetWorldSpaceBounds();
        Vector3 center = toVec3(jph_bounding_box.GetCenter());
        Vector3 extent = toVec3(jph_bounding_box.GetExtent());

        out_bounding_boxes.emplace_back(center, extent);
    }
}

#ifdef ENABLE_PHYSICS_DEBUG_RENDERER
void PhysicsScene::DrawPhysicsScene(JPH::DebugRenderer* debug_renderer)
{
    #ifdef JPH_DEBUG_RENDERER
    m_Physics.m_JoltPhysicsSystem->DrawBodies(JPH::BodyManager::DrawSettings(), debug_renderer);

    #endif
}
#endif
