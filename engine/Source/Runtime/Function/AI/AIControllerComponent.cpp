#include "Runtime/Function/AI/AIControllerComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/AI/AIPerceptionComponent.h"
#include "Runtime/Function/AI/AISystem.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include <chrono>

IMPLEMENT_REGISTER_CLASS(AIControllerComponent)
IMPLEMENT_OBJECT_SERIALIZE(AIControllerComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AIControllerComponent)

namespace
{
  // ---- blackboard slot keys -------------------------------------------------
  // Shared between the built-in trees and any project-authored tree, so an
  // authored tree can read what perception + controller already publish.
  const char* const kKeyHasTarget = "has_target";
  const char* const kKeyHasLineOfSight = "has_line_of_sight";
  const char* const kKeyDistanceToTarget = "distance_to_target";
  const char* const kKeyAttackRange = "attack_range";
  const char* const kKeyMoveSpeedRatio = "move_speed_ratio";
  const char* const kKeyHealthRatio = "health_ratio";
  const char* const kKeyRetreatHealthRatio = "retreat_health_ratio";
  const char* const kKeyGuardRadius = "guard_radius";
  const char* const kKeyHomePositionX = "home_position_x";
  const char* const kKeyHomePositionY = "home_position_y";
  const char* const kKeyHomePositionZ = "home_position_z";
  const char* const kKeySelfPositionX = "self_position_x";
  const char* const kKeySelfPositionY = "self_position_y";
  const char* const kKeySelfPositionZ = "self_position_z";
  const char* const kKeyTargetPositionX = "target_position_x";
  const char* const kKeyTargetPositionY = "target_position_y";
  const char* const kKeyTargetPositionZ = "target_position_z";
  const char* const kKeyAttackActionName = "attack_action_name";

  Vector3 ReadPosition(const AIBlackboard& blackboard,
                       const char* key_x,
                       const char* key_y,
                       const char* key_z)
  {
    return Vector3(blackboard.GetFloatOrDefault(key_x, 0.f),
                   blackboard.GetFloatOrDefault(key_y, 0.f),
                   blackboard.GetFloatOrDefault(key_z, 0.f));
  }

  float SquaredDistance(const Vector3& lhs, const Vector3& rhs) { return lhs.squaredDistance(rhs); }

  // ---- conditions -----------------------------------------------------------

  bool ConditionHasTarget(const AIBlackboard& blackboard)
  {
    return blackboard.GetBoolOrDefault(kKeyHasTarget, false);
  }

  bool ConditionHasLineOfSight(const AIBlackboard& blackboard)
  {
    return blackboard.GetBoolOrDefault(kKeyHasLineOfSight, false);
  }

  bool ConditionTargetInAttackRange(const AIBlackboard& blackboard)
  {
    const float distance = blackboard.GetFloatOrDefault(kKeyDistanceToTarget, -1.f);
    const float attack_range = blackboard.GetFloatOrDefault(kKeyAttackRange, 0.f);
    return distance >= 0.f && distance <= attack_range;
  }

  bool ConditionHealthBelowRetreat(const AIBlackboard& blackboard)
  {
    const float health = blackboard.GetFloatOrDefault(kKeyHealthRatio, 1.f);
    const float retreat = blackboard.GetFloatOrDefault(kKeyRetreatHealthRatio, 0.f);
    return retreat > 0.f && health < retreat;
  }

  bool ConditionTargetWithinGuardRadius(const AIBlackboard& blackboard)
  {
    const Vector3 home = ReadPosition(blackboard, kKeyHomePositionX, kKeyHomePositionY, kKeyHomePositionZ);
    const Vector3 target =
        ReadPosition(blackboard, kKeyTargetPositionX, kKeyTargetPositionY, kKeyTargetPositionZ);
    const float guard_radius = blackboard.GetFloatOrDefault(kKeyGuardRadius, 0.f);
    return SquaredDistance(home, target) <= guard_radius * guard_radius;
  }

