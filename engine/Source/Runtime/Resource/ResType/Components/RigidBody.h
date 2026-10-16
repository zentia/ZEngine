#pragma once

#include "Runtime/BaseClasses/ImmediatePtr.h"
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

template<>
class SerializeTraits<RigidBodyShapeType> : public SerializeTraitsBase<RigidBodyShapeType>
{
public:
    using value_type = RigidBodyShapeType;

    inline static const char* GetTypeString(void* p = nullptr) { return "RigidBodyShapeType"; }
    inline static bool AllowTransferOptimization() { return true; }

    template<typename TransferFunction>
    inline static void Transfer(value_type& data, TransferFunction& transfer)
    {
        using UnderlyingType = std::underlying_type_t<value_type>;
        transfer.TransferBasicData(reinterpret_cast<UnderlyingType&>(data));
    }
};

class RigidBodyShape : public Object
{
    REGISTER_CLASS(RigidBodyShape)
    DECLARE_OBJECT_SERIALIZE()

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
    DECLARE_SERIALIZE(RigidBodyComponentRes)

    std::vector<ImmediatePtr<RigidBodyShape>> m_Shapes;
    float m_InverseMass;
    int m_ActorType;
};

template<typename TransferFunction>
void RigidBodyComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shapes, "shapes");
    transfer.Transfer(m_InverseMass, "inverse_mass");
    transfer.Transfer(m_ActorType, "actor_type");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(RigidBodyComponentRes, , Transfer, void)
