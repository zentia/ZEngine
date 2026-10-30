#include "Runtime/Function/Physics/Physics.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

namespace
{
    /// Unity's layerMask is a signed int where -1 is "everything" and 0 is
    /// "nothing". The engine side masks with a uint32_t, and the two spellings
    /// agree bit for bit, so this is a cast rather than a translation.
    uint32_t ToMask(int32_t layer_mask)
    {
        return static_cast<uint32_t>(layer_mask);
    }

    /// Unity treats maxDistance <= 0 as Mathf.Infinity. Jolt needs a finite
    /// number, so "infinite" becomes "farther than anything in a level".
    constexpr float kInfiniteDistance = 1.0e6f;

    float EffectiveDistance(float max_distance)
    {
        return (max_distance > 0.f && max_distance < kInfiniteDistance) ? max_distance : kInfiniteDistance;
    }

    Matrix4x4 WorldMatrix(const Vector3& position, const Quaternion& rotation)
    {
        return TransformTRS(position, rotation, Vector3::UNIT_SCALE).getMatrix();
    }

    /// Jolt's capsule is Y-up; a Unity CapsuleCast points any which way, so the
    /// descriptor carries a rotation that brings +Y onto the p1 -> p2 axis.
    Quaternion CapsuleOrientation(const Vector3& point1, const Vector3& point2)
    {
        const Vector3 axis = point2 - point1;
        if (axis.squaredLength() < 1.0e-8f)
        {
            return Quaternion::IDENTITY;
        }
        return Vector3::UNIT_Y.getRotationTo(axis.normalisedCopy(), Vector3::UNIT_X);
    }

    RaycastHit ToRaycastHit(const PhysicsHitInfo& in_hit)
    {
        RaycastHit out;
        out.valid = in_hit.body_id != kInvalidPhysicsBodyId;
        out.point = in_hit.hit_position;
        out.normal = in_hit.hit_normal;
        out.distance = in_hit.hit_distance;
        out.bodyId = in_hit.body_id;
        out.layer = in_hit.layer;
        return out;
    }

    std::vector<RaycastHit> ToRaycastHits(const std::vector<PhysicsHitInfo>& in_hits)
    {
        std::vector<RaycastHit> out;
        out.reserve(in_hits.size());
        for (const PhysicsHitInfo& hit : in_hits)
        {
            out.push_back(ToRaycastHit(hit));
        }
        return out;
    }

    /// First hit of a query, or a default-constructed (invalid) record.
    RaycastHit FirstHit(const std::vector<PhysicsHitInfo>& in_hits)
    {
        return in_hits.empty() ? RaycastHit {} : ToRaycastHit(in_hits.front());
    }
}  // namespace

std::shared_ptr<PhysicsScene> Physics::CurrentScene()
{
    // The level's scene is the right one at runtime; the physics manager's
    // first scene is the fallback that makes this reachable from a headless
    // test (and from the editor before a level is loaded).
    if (const WorldManager* world_manager = GET_SYSTEM(WorldManager))
    {
        if (std::shared_ptr<PhysicsScene> active = world_manager->GetCurrentActivePhysicsScene().lock())
        {
            return active;
        }
    }

    if (const PhysicsManager* physics_manager = GET_SYSTEM(PhysicsManager))
    {
        return physics_manager->GetPrimaryPhysicsScene();
    }

    return nullptr;
}

// --- ray -------------------------------------------------------------------

RaycastHit Physics::Raycast(Vector3 origin, Vector3 direction, float maxDistance, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return RaycastHit {};
    }

    std::vector<PhysicsHitInfo> hits;
    scene->Raycast(origin, direction, EffectiveDistance(maxDistance), hits, ToMask(layerMask));
    return FirstHit(hits);
}

std::vector<RaycastHit> Physics::RaycastAll(Vector3 origin, Vector3 direction, float maxDistance, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return {};
    }

    std::vector<PhysicsHitInfo> hits;
    scene->Raycast(origin, direction, EffectiveDistance(maxDistance), hits, ToMask(layerMask));
    return ToRaycastHits(hits);
}

bool Physics::Linecast(Vector3 start, Vector3 end, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return false;
    }

    const Vector3 delta = end - start;
    const float length = delta.length();
    if (length < 1.0e-5f)
    {
        return false;
    }

    std::vector<PhysicsHitInfo> hits;
    // A linecast stops at `end`, so anything strictly beyond the segment is not
    // a hit -- hence the exact length and not "infinite".
    scene->Raycast(start, delta, length, hits, ToMask(layerMask));
    return !hits.empty();
}

// --- sweeps ----------------------------------------------------------------

RaycastHit Physics::SphereCast(Vector3 origin, float radius, Vector3 direction, float maxDistance, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return RaycastHit {};
    }

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::sphere;
    desc.radius = radius;

    std::vector<PhysicsHitInfo> hits;
    scene->CastPrimitive(desc,
                         WorldMatrix(origin, Quaternion::IDENTITY),
                         direction,
                         EffectiveDistance(maxDistance),
                         hits,
                         ToMask(layerMask));
    return FirstHit(hits);
}

