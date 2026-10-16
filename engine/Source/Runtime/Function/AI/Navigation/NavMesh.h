#pragma once

// Navigation mesh: a walkable-surface graph the AI layer can query.
//
// Why engine-side navigation at all: Jolt (our physics backend) answers
// "does this shape collide" and "what does this ray hit", but it has no
// concept of a walkable surface or connectivity. Chasing a target in a
// straight line walks agents straight into walls -- which is exactly what
// phase 1's ActionChase/ActionRetreat do. This module supplies the two
// things they are missing:
//   1. IsStandable(world_position) -- can an agent occupy this point?
//   2. FindPath(start, end)        -- a connected polyline between them.
//
// Axis convention: this engine is Z-up. The grid lies in the XY plane and
// +Z is up, so every "height" in this module is a .z component. See the
// evidence listed in NavMesh.cpp -- building on the wrong axis silently
// produces a mesh in a vertical plane, which is why it is stated here and
// not left to be inferred from the member names.
//
// Representation: a grid of axis-aligned cells in XY, sampled by raycasting
// the level. A grid (rather than a true polygon soup) is chosen because it is
// trivially debuggable, cheap to rebuild, and adequate for phase 2; the query
// API below is deliberately shaped so a later polygon navmesh can replace the
// grid without touching callers.

#include "Runtime/Core/Math/Vector3.h"

#include <EASTL/vector.h>

/// World-space ray query used to sample the level.
///
/// Supplied by the caller so this class stays free of Jolt/PhysicsScene
/// headers and remains unit testable with a synthetic heightfield.
/// Returns true and fills out_hit_position when the ray hits geometry.
using NavRaycastFunc = bool (*)(const Vector3& origin,
                                const Vector3& direction,
                                float length,
                                Vector3& out_hit_position);

/// One sample in the navmesh grid.
struct NavCell
{
  /// World-space position on the walked surface.
  Vector3 center {Vector3::ZERO};
  bool walkable {false};
};

/// Build and query parameters for a navmesh grid.
///
/// All fields are authored rather than hard-coded, because the right cell
/// size depends entirely on the level.
struct NavMeshBuildSettings
{
  /// World-space centre of the sampled volume. .x/.y centre the grid on the
  /// horizontal plane; .z is the base height the downward probe drops from.
  Vector3 origin {Vector3::ZERO};
  /// Cell size in world units. Smaller = more accurate, more memory.
  float cell_size {1.f};
  /// Grid extent along X (horizontal).
  int cells_x {32};
  /// Grid extent along Y (horizontal). Z is up here, so the second
  /// horizontal axis is Y -- naming this "cells_z" would put the grid in a
  /// vertical plane.
  int cells_y {32};
  /// How far above the origin (along +Z) to start the downward probe.
  float probe_height {50.f};
  /// How far below the origin a surface may be and still count as floor.
  float probe_depth {100.f};
  /// Steepest walkable slope, as a rise-over-run ratio (0.5 = 1 unit of rise
  /// per 2 units of run). Checked against neighbouring cells, so no surface
  /// normals are needed.
  float max_slope {0.7f};
  /// A cell is rejected when a wall is within this horizontal distance, which
  /// stops agents hugging (and then clipping into) geometry.
  float agent_radius {0.4f};
  /// Clearance probes are cast at this height above the floor.
  float agent_height {0.9f};
  /// Maximum height difference between connected cells. Lets agents step up
  /// small ledges but not climb walls.
  float max_step_height {0.6f};
};

/// Result of a path query.
struct NavPath
{
  /// Waypoints in world space, from the start-adjacent point to the goal.
  /// Empty when the query failed -- status says why.
  eastl::vector<Vector3> waypoints;

  /// Distinguishing "no route exists" from "the query never ran" matters:
  /// callers must be able to log a real failure instead of silently falling
  /// back to straight-line movement and blaming the AI.
  enum class Status
  {
    success,
    no_navmesh,       // Build() has not been called or produced nothing.
    start_unwalkable, // Start point is off the mesh.
    goal_unwalkable,  // Goal point is off the mesh.
    unreachable,      // Both on the mesh, but no connected route.
  };

  Status status {Status::no_navmesh};

  bool IsValid() const { return status == Status::success && !waypoints.empty(); }
};

class NavMesh
{
public:
  NavMesh() = default;

  /// Sample the level over the configured volume and rebuild the grid.
  /// @return: number of walkable cells discovered (0 means the build found
  ///          nothing usable, which callers should treat as a real failure)
  int Build(const NavMeshBuildSettings& settings, NavRaycastFunc raycast);

  void Clear();

  const NavMeshBuildSettings& GetSettings() const { return m_Settings; }
  bool IsBuilt() const { return m_IsBuilt; }
  int GetWalkableCellCount() const { return m_WalkableCount; }

  /// Index of the cell containing a world position, or -1 when outside.
  int GetCellIndexAt(const Vector3& world_position) const;
  int GetCellIndex(int x, int y) const;

  /// Nearest walkable cell to a world position, or -1 when there is none.
  /// Used to snap agents that spawn slightly off the mesh.
  ///
  /// Matches on grid X/Y only by default. Prefer the overload that takes a
  /// height delta when the caller knows the agent's own height, so a query
  /// does not snap onto an elevated island (see IsStandable).
  int FindNearestWalkableCell(const Vector3& world_position) const;

  /// Overload that only accepts cells whose surface is within
  /// max_height_delta of world_position.z.
  int FindNearestWalkableCell(const Vector3& world_position, float max_height_delta) const;

  /// Is there walkable floor at exactly this world position, at that height?
  ///
  /// The height check is what keeps an agent standing on the floor from
  /// being treated as standing on a wall top that shares the same grid
  /// column: without it, a cell sampled on top of a wall (a legitimate
  /// walkable surface, but an island the agent cannot reach) would still
  /// answer "yes" to a floor-level query, and pathing would snap the agent
  /// onto the wall.
  bool IsStandable(const Vector3& world_position) const;

  /// A* over the grid. Snaps start and goal to the nearest walkable cell.
  NavPath FindPath(const Vector3& start, const Vector3& goal) const;

  /// Drop waypoints the agent can skip because nothing blocks the direct
  /// line between them (string pulling). Produces the natural diagonal cuts
  /// that raw grid paths lack.
  eastl::vector<Vector3> SmoothPath(const eastl::vector<Vector3>& raw_waypoints) const;

  /// Grid access for debug drawing.
  const eastl::vector<NavCell>& GetCells() const { return m_Cells; }

private:
  const NavCell* GetCell(int x, int y) const;
  bool IsWalkable(int x, int y) const;
  /// Can an agent move from one cell to an adjacent one?
  bool IsConnected(int from_index, int to_index, bool diagonal) const;
  /// Every sampled point between two positions is walkable?
  bool HasLineOfWalk(const Vector3& from, const Vector3& to) const;

  NavMeshBuildSettings m_Settings;
  eastl::vector<NavCell> m_Cells;
  int m_WalkableCount {0};
  bool m_IsBuilt {false};
};
