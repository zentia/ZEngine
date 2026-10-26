#pragma once

// -----------------------------------------------------------------------------
// Character controller, built on top of Jolt's CharacterVirtual.
//
// Semantics deliberately follow Unity's CharacterController rather than a rigid
// body: the character is a kinematic capsule that is swept through the world,
// slides along walls, climbs stairs and sticks to the floor. Nothing in the
// simulation can push it around or topple it, and gravity is applied by the
// caller — which is exactly what player movement code wants. A dynamic body
// would constantly fight the input (it accumulates velocity, it can be knocked
// over, it needs damping before it feels controllable).
//
// Lifecycle: Create() needs a live PhysicsScene (the Jolt PhysicsSystem is
// captured at creation time), Destroy() must run before that scene dies.
// The character is stepped by the caller *after* PhysicsScene::Tick(), because
// ExtendedUpdate() reads the world that the last physics step produced.
//
// On Web (Emscripten) Jolt is not linked at all (see the Emscripten branch in
// PhysicsScene.h), and CMake strips Function/Physics/*.cpp there, so this class
// simply does not exist in that configuration.
// -----------------------------------------------------------------------------

#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

#include <cstddef>

namespace JPH
{
    class CharacterVirtual;
    class Shape;
    class TempAllocator;
}  // namespace JPH

/// Mirror of JPH::CharacterBase::EGroundState, kept free of the Jolt include so
/// gameplay code can switch on it.
enum class CharacterGroundState : unsigned char
{
    OnGround,       ///< Standing on ground the character can walk on.
    OnSteepGround,  ///< Touching ground, but too steep to stand on — slide off.
    NotSupported,   ///< Touching something, but nothing that supports it.
    InAir           ///< Not touching anything.
};

/// Everything needed to spawn a controller. Defaults approximate a human-sized
/// capsule in metres.
struct CharacterControllerDesc
{
    /// Capsule radius.
    float m_Radius {0.35f};

    /// Half height of the capsule's cylindrical part (the caps add m_Radius on
    /// top of this at both ends, so total height = 2 * (m_HalfHeight + m_Radius)).
    float m_HalfHeight {0.5f};

    /// Which way is up. Also the axis stairs are climbed along.
    Vector3 m_Up {0.f, 1.f, 0.f};

    /// Steepest slope (degrees) the character can stand on.
    float m_MaxSlopeAngleDegrees {50.f};

    /// Mass used when pushing dynamic bodies, in kg.
    float m_Mass {70.f};

    /// Strength when pushing dynamic bodies.
    float m_MaxStrength {100.f};

    /// How far the sweep stays away from geometry. Higher is cheaper and less
    /// likely to get stuck, but leaves a visible gap.
    float m_CharacterPadding {0.02f};

    /// How far outside the shape predictive contacts are scanned for. 0 makes
    /// the character get stuck: it can no longer compute a sliding direction.
    float m_PredictiveContactDistance {0.1f};

    /// Extra work to drop ghost contacts on internal edges of triangle meshes.
    float m_PenetrationRecoverySpeed {1.f};

    bool m_EnhancedInternalEdgeRemoval {true};

    /// Climb ledges instead of walking into them.
    bool m_EnableWalkStairs {true};

    /// Stay glued to the floor when walking down a slope instead of launching
    /// off it.
    bool m_EnableStickToFloor {true};

    float m_StepUpHeight {0.4f};
    float m_StepDownDistance {0.5f};

    /// Scratch memory handed to Jolt for one ExtendedUpdate(). Each controller
    /// owns its own allocator so two controllers can be stepped from different
    /// threads without aliasing each other's scratch buffer.
    std::size_t m_TempAllocatorSizeBytes {1024 * 1024};
};

/// What a Move() produced. Everything here is post-simulation state.
struct CharacterMoveResult
{
    Vector3d m_Position;
    Vector3 m_Velocity;
    CharacterGroundState m_GroundState {CharacterGroundState::InAir};
    Vector3 m_GroundNormal {0.f, 1.f, 0.f};
    Vector3 m_GroundVelocity {0.f, 0.f, 0.f};
    uint32_t m_GroundBodyId {s_InvalidRigidbodyId};
    bool m_IsGrounded {false};
};

class PhysicsCharacter
{
public:
    PhysicsCharacter() = default;
    ~PhysicsCharacter();

    PhysicsCharacter(const PhysicsCharacter&) = delete;
    PhysicsCharacter& operator=(const PhysicsCharacter&) = delete;

    /// @return false when the scene is null, has no Jolt system, or the shape
    /// could not be built. A failed Create() leaves the object untouched.
    bool Create(const CharacterControllerDesc& in_desc,
                const Vector3d& in_position,
                const Quaternion& in_rotation,
                PhysicsScene* in_scene);

    void Destroy();
    bool IsValid() const { return m_JoltCharacter != nullptr; }

    // --- stepping -----------------------------------------------------------

    /// Advance the character by one frame.
    ///
    /// @in_velocity: desired velocity in units/second, including any vertical
    /// component the caller wants (jump, fall). Gravity is added on top.
    /// @in_gravity: gravity in units/second^2, typically the scene gravity.
    ///
    /// Must be called after PhysicsScene::Tick() for this frame.
    CharacterMoveResult Move(const Vector3& in_velocity, float in_delta_time, const Vector3& in_gravity);

    // --- ground state -------------------------------------------------------

    bool IsGrounded() const;
    CharacterGroundState GetGroundState() const;
    Vector3 GetGroundNormal() const;
    Vector3 GetGroundVelocity() const;
    uint32_t GetGroundBodyId() const;

    // --- transform / velocity -----------------------------------------------

    Vector3d GetPosition() const;
    void SetPosition(const Vector3d& in_position);
    Quaternion GetRotation() const;
    void SetRotation(const Quaternion& in_rotation);
    Vector3 GetLinearVelocity() const;
    void SetLinearVelocity(const Vector3& in_velocity);

    /// Rebuild the capsule at runtime (crouch / stand up). Returns false and
    /// keeps the old shape when the new one cannot be placed without ending up
    /// deeply embedded in geometry.
    bool Resize(float in_radius, float in_half_height);

    // --- tuning -------------------------------------------------------------

    void SetUp(const Vector3& in_up);
    void SetMaxSlopeAngleDegrees(float in_degrees);
    void SetMaxStrength(float in_strength);

    PhysicsScene* GetOwnerScene() const { return m_OwnerScene; }
    const CharacterControllerDesc& GetDesc() const { return m_Desc; }

private:
    /// Shared tail of Create()/Resize(): build the capsule and (re)install it.
    bool BuildAndInstallShape(float in_radius, float in_half_height, bool in_is_initial);

    JPH::CharacterVirtual* m_JoltCharacter {nullptr};
    JPH::Shape* m_JoltShape {nullptr};
    JPH::TempAllocator* m_JoltTempAllocator {nullptr};
    PhysicsScene* m_OwnerScene {nullptr};
    CharacterControllerDesc m_Desc;
};
