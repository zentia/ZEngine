#pragma once

#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"
#include "RigidBody.generated.h"

enum class RigidBodyShapeType : unsigned char
{
    box,
    sphere,
    capsule,
    invalid
};

/// How a rigid body responds to the simulation step and to forces.
///
/// Scripts see this as `declare enum RigidBodyActorType { StaticBody = 0, ... }`
/// (see ZENUM). It mirrors Unity's Rigidbody body type:
///   * static_body -- never moves, the cheapest thing to collide against.
///   * kinematic   -- moved by the game (transform / velocities), pushes
///                    dynamic bodies, but is not pushed back by them.
///   * dynamic     -- simulated: gravity, forces, impulses and collisions all
///                    move it.
///
/// The *serialized* field stays a plain int (assets predate the enum), so the
/// two directions go through ToRigidBodyActorType / ToSerializedActorType.
ZENUM()
enum class RigidBodyActorType : unsigned char
{
    static_body,
    kinematic,
    dynamic
};

/// int -> enum. Anything outside the range (including the 0 an asset gets when
/// it was authored before the field meant anything) reads as a static body,
/// which is what every body used to be created as anyway.
inline RigidBodyActorType ToRigidBodyActorType(int actor_type)
{
    switch (actor_type)
    {
        case static_cast<int>(RigidBodyActorType::kinematic):
            return RigidBodyActorType::kinematic;
        case static_cast<int>(RigidBodyActorType::dynamic):
            return RigidBodyActorType::dynamic;
        default:
            return RigidBodyActorType::static_body;
    }
}

/// enum -> int, for writing the serialized field back.
inline int ToSerializedActorType(RigidBodyActorType actor_type)
{
    return static_cast<int>(actor_type);
}

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

ZCLASS()
class RigidBodyShape : public Object
{
    ZGENERATED_BODY(RigidBodyShape)

public:
    ZPROPERTY()
    TransformTRS m_GlobalTransform;
    AxisAlignedBox m_BoundingBox;
    ZPROPERTY()
    RigidBodyShapeType m_Type {RigidBodyShapeType::invalid};

    ZPROPERTY()
    TransformTRS m_LocalTransform;
    ZPROPERTY()
    PPtr<Geometry> m_Geometry;
};

class RigidBodyComponentRes
{
public:
    DECLARE_SERIALIZE(RigidBodyComponentRes)

    std::vector<ImmediatePtr<RigidBodyShape>> m_Shapes;
    // Both were left uninitialized (POD default), so a res built without going
    // through a deserializer read garbage. 0 == "static / infinite mass", which
    // is what every body used to be created as.
    float m_InverseMass {0.f};
    int m_ActorType {0};

    RigidBodyActorType GetActorType() const { return ToRigidBodyActorType(m_ActorType); }
    void SetActorType(RigidBodyActorType actor_type) { m_ActorType = ToSerializedActorType(actor_type); }
};

template<typename TransferFunction>
void RigidBodyComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shapes, "shapes");
    transfer.Transfer(m_InverseMass, "inverse_mass");
    transfer.Transfer(m_ActorType, "actor_type");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(RigidBodyComponentRes, , Transfer, void)
