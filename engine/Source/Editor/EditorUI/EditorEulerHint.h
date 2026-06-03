#pragma once

#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"

// Shared euler<->quaternion conversion for editor inspectors.
//
// The math (MakeQuaternionFromEulerDegrees) is byte-identical to the legacy
// InspectorWindow.cpp implementation so a given euler triple always maps to the
// same orientation across both the ImGui and ZSlate inspectors.
//
// The "hint" map keys a remembered euler triple by an opaque pointer (the
// TransformComponent address). It prevents the displayed degrees from jumping
// when a rotation can be expressed by multiple euler triples (gimbal aliasing):
// while the stored rotation still matches, GetEulerHint returns the last
// user-entered degrees instead of re-deriving a (possibly different) triple.
namespace EditorEuler
{
// Engine convention: degrees.x = pitch, degrees.y = roll, degrees.z = yaw.
Quaternion MakeQuaternionFromEulerDegrees(const Vector3& degrees);
Vector3 MakeEulerDegreesFromQuaternion(const Quaternion& rotation);

// Stable euler readout for `key`; re-derives only when the rotation changed.
Vector3 GetEulerHint(const void* key, const Quaternion& rotation);

// Record the euler triple the user just committed for `key`.
void SetEulerHint(const void* key, const Vector3& degrees, const Quaternion& rotation);
}  // namespace EditorEuler
