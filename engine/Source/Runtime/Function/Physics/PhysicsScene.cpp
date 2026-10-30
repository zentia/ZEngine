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
            // Ordinary collision: not a trigger, nothing to report.
            return;
        }

        const uint32_t trigger_id =
            (body1_is_sensor ? in_body1 : in_body2).GetID().GetIndexAndSequenceNumber();
        const uint32_t other_id = (body1_is_sensor ? in_body2 : in_body1).GetID().GetIndexAndSequenceNumber();

        Add(trigger_id, other_id);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& in_sub_shape_pair) override
    {
        Remove(in_sub_shape_pair.GetBody1ID().GetIndexAndSequenceNumber(),
               in_sub_shape_pair.GetBody2ID().GetIndexAndSequenceNumber());
    }

    /// Hand the queued events to the caller and start a fresh queue.
    void Poll(std::vector<PhysicsTriggerEvent>& out_events)
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        out_events.swap(m_Events);
        m_Events.clear();
    }

    /// A body is going away: close out every trigger pair it took part in so
    /// gameplay does not keep believing something is still inside a trigger.
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
                m_Events.push_back(exit_event);
                it = m_Active.erase(it);
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
        m_Events.push_back(exit_event);
        m_Active.erase(found);
    }

    std::mutex m_Mutex;
    std::unordered_map<uint64_t, PhysicsTriggerEvent> m_Active;
    std::vector<PhysicsTriggerEvent> m_Events;
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

uint32_t PhysicsScene::CreateRigidBody(const TransformTRS& global_transform,
                                       const RigidBodyComponentRes& rigidbody_actor_res)
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
    JPH::ObjectLayer layer = Layers::NON_MOVING;
    switch (actor_type)
    {
        case RigidBodyActorType::kinematic:
            motion_type = JPH::EMotionType::Kinematic;
            layer = Layers::MOVING;
            break;
        case RigidBodyActorType::dynamic:
            motion_type = JPH::EMotionType::Dynamic;
            layer = Layers::MOVING;
            break;
        default:
            break;
    }

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
        // Only dynamic bodies integrate forces. Silently ignoring it keeps a
        // script from crashing the frame because a level author left a body
        // static.
        return false;
    }

    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();
    body_interface.ActivateBody(JPH::BodyID(body_id));

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

bool PhysicsScene::Raycast(Vector3 ray_origin,
                           Vector3 ray_directory,
                           float ray_length,
                           std::vector<PhysicsHitInfo>& out_hits)
{
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

    out_hits.clear();
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

    return true;
}

bool PhysicsScene::Sweep(const RigidBodyShape& shape,
                         const Matrix4x4& shape_transform,
                         Vector3 sweep_direction,
                         float sweep_length,
                         std::vector<PhysicsHitInfo>& out_hits)
{
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

    out_hits.clear();
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

    return true;
}

bool PhysicsScene::IsOverlap(const RigidBodyShape& shape, const Matrix4x4& global_transform)
{
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

    return collector.HadHit();
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
