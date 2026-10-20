#pragma once

// Bridges an agent's decision output to ZEngine's gameplay layer.
//
// This component owns the AIBrain (blackboard + behaviour tree) and publishes
// the resulting move/action intent. It does *not* move the body itself: the
// execution contract is AIMoveIntent / AIActionRequest, which a project wires
// to MotorComponent (movement) or the GAS AbilitySystemComponent (skills).
// Keeping that seam explicit is what lets the AI layer run headless in tests.

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeTraitsBase.h"
#include "Runtime/Function/AI/AIBlackboard.h"
#include "Runtime/Function/AI/AITypes.h"
#include "Runtime/Function/AI/BehaviourTree.h"
#include "Runtime/Function/AI/Navigation/NavMesh.h"
#include "Runtime/Function/Framework/Component/Behaviour.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <cstdint>
#include <type_traits>
#include "AIControllerComponent.generated.h"

/// Training/personality presets. `custom` keeps whatever tree the project built
/// through SetTree; the others are the built-in templates from
/// AIControllerComponent::BuildDefaultTree.
enum class AIProfile : unsigned char
{
  custom,
  /// Chase while visible, fall back to idle otherwise.
  chase,
  /// Chase while healthy, retreat below m_RetreatHealthRatio.
  chase_and_retreat,
  /// Guard a point, chase only within m_GuardRadius.
  guard
};

template <>
class SerializeTraits<AIProfile> : public SerializeTraitsBase<AIProfile>
{
public:
  using value_type = AIProfile;

  inline static const char* GetTypeString(void* p = nullptr) { return "AIProfile"; }
  inline static bool AllowTransferOptimization() { return true; }

  template <typename TransferFunction>
  inline static void Transfer(value_type& data, TransferFunction& transfer)
  {
    using UnderlyingType = std::underlying_type_t<value_type>;
    transfer.TransferBasicData(reinterpret_cast<UnderlyingType&>(data));
  }
};

ZCLASS()
class AIControllerComponent : public Behaviour
{
    ZGENERATED_BODY(AIControllerComponent)

public:
  AIControllerComponent() = default;
  ~AIControllerComponent() override;

  void PostLoadResource(GameObject* parent_object) override;
  void OnSerializedFieldsUpdated() override;
  void Tick(float delta_time) override;

  /// Releases the sibling MotorComponent when this controller is switched off.
  /// Without this, disabling an AI mid-chase leaves the pawn drifting on the
  /// last intent forever -- the motor has no other way to learn the AI is gone.
  void SetEnabled(bool enabled) override;

  // ---- brain access --------------------------------------------------------

  AIBlackboard& GetBlackboard() { return m_Blackboard; }
  const AIBlackboard& GetBlackboard() const { return m_Blackboard; }

  /// Install a project-authored tree. Ownership transfers to the component.
  /// Passing nullptr reverts to the built-in template for the current profile.
  void SetTree(BehaviourTree* tree);

  /// Rebuild the built-in template for m_Profile, discarding the current tree.
  void BuildDefaultTree();

  // ---- output consumed by the gameplay layer -------------------------------

  const AIMoveIntent& GetMoveIntent() const { return m_Blackboard.GetMoveIntent(); }
  const AIActionRequest& GetActionRequest() const { return m_Blackboard.GetActionRequest(); }

  /// Consume the pending action request (clears it).
  AIActionRequest TakeActionRequest();

  /// When true (default) the controller pushes its move intent straight into
  /// the sibling MotorComponent each tick. This is the framework-level seam:
  /// scripts set up the AI (profile, ranges, tree) but never drive movement.
  ///
  /// Turn this off only when a project wants to consume GetMoveIntent() itself
  /// -- e.g. driving a custom controller or routing movement through GAS.
  bool IsDrivingMotor() const { return m_DriveMotor; }
  void SetDriveMotor(bool drive) { m_DriveMotor = drive; }

  BTNodeStatus GetLastTreeStatus() const { return m_LastStatus; }

  // ---- tuning knobs exposed to designers -----------------------------------

  float GetMoveSpeedRatio() const { return m_MoveSpeedRatio; }
  void SetMoveSpeedRatio(float ratio) { m_MoveSpeedRatio = ratio; }

  float GetAttackRange() const { return m_AttackRange; }
  void SetAttackRange(float range) { m_AttackRange = range; }

