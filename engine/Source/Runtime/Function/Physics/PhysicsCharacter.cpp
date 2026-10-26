#include "Runtime/Function/Physics/PhysicsCharacter.h"

#include "Jolt/Core/TempAllocator.h"
#include "Jolt/Jolt.h"
#include "Jolt/Physics/Character/CharacterVirtual.h"
#include "Jolt/Physics/Body/BodyFilter.h"
#include "Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h"
#include "Jolt/Physics/Collision/ObjectLayer.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/ShapeFilter.h"
#include "Jolt/Physics/PhysicsSystem.h"
#include "Runtime/Function/Physics/Jolt/Utils.h"
#include "core/base/Macro.h"

#include <limits>

namespace
{
    CharacterGroundState FromJoltGroundState(JPH::CharacterBase::EGroundState in_state)
    {
        switch (in_state)
        {
            case JPH::CharacterBase::EGroundState::OnGround:
                return CharacterGroundState::OnGround;
            case JPH::CharacterBase::EGroundState::OnSteepGround:
                return CharacterGroundState::OnSteepGround;
            case JPH::CharacterBase::EGroundState::NotSupported:
                return CharacterGroundState::NotSupported;
            default:
                return CharacterGroundState::InAir;
        }
    }

    Vector3d ToVector3d(const JPH::RVec3& in_value)
    {
        return Vector3d(in_value.GetX(), in_value.GetY(), in_value.GetZ());
    }

    /// Step up / step down are expressed along the character's up axis so a
    /// controller with a non-Y up still climbs in the right direction.
    JPH::Vec3 AlongUp(const Vector3& in_up, float in_distance)
    {
        return toVec3(in_up) * in_distance;
    }
}  // namespace

PhysicsCharacter::~PhysicsCharacter()
{
    Destroy();
}

bool PhysicsCharacter::Create(const CharacterControllerDesc& in_desc,
                             const Vector3d& in_position,
                             const Quaternion& in_rotation,
                             PhysicsScene* in_scene)
{
    if (in_scene == nullptr || in_scene->GetJoltPhysicsSystem() == nullptr)
    {
        LOG_ERROR(ZPhysics, "CharacterController: null physics scene");
        return false;
    }

    if (IsValid())
    {
        LOG_ERROR(ZPhysics, "CharacterController: Create() on an already created controller");
        return false;
    }

    if (in_desc.m_Radius <= 0.f || in_desc.m_HalfHeight < 0.f)
    {
        LOG_ERROR(ZPhysics, "CharacterController: invalid capsule (r={}, h={})", in_desc.m_Radius, in_desc.m_HalfHeight);
        return false;
    }

    m_OwnerScene = in_scene;
    m_Desc = in_desc;

    m_JoltTempAllocator = new JPH::TempAllocatorImpl(in_desc.m_TempAllocatorSizeBytes);

    if (!BuildAndInstallShape(in_desc.m_Radius, in_desc.m_HalfHeight, true))
    {
        delete m_JoltTempAllocator;
        m_JoltTempAllocator = nullptr;
        m_OwnerScene = nullptr;
        return false;
    }

    JPH::CharacterVirtualSettings settings;
    settings.mShape = m_JoltShape;
    settings.mUp = toVec3(m_Desc.m_Up);
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(m_Desc.m_MaxSlopeAngleDegrees);
    settings.mMass = m_Desc.m_Mass;
    settings.mMaxStrength = m_Desc.m_MaxStrength;
    settings.mCharacterPadding = m_Desc.m_CharacterPadding;
    settings.mPredictiveContactDistance = m_Desc.m_PredictiveContactDistance;
    settings.mPenetrationRecoverySpeed = m_Desc.m_PenetrationRecoverySpeed;
    settings.mEnhancedInternalEdgeRemoval = m_Desc.m_EnhancedInternalEdgeRemoval;
    // Feet-at-origin convention: lift the centred capsule by half its height.
    settings.mShapeOffset = AlongUp(m_Desc.m_Up, m_Desc.m_HalfHeight + m_Desc.m_Radius);

    m_JoltCharacter = new JPH::CharacterVirtual(&settings,
                                                JPH::RVec3(toVec3(in_position.ToVector3())),
                                                toQuat(in_rotation),
                                                in_scene->GetJoltPhysicsSystem());

    return true;
}

