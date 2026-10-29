#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

enum SweepPass
{
    SWEEP_PASS_UP,
    SWEEP_PASS_SIDE,
    SWEEP_PASS_DOWN,
    SWEEP_PASS_SENSOR
};

class Controller
{
public:
    virtual ~Controller() = default;

    virtual Vector3 move(const Vector3& current_position, const Vector3& displacement) = 0;
};

/// Owns the Capsule it is initialized with AND the RigidBodyShape it builds from it. Both are
/// ObjectManager::NewObject objects (mimalloc, registered, with an instance ID) held only through
/// PPtr, so nothing else frees them; the destructor does, with MemoryManager::DestroyObject (the
/// pairing for NewObject). The caller must therefore pass a capsule it will NOT free itself.
class CharacterController : public Controller
{
public:
    /// Takes OWNERSHIP of `capsule`. Calling it again releases the previous capsule and shape first.
    void Initialize(PPtr<Capsule> capsule);
    ~CharacterController() override;
    CharacterController() = default;
    // Owning a raw heap pair: copying would double-free.
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    Vector3 move(const Vector3& current_position, const Vector3& displacement) override;

private:
    /// Destroy the shape, and the capsule unless it is `keep_capsule`.
    void ReleaseOwnedObjects(const Capsule* keep_capsule);

    PPtr<Capsule> m_Capsule;
    PPtr<RigidBodyShape> m_RigidbodyShape;
};