  /// Health ratio (0..1) below which the chase_and_retreat profile flees.
  float GetRetreatHealthRatio() const { return m_RetreatHealthRatio; }
  void SetRetreatHealthRatio(float ratio) { m_RetreatHealthRatio = ratio; }

  /// Agents report health here; the engine does not own a health attribute.
  void SetHealthRatio(float ratio);
  float GetHealthRatio() const { return m_HealthRatio; }

  float GetGuardRadius() const { return m_GuardRadius; }
  void SetGuardRadius(float radius) { m_GuardRadius = radius; }

  eastl::string GetAttackActionName() const { return m_AttackActionName; }
  void SetAttackActionName(eastl::string name) { m_AttackActionName = eastl::move(name); }

  /// Ask NavigationSystem for a path to `goal` and start following it.
  ///
  /// Rate limited by m_PathRefreshInterval and skipped when the goal has
  /// barely moved, because A* over the whole grid every frame (per agent) is
  /// the single easiest way to blow the AI frame budget.
  ///
  /// @return: true when a usable path is being followed
  bool RequestPathTo(const Vector3& goal);

  /// Stop following any path and forget the current goal.
  void ClearPath();

  bool IsFollowingPath() const { return m_HasPath && m_PathCursor < m_Path.size(); }

  /// Current waypoint the agent is walking toward. Falls back to the raw
  /// goal when navigation is unavailable, so callers need no special case.
  Vector3 GetCurrentWaypoint() const;

  /// Fraction along the path (0..1), exposed for animation/debug.
  float GetPathProgress() const;

  // ---- navigation tuning ---------------------------------------------------

  float GetPathRefreshInterval() const { return m_PathRefreshInterval; }
  void SetPathRefreshInterval(float seconds) { m_PathRefreshInterval = seconds; }

  /// Distance at which a waypoint counts as reached.
  float GetWaypointReachRadius() const { return m_WaypointReachRadius; }
  void SetWaypointReachRadius(float radius) { m_WaypointReachRadius = radius; }

  /// When false the agent walks straight at its goal even if a navmesh exists.
  bool IsNavigationEnabled() const { return m_NavigationEnabled; }
  void SetNavigationEnabled(bool enabled) { m_NavigationEnabled = enabled; }

  /// Number of path requests that failed since the last reset. Non-zero means
  /// the navmesh does not cover where this agent is trying to go.
  uint32_t GetFailedPathCount() const { return m_FailedPathCount; }

private:
  void RebuildTreeIfNeeded();
  void PushPerceptionToBlackboard();
  void PrewarmBlackboard();

  /// Push this frame's intent into the sibling MotorComponent (if any).
  void ApplyIntentToMotor(float delta_time);

  /// Hand the sibling MotorComponent back to player input / idle.
  void ReleaseMotor();

  /// Advance the waypoint cursor as the agent reaches each point.
  void UpdatePathFollow(const Vector3& self_position);

  ZPROPERTY()
  AIProfile m_Profile {AIProfile::chase};

  ZPROPERTY()
  float m_MoveSpeedRatio {1.f};
  ZPROPERTY()
  float m_AttackRange {2.5f};
  ZPROPERTY()
  float m_RetreatHealthRatio {0.3f};
  ZPROPERTY()
  float m_GuardRadius {15.f};
  ZPROPERTY()
  eastl::string m_AttackActionName {"attack"};

  float m_HealthRatio {1.f};

  AIBlackboard m_Blackboard;
  BehaviourTree* m_Tree {nullptr};
  BTNodeStatus m_LastStatus {BTNodeStatus::invalid};
  bool m_TreeDirty {true};
  bool m_DriveMotor {true};

  // ---- path following -----------------------------------------------------

  bool m_NavigationEnabled {true};
  eastl::vector<Vector3> m_Path;
  size_t m_PathCursor {0};
  bool m_HasPath {false};

  /// Last goal a path was requested for; used to skip re-pathing when the
  /// target has not meaningfully moved.
  Vector3 m_LastPathGoal {Vector3::ZERO};
  bool m_HasLastPathGoal {false};
  float m_PathTimeSinceRefresh {0.f};

  float m_PathRefreshInterval {0.5f};
  float m_WaypointReachRadius {0.5f};
  float m_RepathDistance {1.5f};

  uint32_t m_FailedPathCount {0};
};
