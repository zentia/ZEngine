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
#include "Jolt/Physics/Collision/Shape/EmptyShape.h"
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
#include "Jolt/Physics/Constraints/SixDOFConstraint.h"
#include "Jolt/Physics/Constraints/SliderConstraint.h"
#include "Jolt/Physics/Constraints/SwingTwistConstraint.h"
// `enableCollision` / `currentForce` 要拿约束两端的 body（`GetBody1/2` 与
// `GetConstraintToBody*Matrix` 都在这一层）。别指望被上面那些头文件顺带引进来。
#include "Jolt/Physics/Constraints/TwoBodyConstraint.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Core/Base/Macro.h"

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

    /// swing-twist 的平面轴：要的是"与扭转轴垂直的那条"，脚本给的可能只是"大致
    /// 垂直"甚至平行。
    ///
    /// ⚠️ 平行的情况不能交给 Jolt：它只做一次正交化，`cross(parallel, parallel)`
    /// 是零向量，后面 `.Normalized()` 就是 NaN —— 一个 NaN 约束会把整个
    /// broadphase 拖垮，而报错的位置离这里很远。所以这里先自己把"沿扭转轴的那
    /// 一份"减掉，减完太短就换 `MakePerpendicularAxis` 造一条。
    JPH::Vec3 PlaneAxisFor(const JPH::Vec3& in_twist_axis, const Vector3& in_requested)
    {
        JPH::Vec3 requested = toVec3(in_requested);
        const float along = requested.Dot(in_twist_axis);
        JPH::Vec3 orthogonal = requested - in_twist_axis * along;
        if (orthogonal.LengthSq() < 1.0e-6f)
        {
            return MakePerpendicularAxis(in_twist_axis);
        }
        return orthogonal.Normalized();
    }

    /// `ConfigurableJointAxis` -> Jolt 的 `SixDOFConstraintSettings::EAxis`。
    ///
    /// 两个枚举的取值顺序**刻意一致**（都先三个平移再三个旋转），所以这里就是一个
    /// 下标转换。单独抽成函数是为了让"顺序必须一致"这件事只在一个地方成立，并且
    /// 挡住越界 —— 轴是从脚本的 number 来的，可以乱给。
    bool JoltAxisFor(ConfigurableJointAxis in_axis, JPH::SixDOFConstraintSettings::EAxis& out_axis)
    {
        const int index = static_cast<int>(in_axis);
        if (index < 0 || index >= JPH::SixDOFConstraintSettings::EAxis::Num)
        {
            return false;
        }
        out_axis = static_cast<JPH::SixDOFConstraintSettings::EAxis>(index);
        return true;
    }

    /// 平移轴还是旋转轴 —— 决定 `maximumForce` 该当**力**还是**力矩**限幅。
    bool AxisIsTranslation(ConfigurableJointAxis in_axis)
    {
        return in_axis == ConfigurableJointAxis::TranslationX ||
               in_axis == ConfigurableJointAxis::TranslationY ||
               in_axis == ConfigurableJointAxis::TranslationZ;
    }

    /// 把一个驱动写进六自由度约束的某一个轴。
    ///
    /// ⚠️ "开不开"由 `positionSpring > 0` 决定，没有一个单独的开关：刚度为 0 的
    /// 位置电机在 Jolt 里没有意义（`MotorSettings::IsValid()` 过得去但
    /// `HasStiffness()` 是假，电机什么都不做），而 Unity 的 JointDrive 也是同一
    /// 条规则 —— `spring = 0` 就是没有驱动。
    ///
    /// ⚠️ 用 `PositionAndVelocity` 而不是 `Position`：这样 `positionDamper` 才是
    /// Unity 那个意思（`targetVelocity` 为 0 时公式退化成
    /// `F = k*(target - cur) - c*v`，正是 Unity 的 spring/damper）。
    bool ApplyDriveToAxis(JPH::SixDOFConstraint* in_joint, ConfigurableJointAxis in_axis, const JointDrive& in_drive)
    {
        JPH::SixDOFConstraintSettings::EAxis jolt_axis;
        if (!JoltAxisFor(in_axis, jolt_axis))
        {
            return false;
        }
        if (in_drive.positionDamper < 0.f || in_drive.maximumForce < 0.f)
        {
            return false;
        }

        JPH::MotorSettings& motor = in_joint->GetMotorSettings(jolt_axis);
        // ⚠️ 关掉的电机也把弹簧参数写下去：读回来要能拿到脚本写进去的那两个数，
        // 否则"设了 spring=0"和"从没设过"在 GetJointAxisDrive 上分不出来。
        motor.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping,
                                                   in_drive.positionSpring,
                                                   in_drive.positionDamper);
        if (AxisIsTranslation(in_axis))
        {
            motor.SetForceLimit(in_drive.maximumForce);
        }
        else
        {
            motor.SetTorqueLimit(in_drive.maximumForce);
        }
        in_joint->SetMotorState(jolt_axis,
                                in_drive.positionSpring > 0.f ? JPH::EMotorState::PositionAndVelocity
                                                              : JPH::EMotorState::Off);
        return true;
    }

    /// 最小质量（千克）。0 / 负质量会让倒数质量变成 inf，一步积分之后就是 NaN
    /// 位置，而一个 NaN 物体会把整个 broadphase 拖垮。
    constexpr float kMinMass = 0.0001f;

    /// 退化限位（min == max，即"锁死"）时必须给的限位弹簧频率。
    ///
    /// Jolt 在 HingeConstraint / SliderConstraint 的构造函数里断言
    /// `mLimitsMin != mLimitsMax || mLimitsSpringSettings.mFrequency > 0`
    /// —— 意思是"两个限位重合了，那你干脆用 fixed 约束"。可 Unity 的语义
    /// 不是这样：脚本写 `useLimits = true` 而还没挑区间时，min 和 max 都是 0，
    /// 用户想要的正是"锁死在这个角度"，不是"没有限位"。
    ///
    /// 一个断言在 shipped build 里比一条稍微软一点的限位糟得多，所以这里给
    /// 退化区间配一个真实的弹簧：频率不算高（不会抖），但足够硬。
    constexpr float kDegenerateLimitFrequency = 2.0f;
    constexpr float kDegenerateLimitDamping = 1.0f;

    /// Jolt 的 pi。写成自己的常量而不是 `JPH_PI`：那个宏住在 Jolt 的 Core.h 里，
    /// 而本文件只 include 了约束那几个头文件（Jolt-free 的那一半要靠它保持干净）。
    constexpr float kJoltPi = 3.14159265358979323846f;

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

uint32_t PhysicsScene::CreateStaticAnchorBody(const Vector3& world_position)
{
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();

    // `EmptyShape` 是 Shape（不是 ShapeSettings），所以只能走 `SetShape()`
    // 那条路。Body 的 `mShape` 是 `RefConst<Shape>`，赋值时会自己 AddRef ——
    // 也就是说这里 `new` 出来的形状由 Body 接管，函数返回后不会悬空。
    //
    // ⚠️ 但 `BodyCreationSettings` 在 Body 造出来**之前**只存裸指针，所以
    // 万一 CreateBody 失败要自己 delete。
    JPH::EmptyShape* empty_shape = new JPH::EmptyShape;

    JPH::BodyCreationSettings body_settings(empty_shape,
                                           JPH::RVec3(toVec3(world_position)),
                                           JPH::Quat::sIdentity(),
                                           JPH::EMotionType::Static,
                                           static_cast<JPH::ObjectLayer>(0));

    JPH::Body* jph_body = body_interface.CreateBody(body_settings);
    if (jph_body == nullptr)
    {
        delete empty_shape;
        LOG_ERROR(ZPhysics, "CreateStaticAnchorBody: Jolt refused to create the anchor body");
        return s_InvalidRigidbodyId;
    }

    // 锚点不参与碰撞，也就不需要被唤醒；DontActivate 省掉一次激活回调。
    body_interface.AddBody(jph_body->GetID(), JPH::EActivation::DontActivate);

    return jph_body->GetID().GetIndexAndSequenceNumber();
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
    //
    // ⚠️ 沿用**当前**的自由度掩码而不是写死 `EAllowedDOFs::All`：SetMassProperties
    // 把掩码和质量一起收，写死 All 等于"改一次质量就把 constraints 悄悄清掉"。
    // 那种 bug 的表现是"设了 freezeRotation，一改质量就又开始转了"。
    JPH::MassProperties mass_properties = jph_body.GetShape()->GetMassProperties();
    mass_properties.ScaleToMass(safe_mass);
    motion_properties->SetMassProperties(motion_properties->GetAllowedDOFs(), mass_properties);
    return true;
}

