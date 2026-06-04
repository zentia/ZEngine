#include "Runtime/Function/Physics/PhysicsScene.h"

#include "Runtime/Core/Math/LocalTransform.h"

#include "Jolt/Core/Factory.h"
#include "Jolt/Core/JobSystem.h"
#include "Jolt/Core/JobSystemThreadPool.h"
#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
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
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "core/base/Macro.h"

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
}

PhysicsScene::~PhysicsScene()
{
    delete m_Physics.m_JoltPhysicsSystem;
    delete m_Physics.m_JoltJobSystem;
    delete m_Physics.m_TempAllocator;
    delete m_Physics.m_JoltBroadPhaseLayerInterface;

    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

uint32_t PhysicsScene::CreateRigidBody(const LocalTransform& global_transform,
                                       const RigidBodyComponentRes& rigidbody_actor_res)
{
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();

    struct JPHShapeData
    {
        JPH::Shape* shape {nullptr};
        LocalTransform local_transform;
        Vector3 global_position;
        Vector3 global_scale;
        Quaternion global_rotation;
    };

    std::vector<JPHShapeData> jph_shapes;

    for (size_t shape_index = 0; shape_index < rigidbody_actor_res.m_Shapes.size(); shape_index++)
    {
        const RigidBodyShape& shape = rigidbody_actor_res.m_Shapes[shape_index];

        const Matrix4x4 shape_global_transform = global_transform.getMatrix() * shape.m_LocalTransform.getMatrix();

        Vector3 global_position, global_scale;
        Quaternion global_rotation;

        shape_global_transform.Decomposition(global_position, global_scale, global_rotation);

        JPH::Shape* jph_shape = toShape(shape, global_scale);

        if (jph_shape)
        {
            jph_shapes.push_back({jph_shape, shape.m_LocalTransform, global_position, global_scale, global_rotation});
        }
    }

    if (jph_shapes.empty())
    {
        LOG_ERROR(ZPhysics, "Create JPH Shapes Failed");
        return JPH::BodyID::cInvalidBodyID;
    }

    //$TODO: currently just support static object
    JPH::EMotionType motion_type = JPH::EMotionType::Static;
    JPH::ObjectLayer layer = Layers::NON_MOVING;

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

    JPH::Body* jph_body = body_interface.CreateBody(JPH::BodyCreationSettings(compund_shape_setting,
                                                                              toVec3(global_transform.m_Position.ToVector3()),
                                                                              toQuat(global_transform.m_Rotation),
                                                                              motion_type,
                                                                              layer));

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

void PhysicsScene::UpdateRigidBodyGlobalTransform(uint32_t body_id, const LocalTransform& global_transform)
{
    JPH::BodyInterface& body_interface = m_Physics.m_JoltPhysicsSystem->GetBodyInterface();

    body_interface.SetPositionAndRotation(JPH::BodyID(body_id),
                                          toVec3(global_transform.m_Position.ToVector3()),
                                          toQuat(global_transform.m_Rotation),
                                          JPH::EActivation::Activate);
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
        body_interface.RemoveBody(JPH::BodyID(body_id));
        body_interface.DestroyBody(JPH::BodyID(body_id));
    }
    m_PendingRemoveBodies.clear();
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
