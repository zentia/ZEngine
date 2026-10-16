#include "Runtime/Function/AI/AIPerceptionComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

IMPLEMENT_REGISTER_CLASS(AIPerceptionComponent)
IMPLEMENT_OBJECT_SERIALIZE(AIPerceptionComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AIPerceptionComponent)

template <typename TransferFunction>
void AIPerceptionComponent::Transfer(TransferFunction& transfer)
{
  Super::Transfer(transfer);
  transfer.Transfer(m_PerceptionMode, "perception_mode");
  transfer.Transfer(m_TargetTag, "target_tag");
  transfer.Transfer(m_SightRadius, "sight_radius");
  transfer.Transfer(m_SightHalfAngleDegrees, "sight_half_angle_degrees");
  transfer.Transfer(m_HearingRadius, "hearing_radius");
  transfer.Transfer(m_UpdateInterval, "update_interval");
}

void AIPerceptionComponent::PostLoadResource(GameObject* parent_object)
{
  m_ParentObject = parent_object;
  m_OwnerId = (m_ParentObject != nullptr) ? m_ParentObject->GetID() : k_invalid_gobject_id;

  if (m_ParentObject != nullptr && m_ParentObject->tryGetComponent(Transform) == nullptr)
  {
    LOG_ERROR(ZGame, "AI: perception component on object without a Transform");
  }
}

void AIPerceptionComponent::Tick(float delta_time)
{
  if (!isEnabled())
  {
    return;
  }

  if (m_UpdateInterval <= 0.f)
  {
    RefreshTargets();
    return;
  }

  m_TimeSinceLastScan += delta_time;
  if (m_TimeSinceLastScan >= m_UpdateInterval)
  {
    m_TimeSinceLastScan = 0.f;
    RefreshTargets();
  }
}

void AIPerceptionComponent::ForceRefresh()
{
  m_TimeSinceLastScan = 0.f;
  RefreshTargets();
}

void AIPerceptionComponent::RefreshTargets()
{
  eastl::vector<AIPerceptionTarget> candidates;
  CollectCandidates(candidates);

  m_Targets = eastl::move(candidates);

  m_HasAnyVisibleTarget = false;
  for (const AIPerceptionTarget& target : m_Targets)
  {
    if (target.is_visible)
    {
      m_HasAnyVisibleTarget = true;
      break;
    }
  }
}

void AIPerceptionComponent::CollectCandidates(eastl::vector<AIPerceptionTarget>& out_candidates) const
{
  if (m_ParentObject == nullptr)
  {
    return;
  }

  const Transform* owner_transform = m_ParentObject->tryGetComponentConst(Transform);
  if (owner_transform == nullptr)
  {
    return;
  }

  WorldManager* world_manager = GET_SYSTEM(WorldManager);
  if (world_manager == nullptr)
  {
    return;
  }

  Level* level = world_manager->getCurrentActiveLevel();
  if (level == nullptr)
  {
    return;
  }

  const Vector3 owner_position = owner_transform->GetPosition();

  for (const auto& [object_id, object] : level->getAllGObjects())
  {
    if (object == nullptr || IsSelf(object_id))
    {
      continue;
    }

    if (m_PerceptionMode == AIPerceptionMode::tag)
    {
      if (m_TargetTag.empty() || object->GetName().find(m_TargetTag) == eastl::string::npos)
      {
        continue;
      }
    }

    const Transform* candidate_transform = object->tryGetComponentConst(Transform);
    if (candidate_transform == nullptr)
    {
      continue;
    }

    const Vector3 candidate_position = candidate_transform->GetPosition();
    const float distance = owner_position.distance(candidate_position);

    AIPerceptionTarget target;
    target.object_id = object_id;
    target.position = candidate_position;
    target.distance = distance;

    bool perceived = false;

    if (distance <= m_SightRadius)
    {
      target.is_visible = IsWithinVisionCone(owner_transform, owner_position, candidate_position) &&
                          TestLineOfSight(owner_position, candidate_position);
      target.channels |= static_cast<unsigned char>(AIPerceptionChannel::sight);
      perceived = true;
    }

    if (distance <= m_HearingRadius)
    {
      target.channels |= static_cast<unsigned char>(AIPerceptionChannel::hearing);
      perceived = true;
    }

    if (perceived)
    {
      out_candidates.push_back(target);
    }
  }
}

bool AIPerceptionComponent::IsWithinVisionCone(const Transform* owner_transform,
                                               const Vector3& owner_position,
                                               const Vector3& candidate_position) const
{
  // Zero half-angle means "omniscient within radius": no directional test.
  if (m_SightHalfAngleDegrees <= 0.f)
  {
    return true;
  }

  Vector3 to_candidate = candidate_position - owner_position;
  if (to_candidate.squaredLength() <= 0.f)
  {
    return true;
  }
  to_candidate.normalise();

  // Forward is the owner's local -Y, matching ZEngine's camera convention.
  const Vector3 forward = (owner_transform->GetRotation() * Vector3::NEGATIVE_UNIT_Y).normalisedCopy();

  const float cos_to_candidate = forward.dotProduct(to_candidate);
  const float cos_limit = Math::cos(Math::DegreesToRadians(m_SightHalfAngleDegrees));
  return cos_to_candidate >= cos_limit;
}

bool AIPerceptionComponent::TestLineOfSight(const Vector3& from_position, const Vector3& to_position) const
{
  WorldManager* world_manager = GET_SYSTEM(WorldManager);
  if (world_manager == nullptr)
  {
    return false;
  }

  std::shared_ptr<PhysicsScene> physics_scene = world_manager->GetCurrentActivePhysicsScene().lock();
  if (physics_scene == nullptr)
  {
    // No physics scene (headless test, Web stub): treat sight as unobstructed
    // rather than reporting everyone blind -- a wrong "visible" is far easier
    // to spot in a test than a silently blind agent.
    return true;
  }

  const Vector3 delta = to_position - from_position;
  const float distance = delta.length();
  if (distance <= 0.f)
  {
    return true;
  }

  Vector3 direction = delta;
  direction.normalise();

  std::vector<PhysicsHitInfo> hits;
  physics_scene->Raycast(from_position, direction, distance, hits);
  return hits.empty();
}

bool AIPerceptionComponent::IsSelf(GObjectID object_id) const
{
  return object_id == m_OwnerId;
}
