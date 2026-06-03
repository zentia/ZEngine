#include "Runtime/Function/Controller/CharacterController.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

void CharacterController::Initialize(const PPtr<Capsule> capsule)
{
    m_Capsule = (capsule);
    // m_RigidbodyShape->m_Geometry = (Geometry*)m_Capsule;

    m_RigidbodyShape->m_Type = RigidBodyShapeType::capsule;

    Quaternion orientation;
    orientation.FromAngleAxis(Radian(Degree(90.f)), Vector3::UNIT_X);

    m_RigidbodyShape->m_LocalTransform =
        Transform(Vector3(0, 0, capsule->m_HalfHeight + capsule->m_Radius), orientation, Vector3::UNIT_SCALE);
}

Vector3 CharacterController::move(const Vector3& current_position, const Vector3& displacement)
{
    std::shared_ptr<PhysicsScene> physics_scene = GET_SYSTEM(WorldManager)->GetCurrentActivePhysicsScene().lock();
    ASSERT(physics_scene);

    Vector3 final_position = current_position + displacement;

    Transform final_transform = Transform(final_position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);

    if (physics_scene->IsOverlap(*m_RigidbodyShape, final_transform.getMatrix()))
    {
        final_position = current_position;
    }

    return final_position;
}