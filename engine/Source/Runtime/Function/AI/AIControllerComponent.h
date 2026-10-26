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
///
/// ZENUM: m_Profile is a ZPROPERTY of this type, and an enum that is not a
/// ZENUM is silently skipped when the field property table is built -- the
/// Inspector could edit `profile` but scripts could not. Marking it publishes
/// `profile: AIProfile` on the component, so a script can switch personality
/// (and then call BuildDefaultTree() to actually rebuild the tree for it).
ZENUM()
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
  ///
  /// 不导出：BehaviourTree 不是反射类（没有 ZCLASS），脚本无论如何造不出一个实例。
  void SetTree(BehaviourTree* tree);

  /// Rebuild the built-in template for m_Profile, discarding the current tree.
  /// 导出：脚本改了 `profile` 属性之后要靠它把树重建出来（`profile` 本身只是个
  /// 序列化字段，不会自动触发重建 —— RebuildTreeIfNeeded 只认 m_TreeDirty）。
  ZFUNCTION()
  void BuildDefaultTree();

  // ---- output consumed by the gameplay layer -------------------------------
  //
  // 这一块是"脚本能读意图"的那半边。AIMoveIntent / AIActionRequest 都是纯值
  // 结构（ScriptBinding.h 里有 Converter），直接按对象过界。

  ZFUNCTION()
  const AIMoveIntent& GetMoveIntent() const { return m_Blackboard.GetMoveIntent(); }
  ZFUNCTION()
  const AIActionRequest& GetActionRequest() const { return m_Blackboard.GetActionRequest(); }

  /// Consume the pending action request (clears it).
  ZFUNCTION()
  AIActionRequest TakeActionRequest();

  // ---- 脚本下意图的入口 ----------------------------------------------------
  //
  // 注意"下意图"和"读意图"是两个方向，不要混：读的是行为树这一帧算出来的结果，
  // 下的是脚本自己塞进去的请求。两者共用同一个 blackboard 槽位，所以**谁最后写
  // 谁赢** —— 脚本在 OnUpdate 里 RequestAction 会被下一次树 tick 覆盖（如果树
  // 也发动作请求）。想要脚本独占动作通道，就把行为树配成不发请求，或者脚本
  // 每帧都写。

  /// 请求执行一个具名动作（技能 / 动画标签），`target_id` 可传
  /// k_invalid_gobject_id 表示没有目标。执行层（GAS / AbilitySystemComponent）
  /// 按名字和目标 id 自己去解析。
  ZFUNCTION()
  void RequestAction(const eastl::string& action_name, GObjectID target_id)
  {
    AIActionRequest request;
    request.action_name = action_name;
    request.target_id = target_id;
    m_Blackboard.SetActionRequest(request);
  }
  /// 撤掉还没被消费的动作请求。
  ZFUNCTION()
  void ClearActionRequest() { m_Blackboard.ClearActionRequest(); }

  /// When true (default) the controller pushes its move intent straight into
  /// the sibling MotorComponent each tick. This is the framework-level seam:
  /// scripts set up the AI (profile, ranges, tree) but never drive movement.
  ///
  /// Turn this off only when a project wants to consume GetMoveIntent() itself
  /// -- e.g. driving a custom controller or routing movement through GAS.
  ///
  /// 导出：脚本想自己消费 GetMoveIntent() 就必须能关掉它，否则两边一起推马达。
  ZFUNCTION()
  bool IsDrivingMotor() const { return m_DriveMotor; }
  ZFUNCTION()
  void SetDriveMotor(bool drive) { m_DriveMotor = drive; }

  ZFUNCTION()
  BTNodeStatus GetLastTreeStatus() const { return m_LastStatus; }

  // ---- 读感知结果（黑板上的快路径，由 AIPerceptionComponent 每帧推送）----

  /// 有没有"主目标"= 最近的那个可见目标。没有 = false。
  ZFUNCTION()
  bool HasPrimaryTarget() const { return m_Blackboard.HasPrimaryTarget(); }
  /// 主目标的 GObjectID；没有 = k_invalid_gobject_id（一个很大的数）。
  ZFUNCTION()
  GObjectID GetPrimaryTargetId() const { return m_Blackboard.GetPrimaryTargetId(); }
  ZFUNCTION()
  bool HasLineOfSightToTarget() const { return m_Blackboard.HasLineOfSightToTarget(); }
  /// 到主目标的距离；没有主目标 = 0。
  ZFUNCTION()
  float GetDistanceToTarget() const { return m_Blackboard.GetDistanceToTarget(); }

  // ---- tuning knobs exposed to designers -----------------------------------

  float GetMoveSpeedRatio() const { return m_MoveSpeedRatio; }
  void SetMoveSpeedRatio(float ratio) { m_MoveSpeedRatio = ratio; }

  float GetAttackRange() const { return m_AttackRange; }
  void SetAttackRange(float range) { m_AttackRange = range; }

  /// Health ratio (0..1) below which the chase_and_retreat profile flees.
  float GetRetreatHealthRatio() const { return m_RetreatHealthRatio; }
  void SetRetreatHealthRatio(float ratio) { m_RetreatHealthRatio = ratio; }

  /// Agents report health here; the engine does not own a health attribute.
  ZFUNCTION()
  void SetHealthRatio(float ratio);
  ZFUNCTION()
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
  ///
  /// 这就是"下移动意图"的正路：脚本给一个世界坐标目标，控制器去寻路并沿着走，
  /// 同时按 m_DriveMotor 把意图推进 MotorComponent。比自己写 SetMoveIntent 稳 ——
  /// 后者会被下一次树 tick 直接覆盖掉。
  ZFUNCTION()
  bool RequestPathTo(const Vector3& goal);

  /// Stop following any path and forget the current goal.
  ZFUNCTION()
  void ClearPath();

  ZFUNCTION()
  bool IsFollowingPath() const { return m_HasPath && m_PathCursor < m_Path.size(); }

  /// Current waypoint the agent is walking toward. Falls back to the raw
  /// goal when navigation is unavailable, so callers need no special case.
  ZFUNCTION()
  Vector3 GetCurrentWaypoint() const;

  /// Fraction along the path (0..1), exposed for animation/debug.
  ZFUNCTION()
  float GetPathProgress() const;

  // ---- navigation tuning ---------------------------------------------------
  // 这几个不是 ZPROPERTY（没有序列化），所以不会自动生成脚本属性，得手动标。

  ZFUNCTION()
  float GetPathRefreshInterval() const { return m_PathRefreshInterval; }
  ZFUNCTION()
  void SetPathRefreshInterval(float seconds) { m_PathRefreshInterval = seconds; }

  /// Distance at which a waypoint counts as reached.
  ZFUNCTION()
  float GetWaypointReachRadius() const { return m_WaypointReachRadius; }
  ZFUNCTION()
  void SetWaypointReachRadius(float radius) { m_WaypointReachRadius = radius; }

  /// When false the agent walks straight at its goal even if a navmesh exists.
  ZFUNCTION()
  bool IsNavigationEnabled() const { return m_NavigationEnabled; }
  ZFUNCTION()
  void SetNavigationEnabled(bool enabled) { m_NavigationEnabled = enabled; }

  /// Number of path requests that failed since the last reset. Non-zero means
  /// the navmesh does not cover where this agent is trying to go.
  ZFUNCTION()
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
