#include "Runtime/Function/Physics/Jolt/Utils.h"

#include <algorithm>

#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"
#include "Runtime/Function/Physics/PhysicsQuery.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"
#include "Runtime/BaseClasses/Cast.h"

BPLayerInterfaceImpl::BPLayerInterfaceImpl()
{
    // Every Unity layer lands in the one broadphase bucket. The pair filter
    // below is where the collision matrix is consulted.
    for (uint8_t layer = 0; layer < Layers::NUM_LAYERS; ++layer)
    {
        m_ObjectToBroadPhase[layer] = BroadPhaseLayers::ALL;
    }
}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
const char* BPLayerInterfaceImpl::GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const
{
    switch ((JPH::BroadPhaseLayer::Type)inLayer)
    {
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::ALL:
            return "ALL";
        default:
            ASSERT(false);
            return "INVALID";
    }
}
#endif  // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

bool ObjectLayerPairFilterImpl::ShouldCollide([[maybe_unused]] JPH::ObjectLayer inLayer1,
                                              [[maybe_unused]] JPH::ObjectLayer inLayer2) const
{
    // Unity's layer collision matrix. This is the ONLY place the matrix gates
    // anything: before this, `Physics.IgnoreLayerCollision` wrote bits that no
    // simulation code ever read (implemented but not wired).
    const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
    if (tag_layer == nullptr)
    {
        // No manager (headless test, or teardown ordering). Colliding is the
        // pre-matrix behaviour, so it is the safe fallback: refusing to collide
        // here would make every body fall through the world.
        return true;
    }

    return !tag_layer->GetIgnoreLayerCollision(static_cast<int32_t>(inLayer1), static_cast<int32_t>(inLayer2));
}

bool ObjectVsBroadPhaseLayerFilterImpl::ShouldCollide([[maybe_unused]] JPH::ObjectLayer inLayer1,
                                                      [[maybe_unused]] JPH::BroadPhaseLayer inLayer2) const
{
    // Broadphase is a coarse bucket now, so there is nothing to reject here:
    // gating on this level would silently drop pairs the pair filter would have
    // allowed (Jolt tests both filters and requires BOTH to pass).
    return inLayer1 < Layers::NUM_LAYERS && inLayer2 == BroadPhaseLayers::ALL;
}

JPH::Mat44 toMat44(const Matrix4x4& m)
{
    JPH::Vec4 cols[4];
    for (int i = 0; i < 4; i++)
    {
        cols[i] = JPH::Vec4(m.m_Mat[0][i], m.m_Mat[1][i], m.m_Mat[2][i], m.m_Mat[3][i]);
    }

    return {cols[0], cols[1], cols[2], cols[3]};
}

Matrix4x4 toMat44(const JPH::Mat44& m)
{
    Vector4 cols[4];
    for (int i = 0; i < 4; i++)
    {
        cols[i] = toVec4(m.GetColumn4(i));
    }

    return Matrix4x4(cols[0], cols[1], cols[2], cols[3]).transpose();
}

JPH::Shape* toShape(const RigidBodyShape& shape, const Vector3& scale)
{
    JPH::Shape* jph_shape = nullptr;

    // PPtr<T>::GetTypeString() is STATIC -- it expands to T::GetPPtrTypeString(),
    // which returns "PPtr<Geometry>" for a PPtr<Geometry> no matter what the
    // pointed-to object actually is. Comparing it against "Box"/"Sphere"/"Capsule"
    // therefore never matched and this returned null for every shape, so no
    // collider was ever created. Dispatch on the runtime type instead.
    const Geometry* geometry = static_cast<const Geometry*>(shape.m_Geometry);
    if (geometry == nullptr)
    {
        LOG_ERROR(ZPhysics, "Unsupported Shape: geometry reference did not resolve");
        return nullptr;
    }

    if (const Box* box_geometry = Cast<Box>(geometry))
    {
        JPH::Vec3 jph_box(scale.x * box_geometry->m_HalfExtents.x,
                          scale.y * box_geometry->m_HalfExtents.y,
                          scale.z * box_geometry->m_HalfExtents.z);
        jph_shape = new JPH::BoxShape(jph_box, 0.f);
    }
    else if (const Sphere* sphere_geometry = Cast<Sphere>(geometry))
    {
        jph_shape = new JPH::SphereShape((scale.x + scale.y + scale.z) / 3 * sphere_geometry->m_Radius);
    }
    else if (const Capsule* capsule_geometry = Cast<Capsule>(geometry))
    {
        jph_shape = new JPH::CapsuleShape(scale.z * capsule_geometry->m_HalfHeight,
                                          (scale.x + scale.y) / 2 * capsule_geometry->m_Radius);
    }
    else
    {
        LOG_ERROR(ZPhysics, "Unsupported Shape: {}", geometry->GetTypeName());
    }

    return jph_shape;
}

JPH::Shape* MakePrimitiveShape(const PhysicsPrimitiveDesc& desc)
{
    switch (desc.kind)
    {
        case PhysicsPrimitiveDesc::Kind::box:
        {
            // Jolt's BoxShape takes half extents and requires all three > 0;
            // a flattened axis is a degenerate convex hull and Jolt asserts on
            // it in debug. Clamp to a hair above zero rather than crashing a
            // script that asked for a thin box.
            const float kx = std::max(desc.halfExtents.x, 1.0e-4f);
            const float ky = std::max(desc.halfExtents.y, 1.0e-4f);
            const float kz = std::max(desc.halfExtents.z, 1.0e-4f);
            return new JPH::BoxShape(JPH::Vec3(kx, ky, kz), 0.f);
        }
        case PhysicsPrimitiveDesc::Kind::sphere:
        {
            const float radius = std::max(desc.radius, 1.0e-4f);
            return new JPH::SphereShape(radius);
        }
        case PhysicsPrimitiveDesc::Kind::capsule:
        {
            // CapsuleShape(halfHeightOfCylinder, radius): the total height is
            // 2 * (halfHeight + radius), so a zero half-height is still a valid
            // sphere. Both have to be positive for the convex support function.
            const float radius = std::max(desc.radius, 1.0e-4f);
            const float half_height = std::max(desc.halfHeight, 1.0e-4f);
            return new JPH::CapsuleShape(half_height, radius);
        }
        default:
            break;
    }

    LOG_ERROR(ZPhysics, "Unsupported primitive shape kind: {}", static_cast<int>(desc.kind));
    return nullptr;
}