  bool ConditionAwayFromHome(const AIBlackboard& blackboard)
  {
    const Vector3 home = ReadPosition(blackboard, kKeyHomePositionX, kKeyHomePositionY, kKeyHomePositionZ);
    const Vector3 self = ReadPosition(blackboard, kKeySelfPositionX, kKeySelfPositionY, kKeySelfPositionZ);
    // 0.25m dead zone so a guarding agent does not jitter around its post.
    return SquaredDistance(home, self) > 0.25f * 0.25f;
  }

  // ---- actions --------------------------------------------------------------

  BTNodeStatus ActionMoveToTarget(AIBlackboard& blackboard, float delta_time)
  {
    if (!blackboard.HasPrimaryTarget())
    {
      return BTNodeStatus::failure;
    }

    const Vector3 target =
        ReadPosition(blackboard, kKeyTargetPositionX, kKeyTargetPositionY, kKeyTargetPositionZ);
    AIControllerComponent* controller = blackboard.GetOwnerController();

    // Prefer a navmesh path; fall back to the straight line when there is no
    // mesh or no route. The intent contract is identical either way, so the
    // motor needs no knowledge of which one produced it.
    Vector3 destination = target;
    if (controller != nullptr && controller->RequestPathTo(target))
    {
      destination = controller->GetCurrentWaypoint();
    }

    AIMoveIntent intent;
    intent.has_move_target = true;
    intent.move_target = destination;
    intent.speed_ratio = blackboard.GetFloatOrDefault(kKeyMoveSpeedRatio, 1.f);
    intent.has_face_target = true;
    intent.face_target = target;
    blackboard.SetMoveIntent(intent);

    (void)delta_time;
    return BTNodeStatus::running;
  }

  BTNodeStatus ActionStopMoving(AIBlackboard& blackboard, float delta_time)
  {
    (void)delta_time;
    blackboard.SetMoveIntent(AIMoveIntent {});
    return BTNodeStatus::success;
  }

  BTNodeStatus ActionAttack(AIBlackboard& blackboard, float delta_time)
  {
    (void)delta_time;
    if (!blackboard.HasPrimaryTarget())
    {
      return BTNodeStatus::failure;
    }

    // Standing still while attacking: execution layer owns the animation.
    AIMoveIntent intent;
    intent.has_move_target = false;
    intent.has_face_target = true;
    intent.face_target =
        ReadPosition(blackboard, kKeyTargetPositionX, kKeyTargetPositionY, kKeyTargetPositionZ);
    blackboard.SetMoveIntent(intent);

    AIActionRequest request;
    request.action_name = blackboard.GetStringOrDefault(kKeyAttackActionName, eastl::string("attack"));
    request.target_id = blackboard.GetPrimaryTargetId();
    blackboard.SetActionRequest(request);

    return BTNodeStatus::success;
  }

  BTNodeStatus ActionRetreat(AIBlackboard& blackboard, float delta_time)
  {
    (void)delta_time;
    if (!blackboard.HasPrimaryTarget())
    {
      return BTNodeStatus::failure;
    }

    const Vector3 self = ReadPosition(blackboard, kKeySelfPositionX, kKeySelfPositionY, kKeySelfPositionZ);
    const Vector3 target =
        ReadPosition(blackboard, kKeyTargetPositionX, kKeyTargetPositionY, kKeyTargetPositionZ);

    Vector3 away = self - target;
    if (away.squaredLength() <= 0.f)
    {
      return BTNodeStatus::failure;
    }
    away.normalise();

    // Flee to a point along the inverse bearing, then path to it properly.
    // Pathing matters more here than for chase: the naive straight-line retreat
    // is what walks agents into the wall behind them.
    Vector3 flee_target = self + away * 10.f;

    AIControllerComponent* controller = blackboard.GetOwnerController();
    Vector3 destination = flee_target;
    if (controller != nullptr && controller->RequestPathTo(flee_target))
    {
      destination = controller->GetCurrentWaypoint();
    }

    AIMoveIntent intent;
    intent.has_move_target = true;
    intent.move_target = destination;
    intent.speed_ratio = blackboard.GetFloatOrDefault(kKeyMoveSpeedRatio, 1.f);
    intent.has_face_target = true;
    intent.face_target = target;
    blackboard.SetMoveIntent(intent);

    return BTNodeStatus::running;
  }

