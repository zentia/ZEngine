#include "Runtime/Function/Physics/Jolt/Utils.h"

#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/StaticCompoundShape.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"

BPLayerInterfaceImpl::BPLayerInterfaceImpl()
{
    // Create a mapping table from object to broad phase layer
    m_ObjectToBroadPhase[Layers::UNUSED1] = BroadPhaseLayers::UNUSED;
    m_ObjectToBroadPhase[Layers::UNUSED2] = BroadPhaseLayers::UNUSED;
    m_ObjectToBroadPhase[Layers::UNUSED3] = BroadPhaseLayers::UNUSED;
    m_ObjectToBroadPhase[Layers::UNUSED4] = BroadPhaseLayers::UNUSED;
    m_ObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
    m_ObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
    m_ObjectToBroadPhase[Layers::DEBRIS] = BroadPhaseLayers::DEBRIS;
    m_ObjectToBroadPhase[Layers::SENSOR] = BroadPhaseLayers::SENSOR;
}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
const char* BPLayerInterfaceImpl::GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const
{
    switch ((JPH::BroadPhaseLayer::Type)inLayer)
    {
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
            return "NON_MOVING";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
            return "MOVING";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::DEBRIS:
            return "DEBRIS";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::SENSOR:
            return "SENSOR";
        case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::UNUSED:
            return "UNUSED";
        default:
            ASSERT(false);
            return "INVALID";
    }
}
#endif  // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

bool ObjectLayerPairFilterImpl::ShouldCollide([[maybe_unused]] JPH::ObjectLayer inLayer1,
                                              [[maybe_unused]] JPH::ObjectLayer inLayer2) const
{
    switch (inLayer1)
    {
        case Layers::UNUSED1:
        case Layers::UNUSED2:
        case Layers::UNUSED3:
        case Layers::UNUSED4:
            return false;
        case Layers::NON_MOVING:
            return inLayer2 == Layers::MOVING || inLayer2 == Layers::DEBRIS;
        case Layers::MOVING:
            return inLayer2 == Layers::NON_MOVING || inLayer2 == Layers::MOVING || inLayer2 == Layers::SENSOR;
        case Layers::DEBRIS:
            return inLayer2 == Layers::NON_MOVING;
        case Layers::SENSOR:
            return inLayer2 == Layers::MOVING;
        default:
            ASSERT(false);
            return false;
    }
}

bool ObjectVsBroadPhaseLayerFilterImpl::ShouldCollide([[maybe_unused]] JPH::ObjectLayer inLayer1,
                                                      [[maybe_unused]] JPH::BroadPhaseLayer inLayer2) const
{
    switch (inLayer1)
    {
        case Layers::NON_MOVING:
            return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:
            return inLayer2 == BroadPhaseLayers::NON_MOVING || inLayer2 == BroadPhaseLayers::MOVING ||
                   inLayer2 == BroadPhaseLayers::SENSOR;
        case Layers::DEBRIS:
            return inLayer2 == BroadPhaseLayers::NON_MOVING;
        case Layers::SENSOR:
            return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::UNUSED1:
        case Layers::UNUSED2:
        case Layers::UNUSED3:
            return false;
        default:
            ASSERT(false);
            return false;
    }
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

    const std::string shape_type_str = shape.m_Geometry.GetTypeString();
    if (shape_type_str == "Box")
    {
        const Box* box_geometry = static_cast<const Box*>((Geometry*)shape.m_Geometry);
        if (box_geometry)
        {
            JPH::Vec3 jph_box(scale.x * box_geometry->m_HalfExtents.x,
                              scale.y * box_geometry->m_HalfExtents.y,
                              scale.z * box_geometry->m_HalfExtents.z);
            jph_shape = new JPH::BoxShape(jph_box, 0.f);
        }
    }
    else if (shape_type_str == "Sphere")
    {
        const Sphere* sphere_geometry = static_cast<const Sphere*>((Geometry*)shape.m_Geometry);
        if (sphere_geometry)
        {
            jph_shape = new JPH::SphereShape((scale.x + scale.y + scale.z) / 3 * sphere_geometry->m_Radius);
        }
    }
    else if (shape_type_str == "Capsule")
    {
        const Capsule* capsule_geometry = static_cast<const Capsule*>((Geometry*)shape.m_Geometry);
        if (capsule_geometry)
        {
            jph_shape = new JPH::CapsuleShape(scale.z * capsule_geometry->m_HalfHeight,
                                              (scale.x + scale.y) / 2 * capsule_geometry->m_Radius);
        }
    }
    else
    {
        LOG_ERROR(ZPhysics, "Unsupported Shape");
    }

    return jph_shape;
}