bool PhysicsScene::GetBodyAllowedDOFs(uint32_t body_id, uint32_t& out_dof_mask) const
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
        // 非动力学 body 没有 MotionProperties，也就没有"允许哪些轴"这回事 ——
        // 它本来就一动不动。
        out_dof_mask = 0;
        return false;
    }

    const JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        out_dof_mask = 0;
        return false;
    }

    out_dof_mask = static_cast<uint32_t>(motion_properties->GetAllowedDOFs());
    return true;
}

bool PhysicsScene::SetBodyAllowedDOFs(uint32_t body_id, uint32_t dof_mask, float mass)
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
    JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        return false;
    }

    const float safe_mass = (mass > kMinMass) ? mass : kMinMass;

    // 与 SetMass 同一条路：从形状重新推一份 MassProperties 再按当前质量缩放。
    // 不用 `motion_properties` 里现成的逆惯性反推 —— 位移一旦被冻住，逆质量就
    // 是 0，反推出来的质量是错的。
    JPH::MassProperties mass_properties = jph_body.GetShape()->GetMassProperties();
    mass_properties.ScaleToMass(safe_mass);
    motion_properties->SetMassProperties(static_cast<JPH::EAllowedDOFs>(dof_mask & 0x3fu), mass_properties);
    return true;
}

bool PhysicsScene::GetBodyCenterOfMass(uint32_t body_id, Vector3& out_local_center_of_mass) const
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
    const JPH::Shape* shape = jph_body.GetShape();
    if (shape == nullptr)
    {
        return false;
    }

    out_local_center_of_mass = toVec3(shape->GetCenterOfMass());
    return true;
}

bool PhysicsScene::GetBodyWorldCenterOfMass(uint32_t body_id, Vector3& out_world_center_of_mass) const
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

    // ⚠️ `Body::GetPosition()` 是**物体原点**，不是质心：
    //   `GetPosition()          = mPosition - rotation * shape->GetCenterOfMass()`
    //   `GetCenterOfMassPosition() = mPosition`
    // 形状质心不在原点时两者相差 `rotation * localCOM`。Unity 里这两个也是两个
    // 量（`rigidbody.position` 是 transform 位置、`worldCenterOfMass` 是质心），
    // 所以这里**不能**图省事用 `GetRigidBodyPosition()` 顶替。
    out_world_center_of_mass = toVec3(body_lock.GetBody().GetCenterOfMassPosition());
    return true;
}

bool PhysicsScene::GetBodyInertiaTensor(uint32_t body_id, Vector3& out_inertia, Quaternion& out_rotation) const
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
        // 非动力学 body 的逆惯性是 0 —— 也就是"惯性无穷大"，转不动。返回 0 让
        // 脚本侧"越大越难转"的直觉反过来成立不了，但返回 inf 更糟（会传播成
        // NaN）。调用方要看的是 `IsDynamic()` 而不该到这儿来问。
        out_inertia = Vector3::ZERO;
        out_rotation = Quaternion::IDENTITY;
        return false;
    }

    const JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        out_inertia = Vector3::ZERO;
        out_rotation = Quaternion::IDENTITY;
        return false;
    }

    const JPH::Vec3 inverse_diagonal = motion_properties->GetInverseInertiaDiagonal();
    out_inertia = Vector3(inverse_diagonal.GetX() > 0.f ? 1.f / inverse_diagonal.GetX() : 0.f,
                          inverse_diagonal.GetY() > 0.f ? 1.f / inverse_diagonal.GetY() : 0.f,
                          inverse_diagonal.GetZ() > 0.f ? 1.f / inverse_diagonal.GetZ() : 0.f);
    out_rotation = toQuat(motion_properties->GetInertiaRotation());
    return true;
}

bool PhysicsScene::GetBodyVolume(uint32_t body_id, float& out_volume) const
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

    const JPH::Shape* shape = body_lock.GetBody().GetShape();
    if (shape == nullptr)
    {
        return false;
    }

    out_volume = shape->GetVolume();
    return true;
}

bool PhysicsScene::GetBodyMotionQuality(uint32_t body_id, CollisionDetectionMode& out_mode) const
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
    const JPH::MotionProperties* motion_properties = jph_body.GetMotionPropertiesUnchecked();
    if (motion_properties == nullptr)
    {
        return false;
    }

    // 见 RigidBody.h 里 CollisionDetectionMode 的注释：Jolt 只有两态，三个连续
    // 模式读回来都退化为 Continuous。
    out_mode = (motion_properties->GetMotionQuality() == JPH::EMotionQuality::LinearCast)
                   ? CollisionDetectionMode::Continuous
                   : CollisionDetectionMode::Discrete;
    return true;
}

bool PhysicsScene::SetBodyMotionQuality(uint32_t body_id, CollisionDetectionMode mode)
{
    if (body_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    // 走 BodyInterface 而不是自己拿写锁：`MotionProperties` 上没有
    // `SetMotionQuality`（只有 getter），换运动质量必须经 `BodyManager`，而
    // BodyManager 会自己再拿一次写锁 —— 在外面先持一把就是 Jolt 那条
    // "lock of same or higher priority" 断言（真会死锁，不是警告）。
    //
    // ⚠️ 同理：这里**不能**在其它 BodyLockWrite 的作用域里被调用。
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.SetMotionQuality(JPH::BodyID(body_id),
                                    UsesContinuousCollision(mode) ? JPH::EMotionQuality::LinearCast
                                                                  : JPH::EMotionQuality::Discrete);
    return true;
}

bool PhysicsScene::GetBodyMaxAngularVelocity(uint32_t body_id, float& out_max_angular_velocity) const
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
        out_max_angular_velocity = 0.f;
        return false;
    }

    const JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        out_max_angular_velocity = 0.f;
        return false;
    }

    out_max_angular_velocity = motion_properties->GetMaxAngularVelocity();
    return true;
}

bool PhysicsScene::SetBodyMaxAngularVelocity(uint32_t body_id, float max_angular_velocity)
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
    JPH::MotionProperties* motion_properties = jph_body.GetMotionProperties();
    if (motion_properties == nullptr)
    {
        return false;
    }

    // Jolt 断言这个值必须 >= 0。夹到极小正数而不是把 0 传下去：0 表示"完全不能
    // 转"，那是 freezeRotation 的语义，不该由"上限"表达（上限是挡疯转的，不是
    // 开关）。
    const float safe_max = (max_angular_velocity > kMinMass) ? max_angular_velocity : kMinMass;
    motion_properties->SetMaxAngularVelocity(safe_max);
    return true;
}