  BTNodeStatus ActionReturnToPost(AIBlackboard& blackboard, float delta_time)
  {
    (void)delta_time;
    const Vector3 home = ReadPosition(blackboard, kKeyHomePositionX, kKeyHomePositionY, kKeyHomePositionZ);

    AIControllerComponent* controller = blackboard.GetOwnerController();
    Vector3 destination = home;
    if (controller != nullptr && controller->RequestPathTo(home))
    {
      destination = controller->GetCurrentWaypoint();
    }

    AIMoveIntent intent;
    intent.has_move_target = true;
    intent.move_target = destination;
    intent.speed_ratio = blackboard.GetFloatOrDefault(kKeyMoveSpeedRatio, 1.f);
    blackboard.SetMoveIntent(intent);

    return BTNodeStatus::running;
  }
}  // namespace

template <typename TransferFunction>
void AIControllerComponent::Transfer(TransferFunction& transfer)
{
  Super::Transfer(transfer);
  transfer.Transfer(m_Profile, "profile");
  transfer.Transfer(m_MoveSpeedRatio, "move_speed_ratio");
  transfer.Transfer(m_AttackRange, "attack_range");
  transfer.Transfer(m_RetreatHealthRatio, "retreat_health_ratio");
  transfer.Transfer(m_GuardRadius, "guard_radius");
  transfer.Transfer(m_AttackActionName, "attack_action_name");
}

AIControllerComponent::~AIControllerComponent()
{
  delete m_Tree;
  m_Tree = nullptr;
}

void AIControllerComponent::PostLoadResource(GameObject* parent_object)
{
  m_ParentObject = parent_object;
  m_Blackboard.SetOwnerController(this);
  PrewarmBlackboard();
}

void AIControllerComponent::OnSerializedFieldsUpdated()
{
  // Profile or tuning changed in the Inspector: rebuild the built-in template.
  // A project-authored tree (profile == custom) is left alone.
  if (m_Profile != AIProfile::custom)
  {
    m_TreeDirty = true;
  }
  PrewarmBlackboard();
}

void AIControllerComponent::SetEnabled(bool enabled)
{
  const bool was_enabled = isEnabled();
  Component::SetEnabled(enabled);

  if (was_enabled && !enabled)
  {
    ReleaseMotor();
    m_Blackboard.SetMoveIntent(AIMoveIntent {});
    if (m_Tree != nullptr)
    {
      m_Tree->Reset();
    }
  }
}

void AIControllerComponent::Tick(float delta_time)
{
  if (!isEnabled())
  {
    return;
  }

  // AISystem is absent in the light runtime (TexPreview and friends). Missing
  // system means "no global policy", not "AI disabled", so we keep ticking.
  AISystem* ai_system = GET_SYSTEM(AISystem);
  if ((ai_system != nullptr) && !ai_system->IsAIEnabled())
  {
    return;
  }

  const std::chrono::steady_clock::time_point frame_start = std::chrono::steady_clock::now();

  RebuildTreeIfNeeded();

  // Advance the re-path timer with the real delta rather than assuming 60Hz;
  // the earlier fixed increment would re-path far too often on a slow frame.
  m_PathTimeSinceRefresh += delta_time;

  PrewarmBlackboard();
  PushPerceptionToBlackboard();

  // Walk the agent forward along its path before the tree publishes this
  // frame's intent, so GetCurrentWaypoint() already reflects progress.
  if (m_HasPath)
  {
    Vector3 self_position = Vector3::ZERO;
    if (m_ParentObject != nullptr)
    {
      if (const Transform* transform = m_ParentObject->tryGetComponentConst(Transform))
      {
        self_position = transform->GetPosition();
      }
    }
    UpdatePathFollow(self_position);
  }

  if (m_Tree != nullptr)
  {
    m_LastStatus = m_Tree->Tick(m_Blackboard, delta_time);
  }

  ApplyIntentToMotor(delta_time);

  if (ai_system != nullptr)
  {
    const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - frame_start;
    const float spent_ms = std::chrono::duration<float, std::milli>(elapsed).count();
    ai_system->ReportAgentSpend(spent_ms);
  }
}

