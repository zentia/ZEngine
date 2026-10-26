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

#include <cstdint>
#include <type_traits>
#include "AIPerceptionComponent.generated.h"

/// ZENUM: m_PerceptionMode is a ZPROPERTY of this type, and an enum that is not
/// a ZENUM is silently skipped when the field property table is built -- the
/// Inspector could switch tag/all but scripts could not. Marking it publishes
/// `perceptionMode: AIPerceptionMode` on the component.
ZENUM()
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

ZCLASS()
class AIPerceptionComponent : public Component
{
    ZGENERATED_BODY(AIPerceptionComponent)

public:
  AIPerceptionComponent() = default;

  void PostLoadResource(GameObject* parent_object) override;
  void Tick(float delta_time) override;

  // ---- runtime access ------------------------------------------------------

  /// 不导出：`eastl::vector<AIPerceptionTarget>` 没有 Converter，也没有"结构体
  /// 数组"这一层的 marshalling。脚本读感知请用下面那组 GetTargetXxx(index) ——
  /// 和 SpineRenderer 读动画事件是同一套路子（那边也是事件结构体数组）。
  const eastl::vector<AIPerceptionTarget>& GetPerceivedTargets() const { return m_Targets; }
  ZFUNCTION()
  bool HasAnyVisibleTarget() const { return m_HasAnyVisibleTarget; }

  /// Re-scan immediately instead of waiting for the next interval. Cheap enough
  /// to call from gameplay (e.g. after taking damage).
  ZFUNCTION()
  void ForceRefresh();

  // ---- 脚本读感知结果（下标访问）-------------------------------------------
  //
  // 下标越界一律返回缺省值（k_invalid_gobject_id / ZERO / 0 / false）而不是崩：
  // 感知列表每帧都变，脚本在 OnUpdate 里拿到的下标可能已经失效。宁可给一个能
  // 判断的哨兵，也不要让一个越界把进程带走。

  /// 本帧感知到的目标数量。
  ZFUNCTION()
  int32_t GetTargetCount() const { return static_cast<int32_t>(m_Targets.size()); }
  /// 没有目标时 = k_invalid_gobject_id（一个很大的数）。
  ZFUNCTION()
  GObjectID GetTargetObjectId(int32_t index) const
  {
    const AIPerceptionTarget* target = TargetAt(index);
    return target != nullptr ? target->object_id : k_invalid_gobject_id;
  }
  ZFUNCTION()
  Vector3 GetTargetPosition(int32_t index) const
  {
    const AIPerceptionTarget* target = TargetAt(index);
    return target != nullptr ? target->position : Vector3::ZERO;
  }
  ZFUNCTION()
  float GetTargetDistance(int32_t index) const
  {
    const AIPerceptionTarget* target = TargetAt(index);
    return target != nullptr ? target->distance : 0.f;
  }
  /// 距上次确认可见过了多少秒；0 = 此刻正可见。
  ZFUNCTION()
  float GetTargetLastSeenAge(int32_t index) const
  {
    const AIPerceptionTarget* target = TargetAt(index);
    return target != nullptr ? target->last_seen_age : 0.f;
  }
  /// 此刻是否有视线（sight 通道 + 射线没被挡）。
  ZFUNCTION()
  bool GetTargetIsVisible(int32_t index) const
  {
    const AIPerceptionTarget* target = TargetAt(index);
    return target != nullptr && target->is_visible;
  }
  /// 是否由听觉通道感知到（在听觉半径内但未必看得见）。
  ZFUNCTION()
  bool GetTargetIsHeard(int32_t index) const
  {
    const AIPerceptionTarget* target = TargetAt(index);
    return target != nullptr && HasChannel(target->channels, AIPerceptionChannel::hearing);
  }

private:
  /// 越界返回 nullptr；调用方负责给缺省值。不导出（是访问器，不是 API）。
  const AIPerceptionTarget* TargetAt(int32_t index) const
  {
    if (index < 0 || static_cast<size_t>(index) >= m_Targets.size())
    {
      return nullptr;
    }
    return &m_Targets[static_cast<size_t>(index)];
  }

  void RefreshTargets();
  void CollectCandidates(eastl::vector<AIPerceptionTarget>& out_candidates) const;
  bool IsWithinVisionCone(const Transform* owner_transform,
                          const Vector3& owner_position,
                          const Vector3& candidate_position) const;
  bool TestLineOfSight(const Vector3& from_position, const Vector3& to_position) const;
  bool IsSelf(GObjectID object_id) const;

  // ---- serialized config ---------------------------------------------------

  ZPROPERTY()
  AIPerceptionMode m_PerceptionMode {AIPerceptionMode::tag};
  ZPROPERTY()
  eastl::string m_TargetTag;

  ZPROPERTY(meta=(Category="Sight", ClampMin=0, EditCondition="m_SightEnabled"))
  float m_SightRadius {20.f};
  /// Half-angle of the vision cone in degrees; 0 disables the cone test.
  ZPROPERTY(meta=(Category="Sight", DisplayName="Sight Half Angle", Tooltip="Half-angle of the vision cone in degrees; 0 disables the cone test.", ClampMin=0, ClampMax=180, EditCondition="m_SightEnabled"))
  float m_SightHalfAngleDegrees {0.f};
  ZPROPERTY(meta=(Category="Hearing", ClampMin=0, EditCondition="m_HearingEnabled"))
  float m_HearingRadius {8.f};
  /// Seconds between two perception scans. 0 scans every frame.
  ZPROPERTY(meta=(Tooltip="Seconds between two perception scans; 0 scans every frame.", ClampMin=0))
  float m_UpdateInterval {0.2f};

  // Per-channel master switches. The Inspector greys out the parameters of a
  // disabled channel through meta=(EditCondition=...) above. Declared after the
  // values they gate, which is fine: the condition is resolved by name.
  ZPROPERTY(meta=(Tooltip="Run the vision-cone / line-of-sight test."))
  bool m_SightEnabled {true};
  ZPROPERTY(meta=(Tooltip="Report candidates inside the hearing radius."))
  bool m_HearingEnabled {true};

  // ---- runtime state -------------------------------------------------------

  eastl::vector<AIPerceptionTarget> m_Targets;
  float m_TimeSinceLastScan {0.f};
  bool m_HasAnyVisibleTarget {false};
  GObjectID m_OwnerId {k_invalid_gobject_id};
};