bool PhysicsScene::GetBodyPointVelocity(uint32_t body_id, const Vector3& world_point, Vector3& out_velocity) const
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
        out_velocity = Vector3::ZERO;
        return false;
    }

    out_velocity = toVec3(jph_body.GetPointVelocity(JPH::RVec3(toVec3(world_point))));
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
            // Jolt 断言限位区间跨 0，且两端不能相等（除非带弹簧，见
            // `kDegenerateLimitFrequency` 的注释）。
            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)
            {
                settings.mLimitsMin = in_desc.m_MinLimit;
                settings.mLimitsMax = in_desc.m_MaxLimit;
                if (settings.mLimitsMin == settings.mLimitsMax)
                {
                    settings.mLimitsSpringSettings.mFrequency = kDegenerateLimitFrequency;
                    settings.mLimitsSpringSettings.mDamping = kDegenerateLimitDamping;
                }
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
            // 与铰链同一套规则（`<` / `>` 改成 `<=` / `>=` 是刻意的：滑块的
            // 0/0 同样是"锁死"，不该被静默丢弃成"没有限位"）。
            if (in_desc.m_MinLimit <= 0.f && in_desc.m_MaxLimit >= 0.f)
            {
                settings.mLimitsMin = in_desc.m_MinLimit;
                settings.mLimitsMax = in_desc.m_MaxLimit;
                if (settings.mLimitsMin == settings.mLimitsMax)
                {
                    settings.mLimitsSpringSettings.mFrequency = kDegenerateLimitFrequency;
                    settings.mLimitsSpringSettings.mDamping = kDegenerateLimitDamping;
                }
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
        case PhysicsJointType::swing_twist:
        {
            JPH::SwingTwistConstraintSettings settings;
            settings.mPosition1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
            settings.mPosition2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            settings.mTwistAxis1 = toVec3(in_desc.m_AxisA.normalisedCopy());
            settings.mTwistAxis2 = toVec3(in_desc.m_AxisB.normalisedCopy());
            // 平面轴只用来定出摆动的两个方向，Jolt 会自己正交化，但**平行的两条
            // 轴**正交化出来是零向量 —— 那是 NaN 的来源，所以这里自己处理：
            // 拿不到垂直分量就退回 MakePerpendicularAxis 那条路。
            settings.mPlaneAxis1 = PlaneAxisFor(settings.mTwistAxis1, in_desc.m_PlaneAxisA);
            settings.mPlaneAxis2 = PlaneAxisFor(settings.mTwistAxis2, in_desc.m_PlaneAxisB);
            settings.mNormalHalfConeAngle = std::max(in_desc.m_Swing1HalfAngle, 0.f);
            settings.mPlaneHalfConeAngle = std::max(in_desc.m_Swing2HalfAngle, 0.f);
            // Jolt 的 twist 区间也要跨 0（`mTwistMinAngle <= 0 <= mTwistMaxAngle`），
            // 与铰链同一条规则。写反了就拒掉而不是夹 —— 见 SetJointSwingTwistLimits。
            if (in_desc.m_TwistMinAngle <= 0.f && in_desc.m_TwistMaxAngle >= 0.f)
            {
                settings.mTwistMinAngle = in_desc.m_TwistMinAngle;
                settings.mTwistMaxAngle = in_desc.m_TwistMaxAngle;
            }
            settings.mSwingMotorSettings.mSpringSettings.mFrequency = in_desc.m_SwingSpringFrequency;
            settings.mSwingMotorSettings.mSpringSettings.mDamping = in_desc.m_SwingSpringDamping;
            settings.mTwistMotorSettings.mSpringSettings.mFrequency = in_desc.m_TwistSpringFrequency;
            settings.mTwistMotorSettings.mSpringSettings.mDamping = in_desc.m_TwistSpringDamping;
            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            break;
        }
        case PhysicsJointType::configurable:
        {
            JPH::SixDOFConstraintSettings settings;
            settings.mPosition1 = JPH::RVec3(toVec3(in_desc.m_PivotA));
            settings.mPosition2 = JPH::RVec3(toVec3(in_desc.m_PivotB));
            // ⚠️ 摆动型用 **Pyramid**，不是 Jolt 默认的 Cone。
            //
            // Cone 在 `UpdateRotationLimits` 里会**强行**把 Y / Z 两轴改成对称、
            // 并且只留 max（`mLimitMin[Y] = -mLimitMax[Y]`、`mLimitMax = max(0, ...)`），
            // 因为锥本来就是"一个半径"。而 Unity 的 `angularYLimit` /
            // `angularZLimit` 是**各自独立**的两个限位：脚本写 Y=0.5 / Z=0.2
            // 是完全正常的用法，Cone 会把它悄悄改成 Y=Z=0.5。Pyramid 才是
            // "两轴各自上下限"的模型（Jolt 的注释：pyramid 不需要对称）。
            settings.mSwingType = JPH::ESwingType::Pyramid;
            // 关节参考系：X = axis，Y = secondaryAxis，Z 由 Jolt 自己叉乘
            // （`axis_z = mAxisX.Cross(mAxisY)`）。
            //
            // ⚠️ 两条轴**平行**的后果和 swing-twist 那条平面轴一样：叉乘出零向量，
            // 后面 `Mat44(...).GetQuaternion()` 拿到一个退化旋转 ⇒ 姿态 NaN，一步
            // 之后整个 broadphase 被拖垮，而报错的位置离这里很远。所以走同一条
            // 正交化路。
            const JPH::Vec3 axis_x1 = toVec3(in_desc.m_AxisA.normalisedCopy());
            const JPH::Vec3 axis_x2 = toVec3(in_desc.m_AxisB.normalisedCopy());
            settings.mAxisX1 = axis_x1;
            settings.mAxisY1 = PlaneAxisFor(axis_x1, in_desc.m_SecondaryAxisA);
            settings.mAxisX2 = axis_x2;
            settings.mAxisY2 = PlaneAxisFor(axis_x2, in_desc.m_SecondaryAxisB);

            for (int i = 0; i < JPH::SixDOFConstraintSettings::EAxis::Num; ++i)
            {
                const JPH::SixDOFConstraintSettings::EAxis jolt_axis =
                    static_cast<JPH::SixDOFConstraintSettings::EAxis>(i);
                switch (in_desc.m_Motion[i])
                {
                    case ConfigurableJointMotion::Free:
                        settings.MakeFreeAxis(jolt_axis);
                        break;
                    case ConfigurableJointMotion::Locked:
                        settings.MakeFixedAxis(jolt_axis);
                        break;
                    case ConfigurableJointMotion::Limited:
                        // min > max 在 Jolt 里被当成"锁死"（`IsFixedAxis` 判的是
                        // min >= max）—— 那和 `Locked` 是两种不同的写法却一个意思，
                        // 正是"我明明写了区间却被当成焊死"这类最难查的错。拒掉。
                        if (in_desc.m_AxisLimitMin[i] > in_desc.m_AxisLimitMax[i])
                        {
                            LOG_ERROR(ZPhysics,
                                      "CreateJoint: configurable axis {} has an inverted limit range ({} .. {})",
                                      i,
                                      in_desc.m_AxisLimitMin[i],
                                      in_desc.m_AxisLimitMax[i]);
                            return s_InvalidJointId;
                        }
                        settings.SetLimitedAxis(jolt_axis, in_desc.m_AxisLimitMin[i], in_desc.m_AxisLimitMax[i]);
                        break;
                }
                // 电机初值一律关掉：Jolt 的默认是 `{FrequencyAndDamping, 2 Hz, 1}`，
                // 那是一组**看着像开着**的弹簧参数。不清零的话 `GetJointAxisDrive`
                // 读回来会报出一个脚本从没写过的 2.0。
                settings.mMotorSettings[i] =
                    JPH::MotorSettings(JPH::ESpringMode::StiffnessAndDamping, 0.f, 0.f);
            }

            // 线性限位弹簧：只有平移那三个轴有（`mLimitsSpringSettings` 只有
            // `NumTranslation` 个），所以三个轴共用一个值 —— 与 Unity 的
            // `linearLimitSpring` 一个字段管三个方向一致。
            for (int i = 0; i < JPH::SixDOFConstraintSettings::EAxis::NumTranslation; ++i)
            {
                settings.mLimitsSpringSettings[i] = JPH::SpringSettings(
                    JPH::ESpringMode::StiffnessAndDamping, in_desc.m_LinearLimitSpring, in_desc.m_LinearLimitDamper);
            }

            jph_constraint = settings.Create(*jph_body_a, *jph_body_b);
            if (jph_constraint == nullptr)
            {
                break;
            }

            // 驱动与四个目标量只能在约束**建好之后**写：`SetMotorState` 是
            // `SixDOFConstraint` 上的，settings 里没有。
            JPH::SixDOFConstraint* joint = static_cast<JPH::SixDOFConstraint*>(jph_constraint);
            for (int i = 0; i < JPH::SixDOFConstraintSettings::EAxis::Num; ++i)
            {
                if (!ApplyDriveToAxis(joint,
                                      static_cast<ConfigurableJointAxis>(i),
                                      in_desc.m_AxisDrive[i]))
                {
                    LOG_ERROR(ZPhysics, "CreateJoint: configurable axis {} has an invalid drive", i);
                    jph_constraint->Release();
                    return s_InvalidJointId;
                }
            }
            joint->SetTargetPositionCS(toVec3(in_desc.m_TargetPosition));
            joint->SetTargetVelocityCS(toVec3(in_desc.m_TargetVelocity));
            joint->SetTargetAngularVelocityCS(toVec3(in_desc.m_TargetAngularVelocity));
            // `BS` = body space：`R2 = R1 * inOrientation`，与 Unity 的
            // `targetRotation`（"相对宿主姿态还要再转多少"）同一个意思。
            joint->SetTargetOrientationBS(toQuat(in_desc.m_TargetRotation));
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

    // 拆关节要把 sub-group 还回池子，并把两个 body 恢复成默认（或换成还在管它们
    // 的另一个关节的 sub-group）。少了这一步池子会漏光，而且 body 会一直
    // "不跟别人碰" —— 一个在别处才看得出来的静默错误。
    if (m_JointSubGroups.find(joint_id) != m_JointSubGroups.end())
    {
        ApplyJointCollisionFilter(joint_id, true);
    }

    m_Physics.m_JoltPhysicsSystem->RemoveConstraint(jph_constraint);
    jph_constraint->Release();
    m_Joints[joint_id - 1] = nullptr;

    return true;
}

// 关节 id 是"1 基下标"，而 `RemoveJoint` 只把槽位置空、**不**收缩数组 ——
// 所以 id 永不复用，一个被拆掉的 id 之后永远读不到（这正是想要的：脚本手里
// 那个旧 id 不会忽然指向另一个关节）。
//
// ⚠️ 只认 `m_Joints` 这一份真值，不另存"id -> 类型"表：那份表会在 RemoveJoint
// 之后和真实约束分叉。
JPH::Constraint* PhysicsScene::JointAt(uint32_t joint_id) const
{
    if (joint_id == s_InvalidJointId || joint_id > m_Joints.size())
    {
        return nullptr;
    }
    return m_Joints[joint_id - 1];
}

bool PhysicsScene::GetJointType(uint32_t joint_id, PhysicsJointType& out_type) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Fixed:
            out_type = PhysicsJointType::fixed;
            return true;
        case JPH::EConstraintSubType::Hinge:
            out_type = PhysicsJointType::hinge;
            return true;
        case JPH::EConstraintSubType::Point:
            out_type = PhysicsJointType::point;
            return true;
        case JPH::EConstraintSubType::Slider:
            out_type = PhysicsJointType::slider;
            return true;
        case JPH::EConstraintSubType::Distance:
            out_type = PhysicsJointType::distance;
            return true;
        case JPH::EConstraintSubType::SwingTwist:
            out_type = PhysicsJointType::swing_twist;
            return true;
        case JPH::EConstraintSubType::SixDOF:
            out_type = PhysicsJointType::configurable;
            return true;
        default:
            // 引擎目前只建这七种；剩下的（齿轮 / 路径）今天没有入口，报"查不到"
            // 而不是猜一个。
            return false;
    }
}