void AIControllerComponent::SetTree(BehaviourTree* tree)
{
  delete m_Tree;
  m_Tree = tree;
  m_TreeDirty = false;
}

void AIControllerComponent::RebuildTreeIfNeeded()
{
  if (!m_TreeDirty)
  {
    return;
  }
  BuildDefaultTree();
}

void AIControllerComponent::BuildDefaultTree()
{
  delete m_Tree;
  m_Tree = new BehaviourTree();
  m_TreeDirty = false;

  switch (m_Profile)
  {
    case AIProfile::chase_and_retreat:
    {
      BTSelector* root = m_Tree->CreateNode<BTSelector>("root");

      // Priority 1: flee when hurt.
      BTSequence* retreat_branch = m_Tree->CreateNode<BTSequence>("retreat_branch");
      retreat_branch->AddChild(m_Tree->CreateNode<BTCondition>("is_hurt", &ConditionHealthBelowRetreat));
      retreat_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      retreat_branch->AddChild(m_Tree->CreateNode<BTAction>("retreat", &ActionRetreat));
      root->AddChild(retreat_branch);

      // Priority 2: attack when in range with line of sight.
      BTSequence* attack_branch = m_Tree->CreateNode<BTSequence>("attack_branch");
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_los", &ConditionHasLineOfSight));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("in_range", &ConditionTargetInAttackRange));
      attack_branch->AddChild(m_Tree->CreateNode<BTAction>("attack", &ActionAttack));
      root->AddChild(attack_branch);

      // Priority 3: chase.
      BTSequence* chase_branch = m_Tree->CreateNode<BTSequence>("chase_branch");
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_los", &ConditionHasLineOfSight));
      chase_branch->AddChild(m_Tree->CreateNode<BTAction>("chase", &ActionMoveToTarget));
      root->AddChild(chase_branch);

      // Fallback: idle.
      root->AddChild(m_Tree->CreateNode<BTAction>("idle", &ActionStopMoving));

      m_Tree->SetRoot(root);
      break;
    }

    case AIProfile::guard:
    {
      BTSelector* root = m_Tree->CreateNode<BTSelector>("root");

      BTSequence* attack_branch = m_Tree->CreateNode<BTSequence>("attack_branch");
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("in_guard_range", &ConditionTargetWithinGuardRadius));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_los", &ConditionHasLineOfSight));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("in_range", &ConditionTargetInAttackRange));
      attack_branch->AddChild(m_Tree->CreateNode<BTAction>("attack", &ActionAttack));
      root->AddChild(attack_branch);

      BTSequence* chase_branch = m_Tree->CreateNode<BTSequence>("chase_branch");
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("in_guard_range", &ConditionTargetWithinGuardRadius));
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_los", &ConditionHasLineOfSight));
      chase_branch->AddChild(m_Tree->CreateNode<BTAction>("chase", &ActionMoveToTarget));
      root->AddChild(chase_branch);

      // Return home when strayed.
      BTSequence* return_branch = m_Tree->CreateNode<BTSequence>("return_branch");
      return_branch->AddChild(m_Tree->CreateNode<BTCondition>("away_from_home", &ConditionAwayFromHome));
      return_branch->AddChild(m_Tree->CreateNode<BTAction>("return_to_post", &ActionReturnToPost));
      root->AddChild(return_branch);

      root->AddChild(m_Tree->CreateNode<BTAction>("idle", &ActionStopMoving));
      m_Tree->SetRoot(root);
      break;
    }

    case AIProfile::chase:
    default:
    {
      BTSelector* root = m_Tree->CreateNode<BTSelector>("root");

      BTSequence* attack_branch = m_Tree->CreateNode<BTSequence>("attack_branch");
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_los", &ConditionHasLineOfSight));
      attack_branch->AddChild(m_Tree->CreateNode<BTCondition>("in_range", &ConditionTargetInAttackRange));
      attack_branch->AddChild(m_Tree->CreateNode<BTAction>("attack", &ActionAttack));
      root->AddChild(attack_branch);

      BTSequence* chase_branch = m_Tree->CreateNode<BTSequence>("chase_branch");
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_target", &ConditionHasTarget));
      chase_branch->AddChild(m_Tree->CreateNode<BTCondition>("has_los", &ConditionHasLineOfSight));
      chase_branch->AddChild(m_Tree->CreateNode<BTAction>("chase", &ActionMoveToTarget));
      root->AddChild(chase_branch);

      root->AddChild(m_Tree->CreateNode<BTAction>("idle", &ActionStopMoving));
      m_Tree->SetRoot(root);
      break;
    }
  }
}