std::vector<RaycastHit> Physics::SphereCastAll(Vector3 origin,
                                               float radius,
                                               Vector3 direction,
                                               float maxDistance,
                                               int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return {};
    }

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::sphere;
    desc.radius = radius;

    std::vector<PhysicsHitInfo> hits;
    scene->CastPrimitive(desc,
                         WorldMatrix(origin, Quaternion::IDENTITY),
                         direction,
                         EffectiveDistance(maxDistance),
                         hits,
                         ToMask(layerMask));
    return ToRaycastHits(hits);
}

RaycastHit Physics::BoxCast(Vector3 center,
                            Vector3 halfExtents,
                            Vector3 direction,
                            float maxDistance,
                            int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return RaycastHit {};
    }

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::box;
    desc.halfExtents = halfExtents;

    std::vector<PhysicsHitInfo> hits;
    scene->CastPrimitive(desc,
                         WorldMatrix(center, Quaternion::IDENTITY),
                         direction,
                         EffectiveDistance(maxDistance),
                         hits,
                         ToMask(layerMask));
    return FirstHit(hits);
}

RaycastHit Physics::CapsuleCast(Vector3 point1,
                                Vector3 point2,
                                float radius,
                                Vector3 direction,
                                float maxDistance,
                                int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return RaycastHit {};
    }

    const Vector3 axis = point2 - point1;
    const float half_height = axis.length() * 0.5f;
    const Vector3 center = (point1 + point2) * 0.5f;

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::capsule;
    desc.radius = radius;
    desc.halfHeight = half_height;

    std::vector<PhysicsHitInfo> hits;
    scene->CastPrimitive(desc,
                         WorldMatrix(center, CapsuleOrientation(point1, point2)),
                         direction,
                         EffectiveDistance(maxDistance),
                         hits,
                         ToMask(layerMask));
    return FirstHit(hits);
}

// --- containment -----------------------------------------------------------

bool Physics::CheckSphere(Vector3 center, float radius, int32_t layerMask)
{
    return !OverlapSphere(center, radius, layerMask).empty();
}

bool Physics::CheckBox(Vector3 center, Vector3 halfExtents, int32_t layerMask)
{
    return !OverlapBox(center, halfExtents, layerMask).empty();
}

bool Physics::CheckCapsule(Vector3 point1, Vector3 point2, float radius, int32_t layerMask)
{
    return !OverlapCapsule(point1, point2, radius, layerMask).empty();
}

// --- overlaps --------------------------------------------------------------

std::vector<RaycastHit> Physics::OverlapSphere(Vector3 center, float radius, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return {};
    }

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::sphere;
    desc.radius = radius;

    std::vector<PhysicsHitInfo> hits;
    scene->OverlapPrimitive(desc, WorldMatrix(center, Quaternion::IDENTITY), hits, ToMask(layerMask));
    return ToRaycastHits(hits);
}

std::vector<RaycastHit> Physics::OverlapBox(Vector3 center, Vector3 halfExtents, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return {};
    }

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::box;
    desc.halfExtents = halfExtents;

    std::vector<PhysicsHitInfo> hits;
    scene->OverlapPrimitive(desc, WorldMatrix(center, Quaternion::IDENTITY), hits, ToMask(layerMask));
    return ToRaycastHits(hits);
}

std::vector<RaycastHit> Physics::OverlapCapsule(Vector3 point1, Vector3 point2, float radius, int32_t layerMask)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (!scene)
    {
        return {};
    }

    const Vector3 axis = point2 - point1;
    const float half_height = axis.length() * 0.5f;
    const Vector3 center = (point1 + point2) * 0.5f;

    PhysicsPrimitiveDesc desc;
    desc.kind = PhysicsPrimitiveDesc::Kind::capsule;
    desc.radius = radius;
    desc.halfHeight = half_height;

    std::vector<PhysicsHitInfo> hits;
    scene->OverlapPrimitive(desc, WorldMatrix(center, CapsuleOrientation(point1, point2)), hits, ToMask(layerMask));
    return ToRaycastHits(hits);
}

// --- layers ----------------------------------------------------------------

void Physics::IgnoreLayerCollision(int32_t layer1, int32_t layer2, bool ignore)
{
    TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
    if (tag_layer == nullptr)
    {
        return;
    }
    tag_layer->SetIgnoreLayerCollision(layer1, layer2, ignore);
}

bool Physics::GetIgnoreLayerCollision(int32_t layer1, int32_t layer2)
{
    const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
    if (tag_layer == nullptr)
    {
        return false;
    }
    return tag_layer->GetIgnoreLayerCollision(layer1, layer2);
}

int32_t Physics::GetCollisionLayerMask(int32_t layer)
{
    const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
    if (tag_layer == nullptr)
    {
        return 0;
    }
    return static_cast<int32_t>(tag_layer->GetCollisionMask(layer));
}

eastl::string Physics::LayerToName(int32_t layer)
{
    const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
    if (tag_layer == nullptr)
    {
        return eastl::string();
    }
    return tag_layer->LayerToName(layer);
}

int32_t Physics::NameToLayer(const eastl::string& name)
{
    const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
    if (tag_layer == nullptr)
    {
        return TagLayerManager::kInvalidLayer;
    }
    return tag_layer->NameToLayer(name);
}

// --- world -----------------------------------------------------------------

Vector3 Physics::GetGravity()
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    return scene ? scene->getGravity() : Vector3::ZERO;
}

void Physics::SetGravity(Vector3 gravity)
{
    std::shared_ptr<PhysicsScene> scene = CurrentScene();
    if (scene)
    {
        scene->setGravity(gravity);
    }
}