bool PhysicsScene::SetJointEnabled(uint32_t joint_id, bool enabled)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    jph_constraint->SetEnabled(enabled);
    return true;
}

bool PhysicsScene::IsJointEnabled(uint32_t joint_id, bool& out_enabled) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    out_enabled = jph_constraint->GetEnabled();
    return true;
}

bool PhysicsScene::GetJointAngle(uint32_t joint_id, float& out_angle_radians) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::Hinge)
    {
        return false;
    }

    out_angle_radians = static_cast<const JPH::HingeConstraint*>(jph_constraint)->GetCurrentAngle();
    return true;
}

bool PhysicsScene::GetJointVelocity(uint32_t joint_id, float& out_radians_per_second) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::Hinge)
    {
        return false;
    }

    const JPH::HingeConstraint* hinge = static_cast<const JPH::HingeConstraint*>(jph_constraint);
    const JPH::BodyID body_ids[2] = {hinge->GetBody1()->GetID(), hinge->GetBody2()->GetID()};

    // 两个刚体要一起锁、且用多锁：Jolt 按线程记加锁顺序，两把同优先级的单体锁
    // 会撞 "lock of same or higher priority" 断言（见 CreateJoint 里那段注释）。
    //
    // ⚠️ `BodyLockMultiBase` 没有 `Succeeded()`（只有 `BodyLockWrite`/`BodyLockRead`
    // 才有）。失败与否看 `GetBody(i)` 是否为 nullptr —— 锁内部对无效 id 直接返空。
    JPH::BodyLockMultiRead joint_lock(m_Physics.m_JoltPhysicsSystem->GetBodyLockInterface(), body_ids, 2);
    const JPH::Body* body_1 = joint_lock.GetBody(0);
    const JPH::Body* body_2 = joint_lock.GetBody(1);
    if (body_1 == nullptr || body_2 == nullptr)
    {
        return false;
    }

    // 对象空间的铰链轴要过 `GetCenterOfMassTransform()`（质心变换）才到世界空间。
    // 用 `GetRotation()` 会漏掉形状质心偏移那一半。
    const JPH::Vec3 world_axis = body_1->GetCenterOfMassTransform().Multiply3x3(hinge->GetLocalSpaceHingeAxis1());
    const JPH::Vec3 relative_angular = body_1->GetAngularVelocity() - body_2->GetAngularVelocity();

    out_radians_per_second = relative_angular.Dot(world_axis);
    return true;
}

bool PhysicsScene::SetJointLimits(uint32_t joint_id, float min_limit, float max_limit)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    // min > max 不是一个区间。Jolt 的 SetLimits 会断言，这里直接拒掉 —— 脚本里
    // "把上下限写反了"是常见笔误，而拒掉比在求解器里断言崩掉好诊断。
    if (min_limit > max_limit)
    {
        return false;
    }

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Hinge:
            static_cast<JPH::HingeConstraint*>(jph_constraint)->SetLimits(min_limit, max_limit);
            return true;
        case JPH::EConstraintSubType::Slider:
            static_cast<JPH::SliderConstraint*>(jph_constraint)->SetLimits(min_limit, max_limit);
            return true;
        default:
            return false;
    }
}

bool PhysicsScene::GetJointLimits(uint32_t joint_id, float& out_min_limit, float& out_max_limit) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Hinge:
        {
            const JPH::HingeConstraint* hinge = static_cast<const JPH::HingeConstraint*>(jph_constraint);
            out_min_limit = hinge->GetLimitsMin();
            out_max_limit = hinge->GetLimitsMax();
            return true;
        }
        case JPH::EConstraintSubType::Slider:
        {
            const JPH::SliderConstraint* slider = static_cast<const JPH::SliderConstraint*>(jph_constraint);
            out_min_limit = slider->GetLimitsMin();
            out_max_limit = slider->GetLimitsMax();
            return true;
        }
        default:
            return false;
    }
}

bool PhysicsScene::SetJointMotor(uint32_t joint_id, bool enabled, float target, float max_force)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    // 力矩 / 力必须非负：`SetTorqueLimit` 给的是**对称**区间 [-m, +m]，传一个负
    // 值会断言（min > max）。0 是合法的 —— 那就是"电机使不上劲"，而关掉电机是
    // `enabled = false` 那件事，不该靠 0 表达。
    if (max_force < 0.f)
    {
        return false;
    }

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Hinge:
        {
            JPH::HingeConstraint* hinge = static_cast<JPH::HingeConstraint*>(jph_constraint);
            hinge->GetMotorSettings().SetTorqueLimit(max_force);
            hinge->SetTargetAngularVelocity(target);
            // Velocity 而不是 Position：Unity 的 `HingeJoint.motor` 驱动的也是
            // 目标角速度（要"转到某个角度"走限位 + 弹簧）。
            hinge->SetMotorState(enabled ? JPH::EMotorState::Velocity : JPH::EMotorState::Off);
            return true;
        }
        case JPH::EConstraintSubType::Slider:
        {
            JPH::SliderConstraint* slider = static_cast<JPH::SliderConstraint*>(jph_constraint);
            slider->GetMotorSettings().SetForceLimit(max_force);
            slider->SetTargetPosition(target);
            slider->SetMotorState(enabled ? JPH::EMotorState::Position : JPH::EMotorState::Off);
            return true;
        }
        default:
            return false;
    }
}

