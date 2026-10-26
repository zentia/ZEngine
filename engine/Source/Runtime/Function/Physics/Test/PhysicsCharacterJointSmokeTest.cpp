// Headless smoke test for the P0-2 physics additions: character controller,
// triggers (sensors) and joints.
//
// It does not boot the engine: a bare PhysicsScene is enough, and the bodies
// are created straight through Jolt rather than through RigidBodyComponentRes
// so the test does not need the object/reflection layer. That keeps it in the
// same shape as the other smoke tests in this repo (own main(), Check(), opt-in
// CMake flag, CTest entry).
//
// What it pins down:
//   * a controller can be spawned, stepped and resized;
//   * gravity actually moves it and it reports "in air" with no ground below;
//   * sensors flip and read back;
//   * joints of every exposed type can be created and removed exactly once,
//     and nonsense input is rejected instead of corrupting the scene.

#include "Runtime/Function/Physics/Jolt/Utils.h"
#include "Runtime/Function/Physics/PhysicsCharacter.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/BodyCreationSettings.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/PhysicsSystem.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        if (condition)
        {
            std::printf("[ ok ] %s\n", what);
            return;
        }

        std::printf("[FAIL] %s\n", what);
        ++g_failures;
    }

    /// A plain Jolt box, because going through RigidBodyComponentRes would drag
    /// the object + reflection layer into a headless test.
    uint32_t SpawnBody(PhysicsScene& in_scene, const Vector3& in_position, bool in_is_dynamic)
    {
        JPH::BodyInterface& body_interface = in_scene.GetJoltPhysicsSystem()->GetBodyInterface();

        JPH::BoxShapeSettings box_settings(JPH::Vec3(0.5f, 0.5f, 0.5f));

        JPH::BodyCreationSettings body_settings(&box_settings,
                                                JPH::RVec3(toVec3(in_position)),
                                                JPH::Quat::sIdentity(),
                                                in_is_dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
                                                in_is_dynamic ? Layers::MOVING : Layers::NON_MOVING);

        JPH::Body* jph_body = body_interface.CreateBody(body_settings);
        if (jph_body == nullptr)
        {
            return s_InvalidRigidbodyId;
        }

        body_interface.AddBody(jph_body->GetID(), JPH::EActivation::Activate);
        return jph_body->GetID().GetIndexAndSequenceNumber();
    }
}  // namespace

int main()
{
    PhysicsScene scene(Vector3(0.f, -9.81f, 0.f));

    // --- bodies -------------------------------------------------------------

    const uint32_t body_a = SpawnBody(scene, Vector3(-1.f, 2.f, 0.f), true);
    const uint32_t body_b = SpawnBody(scene, Vector3(1.f, 2.f, 0.f), true);

    Check(body_a != s_InvalidRigidbodyId, "dynamic body A created");
    Check(body_b != s_InvalidRigidbodyId, "dynamic body B created");

    // --- character controller -----------------------------------------------

    PhysicsCharacter character;

    CharacterControllerDesc desc;
    desc.m_Radius = 0.35f;
    desc.m_HalfHeight = 0.5f;

    Check(character.Create(desc, Vector3d(0.0, 5.0, 0.0), Quaternion(), &scene), "character created");
    Check(character.IsValid(), "character reports valid");

    const double start_y = character.GetPosition().y;
    Check(start_y > 4.0, "character spawns where it was told to");

    CharacterMoveResult last_result;
    for (int frame = 0; frame < 30; frame++)
    {
        scene.Tick(1.f / 60.f);
        last_result = character.Move(Vector3(0.f, 0.f, 0.f), 1.f / 60.f, Vector3(0.f, -9.81f, 0.f));
    }

    Check(last_result.m_Position.y < start_y, "gravity pulls the character down");
    Check(last_result.m_Velocity.y < 0.f, "character accumulates downward velocity");
    Check(last_result.m_GroundState == CharacterGroundState::InAir, "no ground below, so it is in the air");
    Check(!last_result.m_IsGrounded, "IsGrounded is false mid-fall");

    Check(character.Resize(0.3f, 0.4f), "character can be resized (crouch)");
    Check(!character.Resize(0.f, 0.4f), "a zero-radius capsule is rejected");

    character.Destroy();
    Check(!character.IsValid(), "character is released by Destroy()");

    // --- triggers -----------------------------------------------------------

    bool is_sensor = false;
    Check(scene.SetRigidBodySensor(body_b, true), "body B flagged as sensor");
    Check(scene.IsRigidBodySensor(body_b, is_sensor) && is_sensor, "sensor flag reads back");
    Check(scene.IsRigidBodySensor(body_a, is_sensor) && !is_sensor, "body A is not a sensor");
    Check(!scene.SetRigidBodySensor(s_InvalidRigidbodyId, true), "flagging an invalid body is rejected");

    std::vector<PhysicsTriggerEvent> events;
    scene.PollTriggerEvents(events);
    Check(events.empty(), "no trigger events in a scene where nothing overlaps yet");

    // --- joints -------------------------------------------------------------

    const PhysicsJointType all_types[] = {PhysicsJointType::fixed,
                                          PhysicsJointType::hinge,
                                          PhysicsJointType::point,
                                          PhysicsJointType::slider,
                                          PhysicsJointType::distance};

    for (PhysicsJointType type : all_types)
    {
        PhysicsJointDesc joint_desc;
        joint_desc.m_Type = type;
        joint_desc.m_BodyA = body_a;
        joint_desc.m_BodyB = body_b;
        joint_desc.m_PivotA = Vector3(-1.f, 2.f, 0.f);
        joint_desc.m_PivotB = Vector3(1.f, 2.f, 0.f);
        joint_desc.m_AutoDetectPoint = true;

        const uint32_t joint_id = scene.CreateJoint(joint_desc);
        Check(joint_id != s_InvalidJointId, "joint created");

        Check(scene.RemoveJoint(joint_id), "joint removed");
        Check(!scene.RemoveJoint(joint_id), "removing the same joint twice is rejected");
    }

    PhysicsJointDesc missing_body;
    missing_body.m_BodyA = s_InvalidRigidbodyId;
    missing_body.m_BodyB = body_b;
    Check(scene.CreateJoint(missing_body) == s_InvalidJointId, "joint with an invalid body is rejected");

    PhysicsJointDesc same_body;
    same_body.m_BodyA = body_a;
    same_body.m_BodyB = body_a;
    Check(scene.CreateJoint(same_body) == s_InvalidJointId, "joint between a body and itself is rejected");

    Check(!scene.RemoveJoint(s_InvalidJointId), "removing the invalid joint id is rejected");

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");

    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
