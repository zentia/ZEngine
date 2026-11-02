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

/// Unity 的 `RigidbodyConstraints`：位标志，脚本写
/// `rigidbody.constraints = RigidbodyConstraints.FreezeRotationY`。
///
/// 取值与 Unity **逐位相同**（X=2 / Y=4 / Z=8 / rotX=16 / rotY=32 / rotZ=64），
/// 因为从 Unity 搬过来的代码里既有按位或也有直接写 `FreezeAll`。改任何一个值
/// 都会让 `FreezeRotation = 112` 这类常量悄悄错位。
///
/// ⚠️ 底层类型刻意是 `int` 不是 `unsigned char`：ZENUM 按底层类型过边界，而
/// `FreezeAll = 126` 已经超过一个字节能舒服表达的范围之外的**语义**（126 本身
/// 进得去，但按位运算的结果在脚本侧是 number，用 int 少一次转换）。
ZENUM()
enum class RigidbodyConstraints : int
{
    None            = 0,
    FreezePositionX = 2,
    FreezePositionY = 4,
    FreezePositionZ = 8,
    FreezePosition  = 14,
    FreezeRotationX = 16,
    FreezeRotationY = 32,
    FreezeRotationZ = 64,
    FreezeRotation  = 112,
    FreezeAll       = 126,
};

/// Unity 的 `CollisionDetectionMode`。
///
/// ⚠️ Jolt 只有**两个**运动质量（`EMotionQuality::Discrete` 与 `LinearCast`），
/// Unity 有四个模式。四个名字都保留（脚本搬过来能编译），但
/// `Continuous` / `ContinuousDynamic` / `ContinuousSpeculative` 落到 Jolt 上都
/// 是 LinearCast。**读回来会退化成 `Continuous`** —— 这是有意的窄化：让脚本
/// 以为自己拿到的是"设进去的那个"，就得在 body 旁边另存一份作者值，而那份
/// 值和 Jolt 的真实状态迟早会分叉。宁可返回一个"确实在用的那个"。
ZENUM()
enum class CollisionDetectionMode : unsigned char
{
    Discrete              = 0,
    Continuous            = 1,
    ContinuousDynamic     = 2,
    ContinuousSpeculative = 3,
};

/// 三个连续模式里到底是不是连续：判据集中在这里，而不是散在各处写
/// `!= Discrete`。加第四种"不连续"的模式时只需要改这一处。
inline bool UsesContinuousCollision(CollisionDetectionMode in_mode)
{
    return in_mode != CollisionDetectionMode::Discrete;
}

/// Unity 的约束位 -> Jolt 的 `EAllowedDOFs` 掩码。
///
/// 与 `ToRigidBodyActorType` 同一条约定：**映射只活在一处**，且是纯粹的位
/// 运算、不碰任何 Jolt 类型 —— 于是它能被一个不起物理世界的单测直接断言
/// （映射写反一位的表现是"物体在某个轴上悄悄不动了"，是最难查的一类）。
///
/// Jolt 的位序（`AllowedDOFs.h`）：Tx=1 Ty=2 Tz=4 Rx=8 Ry=16 Rz=32。
/// Unity 的位序与轴序都不同（位移从 2 起，旋转从 16 起），所以只能逐位搬，
/// 不能整体移位。
inline uint32_t ToJoltAllowedDOFs(RigidbodyConstraints in_constraints)
{
    const int constraints = static_cast<int>(in_constraints);

    uint32_t dofs = 0;
    if ((constraints & static_cast<int>(RigidbodyConstraints::FreezePositionX)) == 0)
    {
        dofs |= 1u << 0;  // TranslationX
    }
    if ((constraints & static_cast<int>(RigidbodyConstraints::FreezePositionY)) == 0)
    {
        dofs |= 1u << 1;  // TranslationY
    }
    if ((constraints & static_cast<int>(RigidbodyConstraints::FreezePositionZ)) == 0)
    {
        dofs |= 1u << 2;  // TranslationZ
    }
    if ((constraints & static_cast<int>(RigidbodyConstraints::FreezeRotationX)) == 0)
    {
        dofs |= 1u << 3;  // RotationX
    }
    if ((constraints & static_cast<int>(RigidbodyConstraints::FreezeRotationY)) == 0)
    {
        dofs |= 1u << 4;  // RotationY
    }
    if ((constraints & static_cast<int>(RigidbodyConstraints::FreezeRotationZ)) == 0)
    {
        dofs |= 1u << 5;  // RotationZ
    }
    return dofs;
}

/// Jolt 的 `EAllowedDOFs` 掩码 -> Unity 的约束位。`ToJoltAllowedDOFs` 的逆。
///
/// 掩码里未出现的轴（Jolt 的 `All` 之外可能缺位）就是"被冻住"的轴，所以未知
/// 位一律翻译成"冻住"而不是"放开" —— 宁可多冻一个轴（看得见），不可少冻一个
/// （"明明设了 freeze 它还在转"）。
inline RigidbodyConstraints ToRigidbodyConstraints(uint32_t in_jolt_dofs)
{
    int constraints = 0;
    if ((in_jolt_dofs & (1u << 0)) == 0)
    {
        constraints |= static_cast<int>(RigidbodyConstraints::FreezePositionX);
    }
    if ((in_jolt_dofs & (1u << 1)) == 0)
    {
        constraints |= static_cast<int>(RigidbodyConstraints::FreezePositionY);
    }
    if ((in_jolt_dofs & (1u << 2)) == 0)
    {
        constraints |= static_cast<int>(RigidbodyConstraints::FreezePositionZ);
    }
    if ((in_jolt_dofs & (1u << 3)) == 0)
    {
        constraints |= static_cast<int>(RigidbodyConstraints::FreezeRotationX);
    }
    if ((in_jolt_dofs & (1u << 4)) == 0)
    {
        constraints |= static_cast<int>(RigidbodyConstraints::FreezeRotationY);
    }
    if ((in_jolt_dofs & (1u << 5)) == 0)
    {
        constraints |= static_cast<int>(RigidbodyConstraints::FreezeRotationZ);
    }
    return static_cast<RigidbodyConstraints>(constraints);
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