bool PhysicsScene::SetJointPositionMotor(uint32_t joint_id,
                                        bool enabled,
                                        float target,
                                        float max_force,
                                        float frequency_hz,
                                        float damping)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    if (max_force < 0.f || frequency_hz <= 0.f || damping < 0.f)
    {
        return false;
    }

    const JPH::MotorSettings motor_settings(JPH::ESpringMode::FrequencyAndDamping, frequency_hz, damping);

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Hinge:
        {
            JPH::HingeConstraint* hinge = static_cast<JPH::HingeConstraint*>(jph_constraint);
            JPH::MotorSettings settings = motor_settings;
            settings.SetTorqueLimit(max_force);
            hinge->GetMotorSettings() = settings;
            hinge->SetTargetAngle(target);
            hinge->SetMotorState(enabled ? JPH::EMotorState::Position : JPH::EMotorState::Off);
            return true;
        }
        case JPH::EConstraintSubType::Slider:
        {
            JPH::SliderConstraint* slider = static_cast<JPH::SliderConstraint*>(jph_constraint);
            JPH::MotorSettings settings = motor_settings;
            settings.SetForceLimit(max_force);
            slider->GetMotorSettings() = settings;
            slider->SetTargetPosition(target);
            slider->SetMotorState(enabled ? JPH::EMotorState::Position : JPH::EMotorState::Off);
            return true;
        }
        default:
            return false;
    }
}

bool PhysicsScene::SetJointDistance(uint32_t joint_id, float min_distance, float max_distance)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::Distance)
    {
        return false;
    }

    if (min_distance > max_distance)
    {
        return false;
    }

    static_cast<JPH::DistanceConstraint*>(jph_constraint)->SetDistance(min_distance, max_distance);
    return true;
}

bool PhysicsScene::GetJointDistance(uint32_t joint_id, float& out_min_distance, float& out_max_distance) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::Distance)
    {
        return false;
    }

    const JPH::DistanceConstraint* distance_constraint =
        static_cast<const JPH::DistanceConstraint*>(jph_constraint);
    out_min_distance = distance_constraint->GetMinDistance();
    out_max_distance = distance_constraint->GetMaxDistance();
    return true;
}

bool PhysicsScene::SetJointSpring(uint32_t joint_id, float frequency_hz, float damping)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    if (frequency_hz < 0.f || damping < 0.f)
    {
        return false;
    }

    // FrequencyAndDamping：Unity 的 `SoftJointLimitSpring.spring` / `.damper`
    // 也是这一对。frequency 为 0 时 Jolt 退化成硬限位 —— 与 Unity 里
    // `spring = 0` 的意思一致（不是"没有弹簧"）。
    const JPH::SpringSettings spring_settings(JPH::ESpringMode::FrequencyAndDamping, frequency_hz, damping);

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Distance:
            static_cast<JPH::DistanceConstraint*>(jph_constraint)->SetLimitsSpringSettings(spring_settings);
            return true;
        case JPH::EConstraintSubType::Hinge:
            static_cast<JPH::HingeConstraint*>(jph_constraint)->SetLimitsSpringSettings(spring_settings);
            return true;
        case JPH::EConstraintSubType::Slider:
            static_cast<JPH::SliderConstraint*>(jph_constraint)->SetLimitsSpringSettings(spring_settings);
            return true;
        default:
            return false;
    }
}

bool PhysicsScene::SetJointSwingTwistLimits(uint32_t joint_id,
                                           float swing1_half_angle,
                                           float swing2_half_angle,
                                           float twist_min_angle,
                                           float twist_max_angle)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SwingTwist)
    {
        return false;
    }

    // 半锥角是对称的（Jolt 存的是一个"半角"），负值只能是笔误。
    if (swing1_half_angle < 0.f || swing2_half_angle < 0.f)
    {
        return false;
    }
    // 扭转轴与铰链同一条规则：区间必须跨 0（"相对建立姿态左右各多少"）。
    if (twist_min_angle > 0.f || twist_max_angle < 0.f)
    {
        return false;
    }
    if (twist_min_angle < -kJoltPi || twist_max_angle > kJoltPi)
    {
        return false;
    }

    JPH::SwingTwistConstraint* joint = static_cast<JPH::SwingTwistConstraint*>(jph_constraint);
    joint->SetNormalHalfConeAngle(swing1_half_angle);
    joint->SetPlaneHalfConeAngle(swing2_half_angle);
    joint->SetTwistMinAngle(twist_min_angle);
    joint->SetTwistMaxAngle(twist_max_angle);
    return true;
}

bool PhysicsScene::GetJointSwingTwistLimits(uint32_t joint_id,
                                           float& out_swing1_half_angle,
                                           float& out_swing2_half_angle,
                                           float& out_twist_min_angle,
                                           float& out_twist_max_angle) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SwingTwist)
    {
        return false;
    }

    const JPH::SwingTwistConstraint* joint = static_cast<const JPH::SwingTwistConstraint*>(jph_constraint);
    out_swing1_half_angle = joint->GetNormalHalfConeAngle();
    out_swing2_half_angle = joint->GetPlaneHalfConeAngle();
    out_twist_min_angle = joint->GetTwistMinAngle();
    out_twist_max_angle = joint->GetTwistMaxAngle();
    return true;
}

bool PhysicsScene::SetJointSwingTwistSpring(uint32_t joint_id,
                                           float swing_frequency_hz,
                                           float swing_damping,
                                           float twist_frequency_hz,
                                           float twist_damping)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SwingTwist)
    {
        return false;
    }

    if (swing_frequency_hz < 0.f || swing_damping < 0.f || twist_frequency_hz < 0.f || twist_damping < 0.f)
    {
        return false;
    }

    JPH::SwingTwistConstraint* joint = static_cast<JPH::SwingTwistConstraint*>(jph_constraint);
    joint->GetSwingMotorSettings().mSpringSettings = JPH::SpringSettings(
        JPH::ESpringMode::FrequencyAndDamping, swing_frequency_hz, swing_damping);
    joint->GetTwistMotorSettings().mSpringSettings = JPH::SpringSettings(
        JPH::ESpringMode::FrequencyAndDamping, twist_frequency_hz, twist_damping);
    return true;
}

bool PhysicsScene::GetJointSwingTwistSpring(uint32_t joint_id,
                                           float& out_swing_frequency_hz,
                                           float& out_swing_damping,
                                           float& out_twist_frequency_hz,
                                           float& out_twist_damping) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SwingTwist)
    {
        return false;
    }

    const JPH::SwingTwistConstraint* joint = static_cast<const JPH::SwingTwistConstraint*>(jph_constraint);
    out_swing_frequency_hz = joint->GetSwingMotorSettings().mSpringSettings.mFrequency;
    out_swing_damping = joint->GetSwingMotorSettings().mSpringSettings.mDamping;
    out_twist_frequency_hz = joint->GetTwistMotorSettings().mSpringSettings.mFrequency;
    out_twist_damping = joint->GetTwistMotorSettings().mSpringSettings.mDamping;
    return true;
}

