#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

enum class RigidBodyShapeType : unsigned char
{
    box,
    sphere,
    capsule,
    invalid
};

class RigidBodyShape : public Object
{
    REGISTER_CLASS(RigidBodyShape)

public:
    TransformTRS m_GlobalTransform;
    AxisAlignedBox m_BoundingBox;
    RigidBodyShapeType m_Type {RigidBodyShapeType::invalid};

    TransformTRS m_LocalTransform;
    PPtr<Geometry> m_Geometry;
};

class RigidBodyComponentRes
{
public:
    std::vector<RigidBodyShape> m_Shapes;
    float m_InverseMass;
    int m_ActorType;
};