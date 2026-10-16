#pragma once

// Shared lightweight value types for the ZEngine AI layer.
//
// Nothing in this header is serialized: blackboards, perception snapshots and
// action intents are all per-frame runtime state. Only the configuration
// structs declared next to the AI components (which the Inspector edits) take
// part in serialization, and they declare their SerializeTraits locally.

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/AI/AIMoveIntent.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

// ============================================================
// Behaviour tree
// ============================================================

/// Result of ticking a behaviour-tree node.
enum class BTNodeStatus : unsigned char
{
  invalid,  // never ticked this frame
  success,
  failure,
  running
};

/// Human-readable name of a node status, for logs and editor overlays.
inline const char* ToString(BTNodeStatus status)
{
  switch (status)
  {
    case BTNodeStatus::success:
      return "success";
    case BTNodeStatus::failure:
      return "failure";
    case BTNodeStatus::running:
      return "running";
    case BTNodeStatus::invalid:
    default:
      return "invalid";
  }
}

// ============================================================
// Perception
// ============================================================

/// Which sense produced a stimulus. Bitmask-friendly.
enum class AIPerceptionChannel : unsigned char
{
  none = 0,
  sight = 1u << 0,
  hearing = 1u << 1,
  damage = 1u << 2
};

inline bool HasChannel(unsigned char mask, AIPerceptionChannel channel)
{
  return (mask & static_cast<unsigned char>(channel)) != 0;
}

/// One entry of an agent's perception snapshot.
struct AIPerceptionTarget
{
  GObjectID object_id {k_invalid_gobject_id};
  Vector3 position {Vector3::ZERO};
  float distance {0.f};
  /// Seconds since this target was last confirmed visible.
  float last_seen_age {0.f};
  /// Whether the target is visible right now (line of sight cleared).
  bool is_visible {false};
  unsigned char channels {static_cast<unsigned char>(AIPerceptionChannel::none)};
};

// AIMoveIntent / AIActionRequest live in AIMoveIntent.h: MotorComponent needs
// them and must not depend on the rest of the AI layer. Re-exported here so
// existing AI-side includes of AITypes.h keep compiling.
// ============================================================
// Perception
// ============================================================
