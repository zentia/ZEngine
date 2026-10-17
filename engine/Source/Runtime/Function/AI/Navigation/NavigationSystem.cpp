#include "Runtime/Function/AI/Navigation/NavigationSystem.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/AI/AISystem.h"
#include "Runtime/Function/AI/Navigation/NavMesh.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/PhysicsScene.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawGroup.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"

#include <EASTL/vector.h>

#include <memory>
#include <vector>

namespace
{
  // Bridge between NavMesh's header-free raycast callback and the real
  // physics scene. The NavMesh takes a plain function pointer so it stays
  // unit testable; this is the only place that knows about PhysicsScene.
  //
  // A file-static is used because the callback signature has no user-data
  // slot. NavigationSystem is the only writer, and it sets the pointer
  // immediately before each build.
  PhysicsScene* g_build_physics_scene = nullptr;

  bool NavRaycastBridge(const Vector3& origin,
                        const Vector3& direction,
                        float length,
                        Vector3& out_hit_position)
  {
    if (g_build_physics_scene == nullptr)
    {
      return false;
    }

    std::vector<PhysicsHitInfo> hits;
    if (!g_build_physics_scene->Raycast(origin, direction, length, hits))
    {
      return false;
    }

    if (hits.empty())
    {
      return false;
    }

    // Raycast documents hits as sorted by distance; take the nearest.
    out_hit_position = hits.front().hit_position;
    return true;
  }
}  // namespace

std::vector<std::type_index> NavigationSystem::GetDependencies() const
{
  return {GET_SYSTEM_TYPE(AISystem)};
}

bool NavigationSystem::Initialize()
{
  LOG_INFO(ZGame, "NavigationSystem initialized");
  return true;
}

void NavigationSystem::Shutdown()
{
  ClearNavMesh();
}

int NavigationSystem::BuildFromVolume(const NavMeshBuildSettings& settings)
{
  return BuildFromActiveLevel(settings);
}

int NavigationSystem::BuildFromActiveLevel(const NavMeshBuildSettings& settings)
{
  std::shared_ptr<PhysicsScene> physics_scene;

  if (WorldManager* world_manager = GET_SYSTEM(WorldManager))
  {
    physics_scene = world_manager->GetCurrentActivePhysicsScene().lock();
  }

  if (physics_scene == nullptr)
  {
    // Not a build failure -- the level simply is not published as active yet
    // (see RequestDeferredBuild's comment). Remember and retry later.
    RequestDeferredBuild(settings);
    return 0;
  }

  g_build_physics_scene = physics_scene.get();
  const int walkable = m_NavMesh.Build(settings, &NavRaycastBridge);
  g_build_physics_scene = nullptr;

  if (walkable == 0)
  {
    LOG_WARNING(ZGame,
                "NavigationSystem: navmesh build found no walkable cells. "
                "Check the volume covers the floor and that geometry is static.");
  }

  return walkable;
}

void NavigationSystem::RequestDeferredBuild(const NavMeshBuildSettings& settings)
{
  m_DeferredSettings = settings;
  m_HasDeferredBuild = true;
  LOG_INFO(ZGame,
           "NavigationSystem: navmesh build deferred until the level finishes loading");
}

int NavigationSystem::FlushDeferredBuild()
{
  if (!m_HasDeferredBuild)
  {
    return -1;
  }

  const NavMeshBuildSettings settings = m_DeferredSettings;
  m_HasDeferredBuild = false;

  std::shared_ptr<PhysicsScene> physics_scene;
  if (WorldManager* world_manager = GET_SYSTEM(WorldManager))
  {
    physics_scene = world_manager->GetCurrentActivePhysicsScene().lock();
  }

  if (physics_scene == nullptr)
  {
    // Still not published: give up on this request rather than looping.
    // The next level load issues a fresh one from PostLoadResource.
    LOG_WARNING(ZGame,
                "NavigationSystem: deferred navmesh build still has no active "
                "physics scene; the level will have no navmesh");
    return 0;
  }

  g_build_physics_scene = physics_scene.get();
  const int walkable = m_NavMesh.Build(settings, &NavRaycastBridge);
  g_build_physics_scene = nullptr;

  if (walkable == 0)
  {
    LOG_WARNING(ZGame,
                "NavigationSystem: deferred navmesh build found no walkable cells. "
                "Check the volume covers the floor and that geometry is static.");
  }
  else
  {
    LOG_INFO(ZGame, "NavigationSystem: navmesh built ({} walkable cells)", walkable);
  }

  return walkable;
}

void NavigationSystem::ClearNavMesh()
{
  m_NavMesh.Clear();
}

NavPath NavigationSystem::FindPath(const Vector3& start,
                                    const Vector3& goal,
                                    bool smooth) const
{
  ++m_PathQueryCount;

  NavPath path = m_NavMesh.FindPath(start, goal);

  if (!path.IsValid())
  {
    ++m_FailedQueryCount;
    return path;
  }

  if (smooth && path.waypoints.size() > 2)
  {
    path.waypoints = m_NavMesh.SmoothPath(path.waypoints);
  }

  return path;
}

bool NavigationSystem::IsStandable(const Vector3& world_position) const
{
  return m_NavMesh.IsStandable(world_position);
}

bool NavigationSystem::FindNearestWalkablePoint(const Vector3& world_position,
                                                Vector3& out_position) const
{
  const int index = m_NavMesh.FindNearestWalkableCell(world_position);
  if (index < 0)
  {
    return false;
  }

  out_position = m_NavMesh.GetCells()[index].center;
  return true;
}

void NavigationSystem::ResetStats()
{
  m_PathQueryCount = 0;
  m_FailedQueryCount = 0;
}

void NavigationSystem::DrawDebug()
{
  if (!m_DebugDrawEnabled || !m_NavMesh.IsBuilt())
  {
    return;
  }

  DebugDrawManager* debug_draw_manager = GET_SYSTEM(DebugDrawManager);
  if (debug_draw_manager == nullptr)
  {
    return;
  }

  DebugDrawGroup* group = debug_draw_manager->TryGetOrCreateDebugDrawGroup("NavigationSystem");
  if (group == nullptr)
  {
    return;
  }

  const eastl::vector<NavCell>& cells = m_NavMesh.GetCells();
  const NavMeshBuildSettings& settings = m_NavMesh.GetSettings();

  const Vector4 walkable_color(0.f, 1.f, 0.f, 1.f);
  const Vector4 blocked_color(1.f, 0.f, 0.f, 0.35f);

  const float half_cell = settings.cell_size * 0.5f;
  const float lift = 0.05f;  // lift off the floor to avoid z-fighting

  for (size_t i = 0; i < cells.size(); ++i)
  {
    const NavCell& cell = cells[i];
    const Vector4& color = cell.walkable ? walkable_color : blocked_color;

    // Draw each cell as a wireframe quad on the walked surface. Up is +Z, so
    // the quad spans XY and the lift is applied along z.
    const Vector3 p0(cell.center.x - half_cell, cell.center.y - half_cell, cell.center.z + lift);
    const Vector3 p1(cell.center.x + half_cell, cell.center.y - half_cell, cell.center.z + lift);
    const Vector3 p2(cell.center.x + half_cell, cell.center.y + half_cell, cell.center.z + lift);
    const Vector3 p3(cell.center.x - half_cell, cell.center.y + half_cell, cell.center.z + lift);

    group->AddLine(p0, p1, color, color);
    group->AddLine(p1, p2, color, color);
    group->AddLine(p2, p3, color, color);
    group->AddLine(p3, p0, color, color);
  }
}
