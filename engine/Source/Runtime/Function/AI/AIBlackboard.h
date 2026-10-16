#pragma once

// Per-agent scratchpad shared between perception, decision and execution.
//
// Design note: a blackboard is deliberately *not* serialized. It holds the
// current frame's world view plus a small set of value slots that BT nodes
// read/write. Everything in it must be reconstructible from the world, so
// saving an agent never depends on it.

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/AI/AITypes.h"

#include <EASTL/string.h>
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>

#include <cstdint>

class AIControllerComponent;

class AIBlackboard
{
public:
  /// The controller that owns this blackboard, so BT actions can reach
  /// per-agent services (currently path following) without going through a
  /// global. Null in headless tests, which is why every action must null-check.
  AIControllerComponent* GetOwnerController() const { return m_OwnerController; }
  void SetOwnerController(AIControllerComponent* controller) { m_OwnerController = controller; }

  void Clear()
  {
    m_FloatValues.clear();
    m_IntValues.clear();
    m_BoolValues.clear();
    m_StringValues.clear();
    m_PerceivedTargets.clear();
    m_MoveIntent = AIMoveIntent {};
    m_ActionRequest = AIActionRequest {};
    m_PrimaryTargetId = k_invalid_gobject_id;
    m_HasLineOfSightToTarget = false;
    m_DistanceToTarget = 0.f;
  }

  // ---- value slots --------------------------------------------------------

  void SetFloat(const eastl::string& key, float value) { m_FloatValues[key] = value; }
  bool TryGetFloat(const eastl::string& key, float& out_value) const
  {
    const auto it = m_FloatValues.find(key);
    if (it == m_FloatValues.end())
    {
      return false;
    }
    out_value = it->second;
    return true;
  }
  float GetFloatOrDefault(const eastl::string& key, float default_value) const
  {
    float value = default_value;
    TryGetFloat(key, value);
    return value;
  }

  void SetInt(const eastl::string& key, int32_t value) { m_IntValues[key] = value; }
  bool TryGetInt(const eastl::string& key, int32_t& out_value) const
  {
    const auto it = m_IntValues.find(key);
    if (it == m_IntValues.end())
    {
      return false;
    }
    out_value = it->second;
    return true;
  }

  void SetString(const eastl::string& key, eastl::string value) { m_StringValues[key] = eastl::move(value); }
  bool TryGetString(const eastl::string& key, eastl::string& out_value) const
  {
    const auto it = m_StringValues.find(key);
    if (it == m_StringValues.end())
    {
      return false;
    }
    out_value = it->second;
    return true;
  }
  eastl::string GetStringOrDefault(const eastl::string& key, const eastl::string& default_value) const
  {
    eastl::string value = default_value;
    TryGetString(key, value);
    return value;
  }

  void SetBool(const eastl::string& key, bool value) { m_BoolValues[key] = value; }
  bool TryGetBool(const eastl::string& key, bool& out_value) const
  {
    const auto it = m_BoolValues.find(key);
    if (it == m_BoolValues.end())
    {
      return false;
    }
    out_value = it->second;
    return true;
  }
  bool GetBoolOrDefault(const eastl::string& key, bool default_value) const
  {
    bool value = default_value;
    TryGetBool(key, value);
    return value;
  }

  // ---- perception ---------------------------------------------------------

  const eastl::vector<AIPerceptionTarget>& GetPerceivedTargets() const { return m_PerceivedTargets; }
  void SetPerceivedTargets(eastl::vector<AIPerceptionTarget> targets)
  {
    m_PerceivedTargets = eastl::move(targets);
  }

  /// Resolve the closest visible target and cache it in the fast-path fields
  /// below. Call once per frame after perception refreshed the list.
  void RefreshPrimaryTarget()
  {
    m_PrimaryTargetId = k_invalid_gobject_id;
    m_HasLineOfSightToTarget = false;
    m_DistanceToTarget = 0.f;

    float best_distance = 0.f;
    for (const AIPerceptionTarget& target : m_PerceivedTargets)
    {
      if (!target.is_visible)
      {
        continue;
      }
      if (m_PrimaryTargetId == k_invalid_gobject_id || target.distance < best_distance)
      {
        m_PrimaryTargetId = target.object_id;
        best_distance = target.distance;
        m_HasLineOfSightToTarget = true;
        m_DistanceToTarget = target.distance;
      }
    }
  }

  GObjectID GetPrimaryTargetId() const { return m_PrimaryTargetId; }
  bool HasPrimaryTarget() const { return m_PrimaryTargetId != k_invalid_gobject_id; }
  bool HasLineOfSightToTarget() const { return m_HasLineOfSightToTarget; }
  float GetDistanceToTarget() const { return m_DistanceToTarget; }

  // ---- output -------------------------------------------------------------

  const AIMoveIntent& GetMoveIntent() const { return m_MoveIntent; }
  void SetMoveIntent(const AIMoveIntent& intent) { m_MoveIntent = intent; }

  const AIActionRequest& GetActionRequest() const { return m_ActionRequest; }
  void SetActionRequest(const AIActionRequest& request) { m_ActionRequest = request; }
  void ClearActionRequest() { m_ActionRequest = AIActionRequest {}; }

private:
  eastl::unordered_map<eastl::string, float> m_FloatValues;
  eastl::unordered_map<eastl::string, int32_t> m_IntValues;
  eastl::unordered_map<eastl::string, bool> m_BoolValues;
  eastl::unordered_map<eastl::string, eastl::string> m_StringValues;

  eastl::vector<AIPerceptionTarget> m_PerceivedTargets;

  GObjectID m_PrimaryTargetId {k_invalid_gobject_id};
  bool m_HasLineOfSightToTarget {false};
  float m_DistanceToTarget {0.f};

  AIMoveIntent m_MoveIntent;
  AIActionRequest m_ActionRequest;

  /// Not a value slot: survives Clear() because it is wiring, not world state.
  AIControllerComponent* m_OwnerController {nullptr};
};
