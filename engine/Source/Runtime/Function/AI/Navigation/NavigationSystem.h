#pragma once

// Owns the navigation mesh for the active level and serves path queries.
//
// Registered as an engine system (like AISystem) so there is one obvious
// place to rebuild the mesh when a level loads and one place agents ask for
// paths, instead of every agent owning its own copy of the grid.

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/AI/Navigation/NavMesh.h"

#include <EASTL/vector.h>

class NavMeshVolumeComponent;

class NavigationSystem : public IEngineSystem
{
public:
  std::string GetName() const override { return "NavigationSystem"; }
  SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }
  std::vector<std::type_index> GetDependencies() const override;

  bool Initialize() override;
  void Shutdown() override;

  /// Rebuild from a volume component's authored settings.
  /// @return: walkable cell count; 0 means the build produced nothing usable
  int BuildFromVolume(const NavMeshBuildSettings& settings);

  /// Rebuild by sampling the currently active level's physics scene.
  /// Convenience path for levels without an explicit volume.
  int BuildFromActiveLevel(const NavMeshBuildSettings& settings);

  void ClearNavMesh();

  bool IsReady() const { return m_NavMesh.IsBuilt(); }
  const NavMesh& GetNavMesh() const { return m_NavMesh; }

  /// Smoothed path between two world points. Returns the raw status when the
  /// query fails so callers can log a real reason.
  NavPath FindPath(const Vector3& start, const Vector3& goal, bool smooth = true) const;

  bool IsStandable(const Vector3& world_position) const;

  /// Nearest point on the mesh, for snapping agents that spawn off-grid.
  /// Returns false when there is no walkable cell at all.
  bool FindNearestWalkablePoint(const Vector3& world_position, Vector3& out_position) const;

  // ---- stats ---------------------------------------------------------------

  int GetWalkableCellCount() const { return m_NavMesh.GetWalkableCellCount(); }
  uint32_t GetPathQueryCount() const { return m_PathQueryCount; }
  uint32_t GetFailedQueryCount() const { return m_FailedQueryCount; }
  void ResetStats();

  /// Toggle debug rendering of the mesh and active paths.
  void SetDebugDrawEnabled(bool enabled) { m_DebugDrawEnabled = enabled; }
  bool IsDebugDrawEnabled() const { return m_DebugDrawEnabled; }

  /// Called each frame to push the mesh into the debug draw group.
  void DrawDebug();

private:
  NavMesh m_NavMesh;

  // Path stats are logical const: a query mutates counters, not the mesh.
  mutable uint32_t m_PathQueryCount {0};
  mutable uint32_t m_FailedQueryCount {0};

  bool m_DebugDrawEnabled {false};
};