void AIControllerComponent::PushPerceptionToBlackboard()
{
  if (m_ParentObject == nullptr)
  {
    return;
  }

  AIPerceptionComponent* perception = m_ParentObject->tryGetComponent(AIPerceptionComponent);
  if (perception == nullptr)
  {
    return;
  }

  eastl::vector<AIPerceptionTarget> targets = perception->GetPerceivedTargets();
  m_Blackboard.SetPerceivedTargets(eastl::move(targets));
  m_Blackboard.RefreshPrimaryTarget();

  if (!m_Blackboard.HasPrimaryTarget())
  {
    return;
  }

  const GObjectID target_id = m_Blackboard.GetPrimaryTargetId();
  for (const AIPerceptionTarget& target : m_Blackboard.GetPerceivedTargets())
  {
    if (target.object_id != target_id)
    {
      continue;
    }
    m_Blackboard.SetFloat(kKeyTargetPositionX, target.position.x);
    m_Blackboard.SetFloat(kKeyTargetPositionY, target.position.y);
    m_Blackboard.SetFloat(kKeyTargetPositionZ, target.position.z);
    break;
  }
}

bool AIControllerComponent::RequestPathTo(const Vector3& goal)
{
  // Re-path only when the interval has elapsed AND the goal actually moved.
  // Without the distance check, a stationary goal still re-runs A* on every
  // interval expiry for no benefit.
  if (m_HasPath && m_PathTimeSinceRefresh < m_PathRefreshInterval)
  {
    return IsFollowingPath();
  }

  if (m_HasPath && m_HasLastPathGoal)
  {
    const float goal_move = m_LastPathGoal.squaredDistance(goal);
    if (goal_move < m_RepathDistance * m_RepathDistance)
    {
      return IsFollowingPath();
    }
  }

  NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
  if (navigation_system == nullptr || !navigation_system->IsReady() || !m_NavigationEnabled)
  {
    // No navmesh: callers fall back to the raw goal. This is a legitimate
    // state (level without a volume), not an error, so no log spam here.
    return false;
  }

  Vector3 self_position = Vector3::ZERO;
  if (m_ParentObject != nullptr)
  {
    if (const Transform* transform = m_ParentObject->tryGetComponentConst(Transform))
    {
      self_position = transform->GetPosition();
    }
  }

  const NavPath path = navigation_system->FindPath(self_position, goal);

  m_PathTimeSinceRefresh = 0.f;
  m_LastPathGoal = goal;
  m_HasLastPathGoal = true;

  if (!path.IsValid())
  {
    ++m_FailedPathCount;
    m_HasPath = false;
    m_Path.clear();

    // Explicit failure reporting: "no route" must be distinguishable from
    // "never asked", otherwise agents silently fall back to straight-line
    // movement and the bug looks like a perception problem.
    LOG_WARNING(ZGame, "AIController: path query failed (status %d) to (%.1f, %.1f, %.1f)",
                static_cast<int>(path.status), goal.x, goal.y, goal.z);
    return false;
  }

  m_Path = path.waypoints;
  m_PathCursor = 0;
  m_HasPath = true;
  return true;
}

void AIControllerComponent::ClearPath()
{
  m_Path.clear();
  m_PathCursor = 0;
  m_HasPath = false;
  m_HasLastPathGoal = false;
  m_PathTimeSinceRefresh = 0.f;
}

Vector3 AIControllerComponent::GetCurrentWaypoint() const
{
  if (!m_HasPath || m_PathCursor >= m_Path.size())
  {
    return m_HasLastPathGoal ? m_LastPathGoal : Vector3::ZERO;
  }
  return m_Path[m_PathCursor];
}

