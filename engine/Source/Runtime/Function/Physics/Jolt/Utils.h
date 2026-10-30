#pragma once

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h"
#include "core/Math/Matrix4.h"
#include "core/Math/Quaternion.h"
#include "core/Math/Vector3.h"
#include "core/base/Macro.h"

namespace JPH
{
    class Shape;
}

class RigidBodyShape;
struct PhysicsPrimitiveDesc;

// ---------------------------------------------------------------------------
// The Jolt object layer IS the Unity layer.
//
// It used to be a 8-value enum (NON_MOVING / MOVING / DEBRIS / SENSOR) that
// had nothing to do with what a level author picks in the Inspector, which is
// why `Physics.IgnoreLayerCollision` and `layerMask` had nowhere to hook into:
// there was no layer to test. Now CreateRigidBody writes the GameObject's
// Unity layer (0..31) straight into the object layer, so
//   * ObjectLayerPairFilterImpl resolves collisions through
//     TagLayerManager's collision matrix, and
//   * every query masks hits with PhysicsLayerMaskContains.
//
// Motion type (static / kinematic / dynamic) is a separate Jolt concept and is
// unaffected: it still comes from RigidBodyActorType.
// ---------------------------------------------------------------------------
namespace Layers
{
    static constexpr uint8_t NUM_LAYERS = 32;  // == TagLayerManager::kLayerCount
};  // namespace Layers

// A single broadphase layer. Splitting by layer would need 32 broadphase
// layers, and the only thing the broadphase filter can usefully reject is
// "static vs static" -- which Jolt already refuses on motion type. The pair
// filter below does the real gating, and it runs per pair, so one coarse
// bucket costs nothing in correctness.
namespace BroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer ALL(0);
    static constexpr uint32_t NUM_LAYERS(1);
};  // namespace BroadPhaseLayers

/// BroadPhaseLayerInterface implementation
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl();

    uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        ASSERT(inLayer < Layers::NUM_LAYERS);
        return m_ObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override;
#endif  // JPH_EXTERNAL_PROFILE || JPH_PROFILE_ENABLED

private:
    JPH::BroadPhaseLayer m_ObjectToBroadPhase[Layers::NUM_LAYERS];
};

/// Function that determines if two object layers can collide
bool ObjectCanCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2);

/// Function that determines if two broadphase layers can collide
bool BroadPhaseCanCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2);

inline JPH::Vec3 toVec3(Vector3 v)
{
    return {v.x, v.y, v.z};
}
inline Vector3 toVec3(JPH::Vec3 v)
{
    return {v.GetX(), v.GetY(), v.GetZ()};
}

inline JPH::Vec4 toVec4(Vector4 v)
{
    return {v.x, v.y, v.z, v.w};
}
inline Vector4 toVec4(JPH::Vec4 v)
{
    return {v.GetX(), v.GetY(), v.GetZ(), v.GetW()};
}

inline JPH::Quat toQuat(Quaternion q)
{
    return {q.x, q.y, q.z, q.w};
}
inline Quaternion toQuat(JPH::Quat q)
{
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
}

JPH::Mat44 toMat44(const Matrix4x4& m);

Matrix4x4 toMat44(const JPH::Mat44& m);

JPH::Shape* toShape(const RigidBodyShape& shape, const Vector3& scale);

/// Build a box / sphere / capsule straight from a descriptor, with no engine
/// Object involved (see PhysicsPrimitiveDesc in PhysicsQuery.h).
///
/// The caller owns the result and must `delete` it: Jolt's RefTarget starts at
/// refcount 0, so nothing has taken a reference.
JPH::Shape* MakePrimitiveShape(const PhysicsPrimitiveDesc& desc);

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    virtual bool ShouldCollide([[maybe_unused]] JPH::ObjectLayer inLayer1,
                               [[maybe_unused]] JPH::BroadPhaseLayer inLayer2) const override;
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
{
public:
    virtual bool ShouldCollide([[maybe_unused]] JPH::ObjectLayer inLayer1,
                               [[maybe_unused]] JPH::ObjectLayer inLayer2) const override;
};