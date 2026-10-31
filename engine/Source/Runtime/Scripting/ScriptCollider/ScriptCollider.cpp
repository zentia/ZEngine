#include "Runtime/Scripting/ScriptCollider/ScriptCollider.h"

#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

#include <EASTL/unordered_map.h>

#include <algorithm>
#include <cmath>
#include <memory>

// ---------------------------------------------------------------------------
// Collider 实例的缓存（与 ScriptCamera / ScriptScene 同一套路）
//
// 按 GameObject* 建索引。同一对象永远拿到同一个 Collider，所以脚本可以放心
// 缓存 `const col = Collider.FromGameObject(go)`。
// ---------------------------------------------------------------------------
namespace
{
    eastl::unordered_map<GameObject*, std::unique_ptr<Collider>>& ColliderCache()
    {
        static eastl::unordered_map<GameObject*, std::unique_ptr<Collider>> cache;
        return cache;
    }

    /// 没有碰撞体的对象也拿到一个（IsValid() == false 的）Collider，而不是 nullptr。
    /// 理由见头文件 `FromGameObject` 的注释：区分"传了空对象"和"这个对象没有碰撞体"。
    Collider* ColliderFor(GameObject* owner)
    {
        if (owner == nullptr)
        {
            return nullptr;
        }
        auto& cache = ColliderCache();
        const auto it = cache.find(owner);
        if (it != cache.end())
        {
            return it->second.get();
        }
        auto created = std::make_unique<Collider>();
        created->SetOwner(owner);
        Collider* raw = created.get();
        cache.emplace(owner, std::move(created));
        return raw;
    }

    /// 当前活动物理场景。没有 WorldManager / 没有活动关卡时返回空 —— 与
    /// `RigidBodyComponent` 那批方法同一套降级契约。
    std::shared_ptr<PhysicsScene> CurrentScene()
    {
        WorldManager* world_manager = GET_SYSTEM(WorldManager);
        if (world_manager == nullptr)
        {
            return nullptr;
        }
        return world_manager->GetCurrentActivePhysicsScene().lock();
    }

    /// 一个形状在世界里占的"半径向量"：按轴给的最大伸展。
    ///
    /// 盒体：半尺寸（旋转后保守起见取各轴绝对值之和的行和 —— 下面按旋转后的
    ///       角点算，精确）。
    /// 球：  各轴 radius。
    /// 胶囊：轴向 radius+halfHeight，另两轴 radius。
    Vector3 ShapeExtent(const RigidBodyShape& shape)
    {
        const Geometry* geometry = static_cast<const Geometry*>(shape.m_Geometry);
        if (geometry == nullptr)
        {
            return Vector3::ZERO;
        }
        if (const Box* box = Cast<Box>(geometry))
        {
            return box->m_HalfExtents;
        }
        if (const Sphere* sphere = Cast<Sphere>(geometry))
        {
            return Vector3(sphere->m_Radius, sphere->m_Radius, sphere->m_Radius);
        }
        if (const Capsule* capsule = Cast<Capsule>(geometry))
        {
            // 引擎的胶囊沿 Z 立着（Z-up）。轴向（Z）是半高 + 半径，另两轴是半径。
            const float radial = capsule->m_Radius;
            const float axial = capsule->m_HalfHeight + capsule->m_Radius;
            return Vector3(radial, radial, axial);
        }
        return Vector3::ZERO;
    }
}  // namespace

Collider* Collider::FromGameObject(GameObject* owner)
{
    return ColliderFor(owner);
}

Collider* Collider::FromBodyId(uint32_t body_id)
{
    if (body_id == kInvalidPhysicsBodyId)
    {
        return nullptr;
    }
    PhysicsManager* manager = GET_SYSTEM(PhysicsManager);
    if (manager == nullptr)
    {
        return nullptr;
    }
    return ColliderFor(manager->FindBodyOwner(body_id));
}

