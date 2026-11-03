#include "FixedJointComponent.h"

#include "Runtime/Function/Physics/PhysicsScene.h"

PhysicsJointType FixedJointComponent::GetJointType() const
{
    return PhysicsJointType::fixed;
}
