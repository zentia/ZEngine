#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"

namespace LargeWorldCoordinates
{
/// UE-style render tile size (2^21 world units).
constexpr double k_render_tile_size = 2097152.0;

bool IsEnabled();
void SetEnabled(bool enabled);

Vector3d GetRenderTile();
void SetRenderTileFromWorldPosition(Vector3d world_position);

/// Absolute world -> float render space (relative to current tile).
Vector3 WorldToRender(Vector3d world_position);
Vector3d RenderToWorld(Vector3 render_position);

/// PreViewTranslation for the current tile (usually -offset within tile, as float).
Vector3 GetPreViewTranslation(Vector3d view_origin_world);

/// Rebases model matrix translation into render space; rotation/scale unchanged.
Matrix4x4 ApplyRenderOriginToModelMatrix(const Matrix4x4& world_matrix);
}  // namespace LargeWorldCoordinates