bool PhysicsScene::GetJointImpulse(uint32_t joint_id, float& out_linear_impulse, float& out_angular_impulse) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    out_linear_impulse = 0.f;
    out_angular_impulse = 0.f;

    switch (jph_constraint->GetSubType())
    {
        case JPH::EConstraintSubType::Fixed:
        {
            const JPH::FixedConstraint* fixed = static_cast<const JPH::FixedConstraint*>(jph_constraint);
            out_linear_impulse = fixed->GetTotalLambdaPosition().Length();
            out_angular_impulse = fixed->GetTotalLambdaRotation().Length();
            return true;
        }
        case JPH::EConstraintSubType::Point:
        {
            // 点约束只焊位置，不传力矩 —— 这正是"球窝"和"焊死"的差别。
            out_linear_impulse =
                static_cast<const JPH::PointConstraint*>(jph_constraint)->GetTotalLambdaPosition().Length();
            return true;
        }
        case JPH::EConstraintSubType::Hinge:
        {
            const JPH::HingeConstraint* hinge = static_cast<const JPH::HingeConstraint*>(jph_constraint);
            out_linear_impulse = hinge->GetTotalLambdaPosition().Length();
            // 两股力矩：保持轴对齐的那两维，加上撞限位时的那一股。取较大的那
            // 一股而不是相加 —— 相加会把"轴对齐"和"撞限位"这两件同时发生的事
            // 算成两倍，于是限位一碰就断。
            const float axis_torque = hinge->GetTotalLambdaRotation().Length();
            const float limit_torque = std::abs(hinge->GetTotalLambdaRotationLimits());
            out_angular_impulse = std::max(axis_torque, limit_torque);
            return true;
        }
        case JPH::EConstraintSubType::Slider:
        {
            const JPH::SliderConstraint* slider = static_cast<const JPH::SliderConstraint*>(jph_constraint);
            out_linear_impulse = slider->GetTotalLambdaPosition().Length();
            out_angular_impulse = slider->GetTotalLambdaRotation().Length();
            return true;
        }
        case JPH::EConstraintSubType::Distance:
        {
            out_linear_impulse = std::abs(
                static_cast<const JPH::DistanceConstraint*>(jph_constraint)->GetTotalLambdaPosition());
            return true;
        }
        case JPH::EConstraintSubType::SwingTwist:
        {
            const JPH::SwingTwistConstraint* joint =
                static_cast<const JPH::SwingTwistConstraint*>(jph_constraint);
            out_linear_impulse = joint->GetTotalLambdaPosition().Length();
            // 三股力矩各活在自己的方向上（扭转 / 摆动两维），取最大的那一股：
            // 相加会把"同时撞上锥面和扭到头"算成三倍，于是布娃娃一着地就断。
            out_angular_impulse = std::max(std::abs(joint->GetTotalLambdaTwist()),
                                           std::max(std::abs(joint->GetTotalLambdaSwingY()),
                                                    std::abs(joint->GetTotalLambdaSwingZ())));
            return true;
        }
        case JPH::EConstraintSubType::SixDOF:
        {
            const JPH::SixDOFConstraint* joint = static_cast<const JPH::SixDOFConstraint*>(jph_constraint);
            // 六自由度这里能拿到**矢量**（`Vec3`），但仍然只给模长：这个 lambda 活
            // 在**约束空间**里（`mConstraintToBody1` 那个参考系），不是世界空间。
            // 拼成一个 Vector3 会得到一个看着像世界矢量、其实不是的东西 ——
            // `GetJointImpulse` 那条注释写的就是这件事。
            out_linear_impulse = joint->GetTotalLambdaPosition().Length();
            out_angular_impulse = joint->GetTotalLambdaRotation().Length();
            return true;
        }
        default:
            return false;
    }
}

// -----------------------------------------------------------------------------
// enableCollision / currentForce（Unity 的 Joint.enableCollision / currentForce）
// -----------------------------------------------------------------------------

namespace
{
    /// 所有"关了碰撞"的 body 共用一个 group id。
    ///
    /// Jolt 的判定里 group id 不同就直接返回"碰"，所以这个值只要**不等于**
    /// `cInvalidGroup` 就行；没入组的 body 保持默认，第一个分支就放行。
    constexpr JPH::CollisionGroup::GroupID kJointCollisionGroup = 1;

    /// 约束矩阵某一列在**世界**空间的方向。Jolt 存的是 body 局部空间那一份
    /// （`Mat44(Vec4(mLocalSpaceHingeAxis1, 0), ...)`），要乘 body 的旋转。
    JPH::Vec3 WorldAxis(const JPH::Body& in_body, const JPH::Mat44& in_c_to_b, int in_column)
    {
        return JPH::Mat44::sRotation(in_body.GetRotation()).Multiply3x3(in_c_to_b.GetColumn3(in_column));
    }

    /// 约束作用在 body 上的那个锚点（世界空间）。与 `TwoBodyConstraint::Draw` 里
    /// `GetCenterOfMassTransform() * GetConstraintToBody1Matrix()` 同一条路。
    JPH::RVec3 WorldAnchor(const JPH::Body& in_body, const JPH::Mat44& in_c_to_b)
    {
        // `RMat44 * Mat44` 出来还是**矩阵**，取平移列才是那个点。
        return (in_body.GetCenterOfMassTransform() * in_c_to_b).GetTranslation();
    }
}  // namespace

bool PhysicsScene::SetJointEnableCollision(uint32_t joint_id, bool in_enable)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }
    // 只有两体约束有"两个 body"可关。
    if (jph_constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint)
    {
        return false;
    }
    return ApplyJointCollisionFilter(joint_id, in_enable);
}

bool PhysicsScene::GetJointEnableCollision(uint32_t joint_id, bool& out_enable) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }
    if (jph_constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint)
    {
        return false;
    }
    // 表里没有 = 没关碰撞（默认）。
    out_enable = (m_JointSubGroups.find(joint_id) == m_JointSubGroups.end());
    return true;
}

void PhysicsScene::EnsureGroupFilter()
{
    if (m_JointGroupFilter == nullptr)
    {
        // `new` 出来的 RefTarget 引用计数是 0，交给 `JPH::Ref` 时会 AddRef。
        m_JointGroupFilter = new JPH::GroupFilterTable(kMaxJointSubGroups);
        m_SubGroupUsed.assign(kMaxJointSubGroups, false);
    }
}

bool PhysicsScene::ApplyJointCollisionFilter(uint32_t joint_id, bool in_enable)
{
    JPH::Constraint* jph_constraint = m_Joints[joint_id - 1];
    JPH::TwoBodyConstraint* two_body = static_cast<JPH::TwoBodyConstraint*>(jph_constraint);
    const uint32_t body_a = two_body->GetBody1()->GetID().GetIndexAndSequenceNumber();
    const uint32_t body_b = two_body->GetBody2()->GetID().GetIndexAndSequenceNumber();

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();

    if (in_enable)
    {
        const auto it = m_JointSubGroups.find(joint_id);
        if (it == m_JointSubGroups.end())
        {
            return true;   // 本来就没关
        }
        m_SubGroupUsed[it->second] = false;
        m_JointSubGroups.erase(it);
        RefreshBodyCollisionGroup(body_a);
        RefreshBodyCollisionGroup(body_b);
        return true;
    }

    // 已经关着就别再租一个 —— 重复调用会把池子漏光。
    if (m_JointSubGroups.find(joint_id) != m_JointSubGroups.end())
    {
        return true;
    }

    EnsureGroupFilter();
    uint32_t slot = kMaxJointSubGroups;
    for (uint32_t i = 0; i < kMaxJointSubGroups; ++i)
    {
        if (!m_SubGroupUsed[i])
        {
            slot = i;
            break;
        }
    }
    if (slot == kMaxJointSubGroups)
    {
        // ⚠️ 池满要**报出来**。静默退化成"不管"正是这个引擎避免的一类坑：脚本
        // 以为关掉了，body 照样互相弹。
        LOG_WARNING(ZPhysics,
                    "SetJointEnableCollision: sub-group pool exhausted (%u joints) -- this pair keeps colliding",
                    kMaxJointSubGroups);
        return false;
    }

    m_SubGroupUsed[slot] = true;
    m_JointSubGroups[joint_id] = slot;
    const JPH::CollisionGroup group(m_JointGroupFilter.GetPtr(), kJointCollisionGroup, slot);
    body_interface.SetCollisionGroup(JPH::BodyID(body_a), group);
    body_interface.SetCollisionGroup(JPH::BodyID(body_b), group);
    return true;
}

