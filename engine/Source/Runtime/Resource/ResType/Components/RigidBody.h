#pragma once

#include "Runtime/BaseClasses/ImmediatePtr.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/TransformTRS.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

#include <cstdint>

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

/// How two touching surfaces' friction / bounciness are folded into one number.
///
/// The declaration order **is** the priority order Unity documents
/// (Average < Minimum < Multiply < Maximum): when the two faces disagree, the
/// higher-priority mode wins outright, so the effective mode is just the max of
/// the two. Reordering these enumerators silently changes every contact in the
/// game -- do not sort them.
ZENUM()
enum class PhysicMaterialCombine : unsigned char
{
    Average,
    Minimum,
    Multiply,
    Maximum
};

/// int -> enum. Out-of-range (including the 0 an asset gets when the field did
/// not exist yet) reads as Average, which is Unity's default for both combines
/// and therefore also what an old asset behaved like.
inline PhysicMaterialCombine ToPhysicMaterialCombine(int combine_mode)
{
    switch (combine_mode)
    {
        case static_cast<int>(PhysicMaterialCombine::Minimum):
            return PhysicMaterialCombine::Minimum;
        case static_cast<int>(PhysicMaterialCombine::Multiply):
            return PhysicMaterialCombine::Multiply;
        case static_cast<int>(PhysicMaterialCombine::Maximum):
            return PhysicMaterialCombine::Maximum;
        default:
            return PhysicMaterialCombine::Average;
    }
}

inline int ToSerializedCombineMode(PhysicMaterialCombine combine_mode)
{
    return static_cast<int>(combine_mode);
}

/// Folds two surface values into the single one the solver sees.
///
/// Deliberately a free function that touches no Jolt type: it is the whole of
/// the "which mode wins, then what does that mode do" rule, and it is the part
/// most likely to be got subtly wrong, so it has to be assertable from a plain
/// unit test without first standing up a physics world.
inline float CombineSurfaceValues(float value_a, PhysicMaterialCombine combine_a,
                                 float value_b, PhysicMaterialCombine combine_b)
{
    // Priority, not preference: Unity says Maximum beats Multiply beats Minimum
    // beats Average, and the enum is declared in exactly that order.
    const PhysicMaterialCombine effective = (combine_a > combine_b) ? combine_a : combine_b;

    switch (effective)
    {
        case PhysicMaterialCombine::Minimum:
            return (value_a < value_b) ? value_a : value_b;
        case PhysicMaterialCombine::Multiply:
            return value_a * value_b;
        case PhysicMaterialCombine::Maximum:
            return (value_a > value_b) ? value_a : value_b;
        case PhysicMaterialCombine::Average:
        default:
            return 0.5f * (value_a + value_b);
    }
}

/// The combine modes ride along on the Jolt body's user data so the combine
/// callback -- which is invoked per contact pair, inside the solver, with no
/// side table it could safely consult -- can resolve "what did this face ask
/// for" with a couple of shifts instead of a map lookup.
///
/// The friction / restitution *values* themselves are not packed: Jolt already
/// stores them on the Body, so duplicating them here would be a second source
/// of truth that drifts the first time anyone sets one of them.
inline uint64_t PackCombineModes(PhysicMaterialCombine friction_combine, PhysicMaterialCombine bounce_combine)
{
    return static_cast<uint64_t>(ToSerializedCombineMode(friction_combine)) |
           (static_cast<uint64_t>(ToSerializedCombineMode(bounce_combine)) << 8);
}

inline PhysicMaterialCombine FrictionCombineFromUserData(uint64_t user_data)
{
    return ToPhysicMaterialCombine(static_cast<int>(user_data & 0xffu));
}

inline PhysicMaterialCombine BounceCombineFromUserData(uint64_t user_data)
{
    return ToPhysicMaterialCombine(static_cast<int>((user_data >> 8) & 0xffu));
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

    // --- surface material (Unity's PhysicMaterial) --------------------------
    //
    // Unity authors these on the *collider*; Jolt (like Jolt's per-body
    // mFriction / mRestitution) has no per-shape material at all, so they live
    // on the body here. That is a real deviation and it is called out in
    // doc/PHYSIC_MATERIAL_SCRIPT_API.md rather than papered over.
    //
    // Unity's PhysicMaterial carries a *dynamic* and a *static* friction and
    // picks between them by whether the contact is sliding. Jolt exposes one
    // friction coefficient per body, so there is nowhere to put the second one
    // -- rather than ship a field that is stored, serialized and never read,
    // there is a single `m_Friction`.

    /// Unity's default PhysicMaterial dynamic friction. The previous engine-wide
    /// value was Jolt's own default (0.2) which nobody could see or change;
    /// adopting Unity's 0.6 makes newly authored bodies behave the way a Unity
    /// author expects, and existing assets now round-trip an explicit number.
    float m_Friction {0.6f};
    /// 0 = no bounce at all, which is Unity's default and Jolt's too.
    float m_Bounciness {0.f};
    /// Serialized as ints, not as the enum, for the same reason as m_ActorType:
    /// the on-disk asset predates the enum and a missing key must read as the
    /// default (0 == Average) rather than as garbage.
    int m_FrictionCombine {0};
    int m_BounceCombine {0};

    RigidBodyActorType GetActorType() const { return ToRigidBodyActorType(m_ActorType); }
    void SetActorType(RigidBodyActorType actor_type) { m_ActorType = ToSerializedActorType(actor_type); }

    PhysicMaterialCombine GetFrictionCombine() const { return ToPhysicMaterialCombine(m_FrictionCombine); }
    void SetFrictionCombine(PhysicMaterialCombine combine_mode)
    {
        m_FrictionCombine = ToSerializedCombineMode(combine_mode);
    }

    PhysicMaterialCombine GetBounceCombine() const { return ToPhysicMaterialCombine(m_BounceCombine); }
    void SetBounceCombine(PhysicMaterialCombine combine_mode)
    {
        m_BounceCombine = ToSerializedCombineMode(combine_mode);
    }
};

template<typename TransferFunction>
void RigidBodyComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shapes, "shapes");
    transfer.Transfer(m_InverseMass, "inverse_mass");
    transfer.Transfer(m_ActorType, "actor_type");
    transfer.Transfer(m_Friction, "friction");
    transfer.Transfer(m_Bounciness, "bounciness");
    transfer.Transfer(m_FrictionCombine, "friction_combine");
    transfer.Transfer(m_BounceCombine, "bounce_combine");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(RigidBodyComponentRes, , Transfer, void)
