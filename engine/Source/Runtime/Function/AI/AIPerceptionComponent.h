#pragma once

// Sight / hearing perception for one agent.
//
// Perception is the only AI subsystem allowed to query the physics scene, and
// it only ever reads: raycasts for line of sight plus a distance sweep over the
// level's objects. Results land on the agent's blackboard as
// AIPerceptionTarget entries; the decision layer never re-queries physics.

#include "Runtime/Core/Serialize/SerializeTraitsBase.h"
#include "Runtime/Function/AI/AITypes.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <type_traits>

enum class AIPerceptionMode : unsigned char
{
  /// Only objects whose name carries the matching tag are perceived.
  tag,
  /// Perceive every object in the level (expensive; for prototyping).
  all
};

template <>
class SerializeTraits<AIPerceptionMode> : public SerializeTraitsBase<AIPerceptionMode>
{
public:
  using value_type = AIPerceptionMode;

  inline static const char* GetTypeString(void* p = nullptr) { return "AIPerceptionMode"; }
  inline static bool AllowTransferOptimization() { return true; }

  template <typename TransferFunction>
  inline static void Transfer(value_type& data, TransferFunction& transfer)
  {
    using UnderlyingType = std::underlying_type_t<value_type>;
    transfer.TransferBasicData(reinterpret_cast<UnderlyingType&>(data));
  }
};

class AIPerceptionComponent : public Component
{
  REGISTER_CLASS(AIPerceptionComponent);
  DECLARE_OBJECT_SERIALIZE();

public:
  AIPerceptionComponent() = default;

  void PostLoadResource(GameObject* parent_object) override;
  void Tick(float delta_time) override;

  // ---- runtime access ------------------------------------------------------

  const eastl::vector<AIPerceptionTarget>& GetPerceivedTargets() const { return m_Targets; }
  bool HasAnyVisibleTarget() const { return m_HasAnyVisibleTarget; }

  /// Re-scan immediately instead of waiting for the next interval. Cheap enough
  /// to call from gameplay (e.g. after taking damage).
  void ForceRefresh();

private:
  void RefreshTargets();
  void CollectCandidates(eastl::vector<AIPerceptionTarget>& out_candidates) const;
  bool IsWithinVisionCone(const Transform* owner_transform,
                          const Vector3& owner_position,
                          const Vector3& candidate_position) const;
  bool TestLineOfSight(const Vector3& from_position, const Vector3& to_position) const;
  bool IsSelf(GObjectID object_id) const;

  // ---- serialized config ---------------------------------------------------

  AIPerceptionMode m_PerceptionMode {AIPerceptionMode::tag};
  eastl::string m_TargetTag;

  float m_SightRadius {20.f};
  /// Half-angle of the vision cone in degrees; 0 disables the cone test.
  float m_SightHalfAngleDegrees {0.f};
  float m_HearingRadius {8.f};
  /// Seconds between two perception scans. 0 scans every frame.
  float m_UpdateInterval {0.2f};

  // ---- runtime state -------------------------------------------------------

  eastl::vector<AIPerceptionTarget> m_Targets;
  float m_TimeSinceLastScan {0.f};
  bool m_HasAnyVisibleTarget {false};
  GObjectID m_OwnerId {k_invalid_gobject_id};
};