RigidBodyComponent* Collider::ResolveRigidBody() const
{
    if (m_Owner == nullptr)
    {
        return nullptr;
    }
    // 按类型名查，不用 Cast 遍历：`GetComponentByTypeName` 还认派生类，将来自定
    // 义刚体组件也能命中。
    return Cast<RigidBodyComponent>(m_Owner->GetComponentByTypeName("RigidBodyComponent"));
}

bool Collider::IsValid() const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    return rigidbody != nullptr && !rigidbody->GetRigidbodyRes().m_Shapes.empty();
}

RigidBodyComponent* Collider::GetAttachedRigidbody() const
{
    return ResolveRigidBody();
}

int32_t Collider::GetShapeCount() const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr)
    {
        return 0;
    }
    return static_cast<int32_t>(rigidbody->GetRigidbodyRes().m_Shapes.size());
}

int32_t Collider::GetShapeType(int32_t index) const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr)
    {
        return static_cast<int32_t>(ColliderShapeType::invalid);
    }
    const auto& shapes = rigidbody->GetRigidbodyRes().m_Shapes;
    if (index < 0 || static_cast<size_t>(index) >= shapes.size())
    {
        return static_cast<int32_t>(ColliderShapeType::invalid);
    }
    switch (shapes[static_cast<size_t>(index)]->m_Type)
    {
        case RigidBodyShapeType::box:
            return static_cast<int32_t>(ColliderShapeType::box);
        case RigidBodyShapeType::sphere:
            return static_cast<int32_t>(ColliderShapeType::sphere);
        case RigidBodyShapeType::capsule:
            return static_cast<int32_t>(ColliderShapeType::capsule);
        default:
            return static_cast<int32_t>(ColliderShapeType::invalid);
    }
}

Vector3 Collider::GetShapeCenter(int32_t index) const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr)
    {
        return Vector3::ZERO;
    }
    const auto& shapes = rigidbody->GetRigidbodyRes().m_Shapes;
    if (index < 0 || static_cast<size_t>(index) >= shapes.size())
    {
        return Vector3::ZERO;
    }
    return shapes[static_cast<size_t>(index)]->m_LocalTransform.m_Position.ToVector3();
}

Vector3 Collider::GetShapeSize(int32_t index) const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr)
    {
        return Vector3::ZERO;
    }
    const auto& shapes = rigidbody->GetRigidbodyRes().m_Shapes;
    if (index < 0 || static_cast<size_t>(index) >= shapes.size())
    {
        return Vector3::ZERO;
    }
    const Geometry* geometry = static_cast<const Geometry*>(shapes[static_cast<size_t>(index)]->m_Geometry);
    if (const Box* box = Cast<Box>(geometry))
    {
        // Unity 的 `BoxCollider.size` 是全尺寸；资源里存的是半尺寸。
        return box->m_HalfExtents * 2.0f;
    }
    return Vector3::ZERO;
}

float Collider::GetShapeRadius(int32_t index) const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr)
    {
        return 0.f;
    }
    const auto& shapes = rigidbody->GetRigidbodyRes().m_Shapes;
    if (index < 0 || static_cast<size_t>(index) >= shapes.size())
    {
        return 0.f;
    }
    const Geometry* geometry = static_cast<const Geometry*>(shapes[static_cast<size_t>(index)]->m_Geometry);
    if (const Sphere* sphere = Cast<Sphere>(geometry))
    {
        return sphere->m_Radius;
    }
    if (const Capsule* capsule = Cast<Capsule>(geometry))
    {
        return capsule->m_Radius;
    }
    return 0.f;
}

float Collider::GetShapeHeight(int32_t index) const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr)
    {
        return 0.f;
    }
    const auto& shapes = rigidbody->GetRigidbodyRes().m_Shapes;
    if (index < 0 || static_cast<size_t>(index) >= shapes.size())
    {
        return 0.f;
    }
    const Geometry* geometry = static_cast<const Geometry*>(shapes[static_cast<size_t>(index)]->m_Geometry);
    if (const Capsule* capsule = Cast<Capsule>(geometry))
    {
        // 同 CharacterController.height：含两端半球帽。
        return 2.0f * (capsule->m_HalfHeight + capsule->m_Radius);
    }
    return 0.f;
}

