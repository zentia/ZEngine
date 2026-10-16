#pragma once

// The contract between the AI decision layer and the execution layer.
//
// This header is intentionally dependency-free (math types only) and lives
// outside the AI folder's heavier headers so MotorComponent can include it
// without pulling in behaviour trees, blackboards or perception. That is what
// lets the AI -> Motor seam be a *framework* seam: MotorComponent knows about
// a move intent, and nothing else about AI.

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <EASTL/string.h>

/// What the decision layer asks the execution layer to do.
///
/// Fields are plain values rather than an object reference on purpose: an
/// intent must be cheap to produce every frame and safe to hold across a tick.
struct AIMoveIntent
{
  /// When false the motor decelerates to a stop instead of seeking.
  bool has_move_target {false};
  /// World-space destination.
  Vector3 move_target {Vector3::ZERO};
  /// Normalised speed request in [0, 1]; the motor clamps it to its own
  /// m_MaxMoveSpeedRatio so a tree cannot exceed a pawn's authored limits.
  float speed_ratio {1.f};
  /// When false the motor keeps its current facing.
  bool has_face_target {false};
  Vector3 face_target {Vector3::ZERO};
};

/// Named request for the execution layer (ability or animation tag).
/// An empty action_name means "nothing to fire this tick".
struct AIActionRequest
{
  eastl::string action_name;
  GObjectID target_id {k_invalid_gobject_id};
};