void PhysicsCharacter::Destroy()
{
    if (m_JoltCharacter != nullptr)
    {
        // CharacterVirtual is a Jolt RefTarget: it was created with refcount 1,
        // so releasing it is what destroys it.
        m_JoltCharacter->Release();
        m_JoltCharacter = nullptr;
    }

    if (m_JoltShape != nullptr)
    {
        m_JoltShape->Release();
        m_JoltShape = nullptr;
    }

    delete m_JoltTempAllocator;
    m_JoltTempAllocator = nullptr;

    m_OwnerScene = nullptr;
}

bool PhysicsCharacter::BuildAndInstallShape(float in_radius, float in_half_height, bool in_is_initial)
{
    // Jolt capsulates are centred on the origin, but a character's shape is
    // expected to have its *bottom* at (0,0,0): the controller's position is
    // where the feet are. Lifting the shape by half its total height is what
    // makes "position = feet" hold.
    const float shape_offset = in_half_height + in_radius;

    JPH::Shape* new_shape = new JPH::CapsuleShape(in_half_height, in_radius);
    if (new_shape == nullptr)
    {
        LOG_ERROR(ZPhysics, "CharacterController: capsule shape allocation failed");
        return false;
    }

    if (in_is_initial)
    {
        m_JoltShape = new_shape;
        // The offset is stored on the character, not on the shape, so it is
        // applied when the controller is constructed in Create().
        return true;
    }

    const bool installed = m_JoltCharacter->SetShape(new_shape,
                                                     std::numeric_limits<float>::max(),
                                                     JPH::BroadPhaseLayerFilter(),
                                                     JPH::ObjectLayerFilter(),
                                                     JPH::BodyFilter(),
                                                     JPH::ShapeFilter(),
                                                     *m_JoltTempAllocator);

    if (!installed)
    {
        // SetShape() refuses when the new shape would be spawned deeply inside
        // geometry. Keep walking with the old capsule rather than dropping the
        // character through the world.
        LOG_ERROR(ZPhysics, "CharacterController: resize rejected (r={}, h={}), keeping previous capsule", in_radius, in_half_height);
        new_shape->Release();
        return false;
    }

    m_JoltCharacter->SetShapeOffset(AlongUp(m_Desc.m_Up, shape_offset));

    if (m_JoltShape != nullptr)
    {
        m_JoltShape->Release();
    }
    m_JoltShape = new_shape;

    m_Desc.m_Radius = in_radius;
    m_Desc.m_HalfHeight = in_half_height;

    return true;
}

CharacterMoveResult PhysicsCharacter::Move(const Vector3& in_velocity, float in_delta_time, const Vector3& in_gravity)
{
    CharacterMoveResult result;

    if (!IsValid())
    {
        return result;
    }

    if (in_delta_time <= 0.f)
    {
        // Nothing to integrate; still report where the controller is so a
        // caller that polls every frame sees a consistent snapshot.
        result.m_Position = GetPosition();
        result.m_Velocity = GetLinearVelocity();
        result.m_GroundState = GetGroundState();
        result.m_GroundNormal = GetGroundNormal();
        result.m_GroundVelocity = GetGroundVelocity();
        result.m_GroundBodyId = GetGroundBodyId();
        result.m_IsGrounded = result.m_GroundState == CharacterGroundState::OnGround;
        return result;
    }

    JPH::CharacterVirtual::ExtendedUpdateSettings extended;
    extended.mStickToFloorStepDown = m_Desc.m_EnableStickToFloor ? AlongUp(m_Desc.m_Up, -m_Desc.m_StepDownDistance) : JPH::Vec3::sZero();
    extended.mWalkStairsStepUp = m_Desc.m_EnableWalkStairs ? AlongUp(m_Desc.m_Up, m_Desc.m_StepUpHeight) : JPH::Vec3::sZero();

    // ExtendedUpdate() reads the velocity off the character, it is not a
    // parameter — so the desired velocity has to be pushed in first.
    m_JoltCharacter->SetLinearVelocity(toVec3(in_velocity));

    m_JoltCharacter->ExtendedUpdate(in_delta_time,
                                    toVec3(in_gravity),
                                    extended,
                                    JPH::BroadPhaseLayerFilter(),
                                    JPH::ObjectLayerFilter(),
                                    JPH::BodyFilter(),
                                    JPH::ShapeFilter(),
                                    *m_JoltTempAllocator);

    result.m_Position = GetPosition();
    result.m_Velocity = GetLinearVelocity();
    result.m_GroundState = GetGroundState();
    result.m_GroundNormal = GetGroundNormal();
    result.m_GroundVelocity = GetGroundVelocity();
    result.m_GroundBodyId = GetGroundBodyId();
    result.m_IsGrounded = result.m_GroundState == CharacterGroundState::OnGround;

    return result;
}