void PhysicsScene::RefreshBodyCollisionGroup(uint32_t body_index)
{
    // 还有别的关节在管这个 body 吗？有就换成那个的 sub-group（一个 body 只能有
    // 一个 sub-group，所以后一个关节会顶掉前一个 —— 这是写进文档的已知限制）。
    bool managed = false;
    uint32_t sub_group = 0;
    for (const auto& entry : m_JointSubGroups)
    {
        JPH::Constraint* jph_constraint = JointAt(entry.first);
        if (jph_constraint == nullptr ||
            jph_constraint->GetType() != JPH::EConstraintType::TwoBodyConstraint)
        {
            continue;
        }
        JPH::TwoBodyConstraint* two_body = static_cast<JPH::TwoBodyConstraint*>(jph_constraint);
        if (two_body->GetBody1()->GetID().GetIndexAndSequenceNumber() == body_index ||
            two_body->GetBody2()->GetID().GetIndexAndSequenceNumber() == body_index)
        {
            managed = true;
            sub_group = entry.second;
            break;
        }
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    if (!managed)
    {
        // 恢复默认：`cInvalidGroup` 那条分支让这个 body 与所有东西正常碰撞。
        body_interface.SetCollisionGroup(JPH::BodyID(body_index), JPH::CollisionGroup());
        return;
    }
    body_interface.SetCollisionGroup(
        JPH::BodyID(body_index),
        JPH::CollisionGroup(m_JointGroupFilter.GetPtr(), kJointCollisionGroup, sub_group));
}

bool PhysicsScene::GetJointForce(uint32_t joint_id, Vector3& out_force) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr)
    {
        return false;
    }

    // ⚠️ 除的是**物理步长**，不是 `Tick(delta_time)` 的形参：引擎是固定步长，那个
    // 形参在 `Tick` 里压根没被用。拿它去除会在变帧率下给出错的数，而它还停留在
    // "看起来合理"的量级上。
    const float step = 1.f / m_Config.m_UpdateFrequency;
    if (!(step > 0.f))
    {
        return false;
    }

    JPH::Vec3 world_impulse = JPH::Vec3::sZero();

    switch (jph_constraint->GetSubType())
    {
        // --- 线性 lambda 直接就是世界空间冲量 --------------------------------
        // `PointConstraintPart` 的有效质量矩阵用世界空间的 mR1/mR2 与世界空间逆
        // 惯量张量建的，lambda 以 `invM * lambda` 直接加到线速度上。
        case JPH::EConstraintSubType::Fixed:
            world_impulse =
                static_cast<const JPH::FixedConstraint*>(jph_constraint)->GetTotalLambdaPosition();
            break;
        case JPH::EConstraintSubType::Point:
            world_impulse =
                static_cast<const JPH::PointConstraint*>(jph_constraint)->GetTotalLambdaPosition();
            break;
        case JPH::EConstraintSubType::Hinge:
            world_impulse =
                static_cast<const JPH::HingeConstraint*>(jph_constraint)->GetTotalLambdaPosition();
            break;
        case JPH::EConstraintSubType::SwingTwist:
            world_impulse =
                static_cast<const JPH::SwingTwistConstraint*>(jph_constraint)->GetTotalLambdaPosition();
            break;

        case JPH::EConstraintSubType::Slider:
        {
            // 三个分量各来自**不同的约束部件**，别顺手补 0：
            //   Vector<2>  = 两条 normal 上的（约束矩阵 col1 / col2）
            //   limits 标量 = 沿滑块轴（约束矩阵 col0）
            const JPH::SliderConstraint* slider =
                static_cast<const JPH::SliderConstraint*>(jph_constraint);
            // `Vector<2>` 到 `Vec3` 没有隐式转换，也别去猜它的类型名 —— 用 auto
            // 拿下标就行。
            const auto lambda2 = slider->GetTotalLambdaPosition();
            const JPH::Mat44 c_to_b1 = slider->GetConstraintToBody1Matrix();
            world_impulse = WorldAxis(*slider->GetBody1(), c_to_b1, 1) * lambda2[0] +
                            WorldAxis(*slider->GetBody1(), c_to_b1, 2) * lambda2[1] +
                            WorldAxis(*slider->GetBody1(), c_to_b1, 0) *
                                slider->GetTotalLambdaPositionLimits();
            break;
        }

        case JPH::EConstraintSubType::Distance:
        {
            // 只有一个标量，方向是两点连线（自己取 —— 少这一步就是"大小对、方向
            // 是猜的"）。
            const JPH::DistanceConstraint* distance =
                static_cast<const JPH::DistanceConstraint*>(jph_constraint);
            const JPH::RVec3 anchor1 = WorldAnchor(*distance->GetBody1(),
                                                   distance->GetConstraintToBody1Matrix());
            const JPH::RVec3 anchor2 = WorldAnchor(*distance->GetBody2(),
                                                   distance->GetConstraintToBody2Matrix());
            const JPH::RVec3 delta = anchor2 - anchor1;
            const JPH::Vec3 axis(static_cast<float>(delta.GetX()),
                                 static_cast<float>(delta.GetY()),
                                 static_cast<float>(delta.GetZ()));
            if (axis.IsNearZero())
            {
                // 两点重合 ⇒ 方向未定义。给一个零向量比给一个随机方向好。
                world_impulse = JPH::Vec3::sZero();
            }
            else
            {
                world_impulse = axis.Normalized() * distance->GetTotalLambdaPosition();
            }
            break;
        }

        case JPH::EConstraintSubType::SixDOF:
        {
            const JPH::SixDOFConstraint* six_dof =
                static_cast<const JPH::SixDOFConstraint*>(jph_constraint);
            world_impulse = six_dof->GetTotalLambdaPosition();
            // ⚠️ `GetTotalLambdaPosition()` 是**二选一**：平移全约束时走
            // `PointConstraintPart`（世界空间），否则走三个逐轴部件（**约束空间**）。
            // 少判这一个分支，只有一两个轴被约束的关节会拿到一个转错了方向的力。
            //
            // `IsTranslationFullyConstrained()` 是 private，按它的定义还原：
            //   (mFixedAxis & 0b111) == 0b111 && !mHasSpringLimits
            // `IsFixedAxis(axis)` 就是 `(mFixedAxis & (1 << axis)) != 0`，三个平移
            // 轴正好是 bit 0/1/2 ⇒ 三项与运算与 `& 0b111 == 0b111` 等价。
            // `mHasSpringLimits` 是"任一平移轴的限位弹簧频率 > 0" —— 注意它读的是
            // `mFrequency`，而它与 `mStiffness` 是同一个 union，所以我们写进去的
            // stiffness 在那儿读出来是同一个数（Jolt 自己就是这么判的）。
            const bool has_spring_limits =
                six_dof->GetLimitsSpringSettings(JPH::SixDOFConstraintSettings::TranslationX).mFrequency >
                    0.f ||
                six_dof->GetLimitsSpringSettings(JPH::SixDOFConstraintSettings::TranslationY).mFrequency >
                    0.f ||
                six_dof->GetLimitsSpringSettings(JPH::SixDOFConstraintSettings::TranslationZ).mFrequency >
                    0.f;
            const bool fully_constrained =
                six_dof->IsFixedAxis(JPH::SixDOFConstraintSettings::TranslationX) &&
                six_dof->IsFixedAxis(JPH::SixDOFConstraintSettings::TranslationY) &&
                six_dof->IsFixedAxis(JPH::SixDOFConstraintSettings::TranslationZ) && !has_spring_limits;
            if (!fully_constrained)
            {
                const JPH::Quat c_to_b1 = six_dof->GetConstraintToBody1Matrix().GetQuaternion();
                const JPH::Quat to_world = six_dof->GetBody1()->GetRotation() * c_to_b1;
                world_impulse = to_world * world_impulse;
            }
            break;
        }

        default:
            return false;
    }

    // ⚠️ 符号：`PointConstraintPart::ApplyVelocityStep` 给 body1 **减** lambda、
    // 给 body2 **加** lambda。而我们的 `m_BodyA` 是**宿主自己**（Unity 的
    // `Joint.currentForce` 是"关节施加到它自己刚体上的力"），所以取负。
    // 拿错方向会得到一对大小全对、方向全反的力 —— 这种错在"能跑"上看不出来。
    out_force = (-toVec3(world_impulse)) / step;
    return true;
}

// -----------------------------------------------------------------------------
// configurable（Unity 的 ConfigurableJoint）
// -----------------------------------------------------------------------------