bool Collider::GetIsTrigger() const
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr || !rigidbody->HasBody())
    {
        return false;
    }
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return false;
    }
    bool is_sensor = false;
    return scene->IsRigidBodySensor(rigidbody->GetBodyId(), is_sensor) && is_sensor;
}

void Collider::SetIsTrigger(bool is_trigger)
{
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (rigidbody == nullptr || !rigidbody->HasBody())
    {
        return;
    }
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return;
    }
    scene->SetRigidBodySensor(rigidbody->GetBodyId(), is_trigger);
}

AxisAlignedBox Collider::ComputeWorldBounds() const
{
    AxisAlignedBox bounds;
    RigidBodyComponent* rigidbody = ResolveRigidBody();
    if (m_Owner == nullptr || rigidbody == nullptr)
    {
        return bounds;
    }

    const Transform* transform = m_Owner->tryGetComponentConst(Transform);
    if (transform == nullptr)
    {
        return bounds;
    }

    const Vector3 object_position = transform->GetPosition();
    const Quaternion object_rotation = transform->GetRotation();

    for (const auto& shape : rigidbody->GetRigidbodyRes().m_Shapes)
    {
        if (shape == nullptr)
        {
            continue;
        }
        // 形状中心：局部偏移经对象旋转后 + 对象位置。
        const Vector3 local_center = shape->m_LocalTransform.m_Position.ToVector3();
        const Vector3 world_center = object_position + object_rotation * local_center;

        // 形状朝向也要算进来：一个转了 45° 的盒子，它的 AABB 比自身大。
        const Quaternion local_rotation = shape->m_LocalTransform.m_Rotation;
        const Quaternion world_rotation = object_rotation * local_rotation;

        const Vector3 extent = ShapeExtent(*shape);
        // 旋转后各轴的行和 = 该轴上的最大伸展（|m| 逐元素取绝对值，右乘 1 向量）。
        const Vector3 rx = world_rotation * Vector3(extent.x, 0.f, 0.f);
        const Vector3 ry = world_rotation * Vector3(0.f, extent.y, 0.f);
        const Vector3 rz = world_rotation * Vector3(0.f, 0.f, extent.z);
        const Vector3 reach(std::fabs(rx.x) + std::fabs(ry.x) + std::fabs(rz.x),
                            std::fabs(rx.y) + std::fabs(ry.y) + std::fabs(rz.y),
                            std::fabs(rx.z) + std::fabs(ry.z) + std::fabs(rz.z));

        bounds.Merge(world_center + reach);
        bounds.Merge(world_center - reach);
    }

    return bounds;
}

Vector3 Collider::GetBoundsCenter() const
{
    return ComputeWorldBounds().getCenter();
}

Vector3 Collider::GetBoundsSize() const
{
    return ComputeWorldBounds().getHalfExtent() * 2.0f;
}

bool Collider::Contains(const Vector3& point) const
{
    const AxisAlignedBox bounds = ComputeWorldBounds();
    return bounds.IsValid() && bounds.Contains(point);
}

Vector3 Collider::ClosestPoint(const Vector3& point) const
{
    const AxisAlignedBox bounds = ComputeWorldBounds();
    if (!bounds.IsValid())
    {
        // 没有碰撞体时"最近点"就是点自己：返回零向量会被当成"包围盒在原点"，
        // 而那是一个完全错误的答案。
        return point;
    }
    const Vector3 min_corner = bounds.getMinCorner();
    const Vector3 max_corner = bounds.getMaxCorner();
    return Vector3(std::clamp(point.x, min_corner.x, max_corner.x),
                   std::clamp(point.y, min_corner.y, max_corner.y),
                   std::clamp(point.z, min_corner.z, max_corner.z));
}
