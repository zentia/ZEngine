#include "Runtime/Function/Controller/CharacterController.h"

#include "Runtime/Core/Math/TransformTRS.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

void CharacterController::Initialize(PPtr<Capsule> capsule)
{
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

    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    Vector3 final_position = current_position + displacement;

    TransformTRS final_transform = TransformTRS(final_position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);

    if (physics_scene->IsOverlap(*m_RigidbodyShape, final_transform.getMatrix()))
    {
        final_position = current_position;
    }

    return final_position;
}