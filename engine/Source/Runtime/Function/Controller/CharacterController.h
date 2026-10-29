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

class CharacterController : public Controller
{
public:
    void Initialize(PPtr<Capsule> capsule);
    ~CharacterController() = default;

    Vector3 move(const Vector3& current_position, const Vector3& displacement) override;

private:
    PPtr<Capsule> m_Capsule;
    PPtr<RigidBodyShape> m_RigidbodyShape;
};