bool PhysicsCharacter::IsGrounded() const
{
    return IsValid() && m_JoltCharacter->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

CharacterGroundState PhysicsCharacter::GetGroundState() const
{
    if (!IsValid())
    {
        return CharacterGroundState::InAir;
    }

    return FromJoltGroundState(m_JoltCharacter->GetGroundState());
}

Vector3 PhysicsCharacter::GetGroundNormal() const
{
    if (!IsValid())
    {
        return Vector3(0.f, 1.f, 0.f);
    }

    return toVec3(m_JoltCharacter->GetGroundNormal());
}

Vector3 PhysicsCharacter::GetGroundVelocity() const
{
    if (!IsValid())
    {
        return Vector3(0.f, 0.f, 0.f);
    }

    return toVec3(m_JoltCharacter->GetGroundVelocity());
}

uint32_t PhysicsCharacter::GetGroundBodyId() const
{
    if (!IsValid())
    {
        return s_InvalidRigidbodyId;
    }

    return m_JoltCharacter->GetGroundBodyID().GetIndexAndSequenceNumber();
}

Vector3d PhysicsCharacter::GetPosition() const
{
    if (!IsValid())
    {
        return Vector3d();
    }

    return ToVector3d(m_JoltCharacter->GetPosition());
}

void PhysicsCharacter::SetPosition(const Vector3d& in_position)
{
    if (!IsValid())
    {
        return;
    }

    m_JoltCharacter->SetPosition(JPH::RVec3(toVec3(in_position.ToVector3())));
}

Quaternion PhysicsCharacter::GetRotation() const
{
    if (!IsValid())
    {
        return Quaternion();
    }

    return toQuat(m_JoltCharacter->GetRotation());
}

void PhysicsCharacter::SetRotation(const Quaternion& in_rotation)
{
    if (!IsValid())
    {
        return;
    }

    m_JoltCharacter->SetRotation(toQuat(in_rotation));
}

Vector3 PhysicsCharacter::GetLinearVelocity() const
{
    if (!IsValid())
    {
        return Vector3(0.f, 0.f, 0.f);
    }

    return toVec3(m_JoltCharacter->GetLinearVelocity());
}

void PhysicsCharacter::SetLinearVelocity(const Vector3& in_velocity)
{
    if (!IsValid())
    {
        return;
    }

    m_JoltCharacter->SetLinearVelocity(toVec3(in_velocity));
}

bool PhysicsCharacter::Resize(float in_radius, float in_half_height)
{
    if (!IsValid())
    {
        return false;
    }

    if (in_radius <= 0.f || in_half_height < 0.f)
    {
        LOG_ERROR(ZPhysics, "CharacterController: invalid capsule (r={}, h={})", in_radius, in_half_height);
        return false;
    }

    return BuildAndInstallShape(in_radius, in_half_height, false);
}

void PhysicsCharacter::SetUp(const Vector3& in_up)
{
    m_Desc.m_Up = in_up;

    if (!IsValid())
    {
        return;
    }

    m_JoltCharacter->SetUp(toVec3(in_up));
}

void PhysicsCharacter::SetMaxSlopeAngleDegrees(float in_degrees)
{
    m_Desc.m_MaxSlopeAngleDegrees = in_degrees;

    if (!IsValid())
    {
        return;
    }

    m_JoltCharacter->SetMaxSlopeAngle(JPH::DegreesToRadians(in_degrees));
}

void PhysicsCharacter::SetMaxStrength(float in_strength)
{
    m_Desc.m_MaxStrength = in_strength;

    if (!IsValid())
    {
        return;
    }

    m_JoltCharacter->SetMaxStrength(in_strength);
}