bool PhysicsScene::SetJointAxisMotion(uint32_t joint_id,
                                     ConfigurableJointAxis axis,
                                     ConfigurableJointMotion motion,
                                     float limit_min,
                                     float limit_max)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }
    JPH::SixDOFConstraintSettings::EAxis jolt_axis;
    if (!JoltAxisFor(axis, jolt_axis))
    {
        return false;
    }
    if (motion == ConfigurableJointMotion::Limited && limit_min > limit_max)
    {
        return false;
    }

    // 这个轴要写进 Jolt 的那个"限位区间"是什么。
    //
    // ⚠️ `Locked` 是**反序**区间（min = FLT_MAX / max = -FLT_MAX）：Jolt 的
    // `IsFixedAxis` 判的是 `min >= max`，那一条分支会把这个自由度驱动到 0 ——
    // 这正是 Unity 的 Locked 的意思，不是一个可以顺手夹一夹的非法输入。
    float new_min = 0.f;
    float new_max = 0.f;
    switch (motion)
    {
        case ConfigurableJointMotion::Free:
            new_min = -FLT_MAX;
            new_max = FLT_MAX;
            break;
        case ConfigurableJointMotion::Locked:
            new_min = FLT_MAX;
            new_max = -FLT_MAX;
            break;
        case ConfigurableJointMotion::Limited:
            new_min = limit_min;
            new_max = limit_max;
            break;
    }

    // Jolt 的平移 / 旋转限位各自是一个 `Vec3` **整体** setter（三个轴一起写），
    // 所以先读回当前三个轴、只改其中这一个、再整个写回去。少了读改写这一步，
    // "改一个轴"会顺手把另外两个轴也改掉 —— 而限位被改宽了只在物理表现上看得
    // 出来，不报错。
    JPH::SixDOFConstraint* joint = static_cast<JPH::SixDOFConstraint*>(jph_constraint);
    const bool translation = jolt_axis <= JPH::SixDOFConstraintSettings::TranslationZ;
    const int component = translation
                              ? static_cast<int>(jolt_axis)
                              : static_cast<int>(jolt_axis) -
                                    static_cast<int>(JPH::SixDOFConstraintSettings::RotationX);
    if (translation)
    {
        JPH::Vec3 min = joint->GetTranslationLimitsMin();
        JPH::Vec3 max = joint->GetTranslationLimitsMax();
        min.SetComponent(component, new_min);
        max.SetComponent(component, new_max);
        joint->SetTranslationLimits(min, max);
    }
    else
    {
        JPH::Vec3 min = joint->GetRotationLimitsMin();
        JPH::Vec3 max = joint->GetRotationLimitsMax();
        min.SetComponent(component, new_min);
        max.SetComponent(component, new_max);
        joint->SetRotationLimits(min, max);
    }
    return true;
}

bool PhysicsScene::GetJointAxisMotion(uint32_t joint_id,
                                     ConfigurableJointAxis axis,
                                     ConfigurableJointMotion& out_motion,
                                     float& out_limit_min,
                                     float& out_limit_max) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }
    JPH::SixDOFConstraintSettings::EAxis jolt_axis;
    if (!JoltAxisFor(axis, jolt_axis))
    {
        return false;
    }

    const JPH::SixDOFConstraint* joint = static_cast<const JPH::SixDOFConstraint*>(jph_constraint);
    out_limit_min = joint->GetLimitsMin(jolt_axis);
    out_limit_max = joint->GetLimitsMax(jolt_axis);
    // ⚠️ 先判 Free / Fixed 再看数值：`Locked` 在 Jolt 里存的是**反序**区间
    // （min = FLT_MAX, max = -FLT_MAX），直接按 min/max 判会把它读成 Limited。
    if (joint->IsFreeAxis(jolt_axis))
    {
        out_motion = ConfigurableJointMotion::Free;
    }
    else if (joint->IsFixedAxis(jolt_axis))
    {
        out_motion = ConfigurableJointMotion::Locked;
    }
    else
    {
        out_motion = ConfigurableJointMotion::Limited;
    }
    return true;
}

bool PhysicsScene::SetJointAxisDrive(uint32_t joint_id, ConfigurableJointAxis axis, const JointDrive& drive)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }
    return ApplyDriveToAxis(static_cast<JPH::SixDOFConstraint*>(jph_constraint), axis, drive);
}

bool PhysicsScene::GetJointAxisDrive(uint32_t joint_id, ConfigurableJointAxis axis, JointDrive& out_drive) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }
    JPH::SixDOFConstraintSettings::EAxis jolt_axis;
    if (!JoltAxisFor(axis, jolt_axis))
    {
        return false;
    }

    const JPH::SixDOFConstraint* joint = static_cast<const JPH::SixDOFConstraint*>(jph_constraint);
    const JPH::MotorSettings& motor = joint->GetMotorSettings(jolt_axis);
    out_drive.positionSpring = motor.mSpringSettings.mStiffness;
    out_drive.positionDamper = motor.mSpringSettings.mDamping;
    out_drive.maximumForce = AxisIsTranslation(axis) ? motor.mMaxForceLimit : motor.mMaxTorqueLimit;
    // 电机关着时刚度读回来就是 0（写入时保证了这一点），所以不需要额外报一个
    // "enabled" —— Unity 那边也是靠 `positionSpring` 判的。
    return true;
}

bool PhysicsScene::SetJointTarget(uint32_t joint_id,
                                  const Vector3& target_position,
                                  const Vector3& target_velocity,
                                  const Vector3& target_angular_velocity,
                                  const Quaternion& target_rotation)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }

    JPH::SixDOFConstraint* joint = static_cast<JPH::SixDOFConstraint*>(jph_constraint);
    joint->SetTargetPositionCS(toVec3(target_position));
    joint->SetTargetVelocityCS(toVec3(target_velocity));
    joint->SetTargetAngularVelocityCS(toVec3(target_angular_velocity));
    joint->SetTargetOrientationBS(toQuat(target_rotation));
    return true;
}

bool PhysicsScene::GetJointTarget(uint32_t joint_id,
                                  Vector3& out_target_position,
                                  Vector3& out_target_velocity,
                                  Vector3& out_target_angular_velocity,
                                  Quaternion& out_target_rotation) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }

    const JPH::SixDOFConstraint* joint = static_cast<const JPH::SixDOFConstraint*>(jph_constraint);
    out_target_position = toVec3(joint->GetTargetPositionCS());
    out_target_velocity = toVec3(joint->GetTargetVelocityCS());
    out_target_angular_velocity = toVec3(joint->GetTargetAngularVelocityCS());
    // ⚠️ 写入走的是 `BS`（body space），读出来也要换回 body space：Jolt 只存
    // constraint space 的那一份（`GetTargetOrientationCS`）。少这一步的话
    // "设进去再读出来"会拿到另一个数，而两个都是合法的四元数 —— 看不出错。
    //
    // `SetTargetOrientationBS(q)` 存的是 `CS = B1⁻¹ * q * B2`，所以反解是
    // `q = B1 * CS * B2⁻¹`。`mConstraintToBody*` 是 protected，公开侧只有
    // `GetConstraintToBody*Matrix()`，从它取旋转即可。
    const JPH::Quat c_to_b1 = joint->GetConstraintToBody1Matrix().GetQuaternion();
    const JPH::Quat c_to_b2 = joint->GetConstraintToBody2Matrix().GetQuaternion();
    out_target_rotation = toQuat(c_to_b1 * joint->GetTargetOrientationCS() * c_to_b2.Conjugated());
    return true;
}

bool PhysicsScene::SetJointLinearLimitSpring(uint32_t joint_id, float spring, float damper)
{
    JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }
    if (spring < 0.f || damper < 0.f)
    {
        return false;
    }

    JPH::SixDOFConstraint* joint = static_cast<JPH::SixDOFConstraint*>(jph_constraint);
    const JPH::SpringSettings settings(JPH::ESpringMode::StiffnessAndDamping, spring, damper);
    for (int i = 0; i < JPH::SixDOFConstraintSettings::EAxis::NumTranslation; ++i)
    {
        joint->SetLimitsSpringSettings(static_cast<JPH::SixDOFConstraintSettings::EAxis>(i), settings);
    }
    return true;
}

bool PhysicsScene::GetJointLinearLimitSpring(uint32_t joint_id, float& out_spring, float& out_damper) const
{
    const JPH::Constraint* jph_constraint = JointAt(joint_id);
    if (jph_constraint == nullptr || jph_constraint->GetSubType() != JPH::EConstraintSubType::SixDOF)
    {
        return false;
    }

    const JPH::SixDOFConstraint* joint = static_cast<const JPH::SixDOFConstraint*>(jph_constraint);
    // 三个平移轴共用一个值（`SetJointLinearLimitSpring` 保证了它们一致），取
    // TranslationX 那一份。
    const JPH::SpringSettings& settings = joint->GetLimitsSpringSettings(JPH::SixDOFConstraintSettings::TranslationX);
    out_spring = settings.mStiffness;
    out_damper = settings.mDamping;
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