float AIControllerComponent::GetPathProgress() const
{
  if (!m_HasPath || m_Path.empty())
  {
    return 0.f;
  }
  return static_cast<float>(m_PathCursor) / static_cast<float>(m_Path.size());
}

void AIControllerComponent::UpdatePathFollow(const Vector3& self_position)
{
  if (!m_HasPath || m_PathCursor >= m_Path.size())
  {
    return;
  }

  // Advance past every waypoint already reached, not just the current one:
  // a fast agent can cross several cells in a single frame, and skipping only
  // one makes it backtrack to touch each intermediate point.
  const float reach_squared = m_WaypointReachRadius * m_WaypointReachRadius;
  while (m_PathCursor < m_Path.size() &&
         m_Path[m_PathCursor].squaredDistance(self_position) <= reach_squared)
  {
    ++m_PathCursor;
  }
}

void AIControllerComponent::ApplyIntentToMotor(float delta_time)
{
  if (!m_DriveMotor || m_ParentObject == nullptr)
  {
    return;
  }

  MotorComponent* motor = m_ParentObject->tryGetComponent(MotorComponent);
  if (motor == nullptr)
  {
    // No motor on this object: the agent is decision-only. That is a valid
    // setup (turrets, dialogue NPCs), so stay quiet rather than warning.
    return;
  }

  motor->ApplyAIMoveIntent(m_Blackboard.GetMoveIntent(), delta_time);
}

void AIControllerComponent::ReleaseMotor()
{
  if (m_ParentObject == nullptr)
  {
    return;
  }

  MotorComponent* motor = m_ParentObject->tryGetComponent(MotorComponent);
  if (motor != nullptr)
  {
    motor->ClearAIControl();
  }
}

void AIControllerComponent::PrewarmBlackboard()
{
  m_Blackboard.SetBool(kKeyHasTarget, m_Blackboard.HasPrimaryTarget());
  m_Blackboard.SetBool(kKeyHasLineOfSight, m_Blackboard.HasLineOfSightToTarget());
  m_Blackboard.SetFloat(kKeyDistanceToTarget, m_Blackboard.GetDistanceToTarget());

  m_Blackboard.SetFloat(kKeyAttackRange, m_AttackRange);
  m_Blackboard.SetFloat(kKeyMoveSpeedRatio, m_MoveSpeedRatio);
  m_Blackboard.SetFloat(kKeyHealthRatio, m_HealthRatio);
  m_Blackboard.SetFloat(kKeyRetreatHealthRatio, m_RetreatHealthRatio);
  m_Blackboard.SetFloat(kKeyGuardRadius, m_GuardRadius);
  m_Blackboard.SetString(kKeyAttackActionName, m_AttackActionName);

  if (m_ParentObject != nullptr)
  {
    const Transform* transform = m_ParentObject->tryGetComponentConst(Transform);
    if (transform != nullptr)
    {
      const Vector3 self_position = transform->GetPosition();
      m_Blackboard.SetFloat(kKeySelfPositionX, self_position.x);
      m_Blackboard.SetFloat(kKeySelfPositionY, self_position.y);
      m_Blackboard.SetFloat(kKeySelfPositionZ, self_position.z);

      // The guard post is wherever the agent stood when the level loaded.
      float home_x = 0.f;
      if (!m_Blackboard.TryGetFloat(kKeyHomePositionX, home_x))
      {
        m_Blackboard.SetFloat(kKeyHomePositionX, self_position.x);
        m_Blackboard.SetFloat(kKeyHomePositionY, self_position.y);
        m_Blackboard.SetFloat(kKeyHomePositionZ, self_position.z);
      }
    }
  }
}

AIActionRequest AIControllerComponent::TakeActionRequest()
{
  AIActionRequest request = m_Blackboard.GetActionRequest();
  m_Blackboard.ClearActionRequest();
  return request;
}

void AIControllerComponent::SetHealthRatio(float ratio)
{
  m_HealthRatio = ratio;
}
