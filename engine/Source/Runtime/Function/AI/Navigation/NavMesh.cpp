#include "Runtime/Function/AI/Navigation/NavMesh.h"

#include "Runtime/Core/Base/Macro.h"

#include <EASTL/vector.h>

#include <algorithm>
#include <cmath>

namespace
{
  constexpr int k_invalid_index = -1;

  // 8-connected grid over the horizontal plane.
  //
  // Axis convention: this engine is Z-up. The evidence is consistent across
  // the codebase and the shipped level data:
  //   - PhysicsConfig defaults gravity to (0, 0, -9.8), and 1-1.level.json
  //     authors (0, 0, -15). Down is -Z.
  //   - The floor object is a box of half extents (43, 24, 0.5) offset by
  //     z = -0.5: thin in Z, so it lies in the XY plane.
  //   - Walls are half extents (0.15, 5, 3.5) offset by z = 3.5: tall in Z.
  //   - MotorComponent zeroes .z to keep turns yaw-only, applies jump
  //     velocity along UNIT_Z, and clamps landing to the z-plane.
  // So the grid runs along X and Y, the floor probe drops along -Z, and
  // every height comparison below reads .z -- never .y. Building this the
  // other way round puts the mesh in a vertical plane where the probe shoots
  // sideways into walls, and every query silently returns nonsense.
  constexpr int k_neighbour_dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  constexpr int k_neighbour_dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  constexpr int k_diagonal_begin = 4;
  constexpr float k_sqrt_two = 1.41421356f;

  struct AStarNode
  {
    int parent_index {k_invalid_index};
    float g_score {0.f};  // cost from start
    float f_score {0.f};  // g + heuristic
    bool closed {false};
    bool opened {false};
  };

  float Heuristic(int ax, int ay, int bx, int by)
  {
    // Octile distance: matches the 8-connected movement costs, so the
    // heuristic never overestimates and A* stays admissible.
    const int dx = std::abs(ax - bx);
    const int dy = std::abs(ay - by);
    const int min_d = std::min(dx, dy);
    const int max_d = std::max(dx, dy);
    return static_cast<float>(max_d - min_d) + k_sqrt_two * static_cast<float>(min_d);
  }
}  // namespace

int NavMesh::GetCellIndex(int x, int y) const
{
  if (x < 0 || y < 0 || x >= m_Settings.cells_x || y >= m_Settings.cells_y)
  {
    return k_invalid_index;
  }
  return y * m_Settings.cells_x + x;
}

int NavMesh::GetCellIndexAt(const Vector3& world_position) const
{
  if (!m_IsBuilt)
  {
    return k_invalid_index;
  }

  const float half_extent_x = m_Settings.cells_x * m_Settings.cell_size * 0.5f;
  const float half_extent_y = m_Settings.cells_y * m_Settings.cell_size * 0.5f;

  const float local_x = world_position.x - (m_Settings.origin.x - half_extent_x);
  const float local_y = world_position.y - (m_Settings.origin.y - half_extent_y);

  if (local_x < 0.f || local_y < 0.f)
  {
    return k_invalid_index;
  }

  const int x = static_cast<int>(std::floor(local_x / m_Settings.cell_size));
  const int y = static_cast<int>(std::floor(local_y / m_Settings.cell_size));

  return GetCellIndex(x, y);
}

const NavCell* NavMesh::GetCell(int x, int y) const
{
  const int index = GetCellIndex(x, y);
  if (index == k_invalid_index)
  {
    return nullptr;
  }
  return &m_Cells[index];
}

bool NavMesh::IsWalkable(int x, int y) const
{
  const NavCell* cell = GetCell(x, y);
  return cell != nullptr && cell->walkable;
}

bool NavMesh::IsConnected(int from_index, int to_index, bool diagonal) const
{
  const NavCell& from_cell = m_Cells[from_index];
  const NavCell& to_cell = m_Cells[to_index];

  if (!from_cell.walkable || !to_cell.walkable)
  {
    return false;
  }

  // Height is .z: Z is up.
  const float delta_z = std::fabs(to_cell.center.z - from_cell.center.z);
  if (delta_z > m_Settings.max_step_height)
  {
    return false;
  }

  // Slope as rise-over-run against the cell size. Using neighbouring heights
  // rather than surface normals keeps this independent of how the raycast
  // backend reports normals.
  const float run = diagonal ? m_Settings.cell_size * k_sqrt_two : m_Settings.cell_size;
  if (run > 0.f && (delta_z / run) > m_Settings.max_slope)
  {
    return false;
  }

  return true;
}

int NavMesh::Build(const NavMeshBuildSettings& settings, NavRaycastFunc raycast)
{
  Clear();

  if (settings.cells_x <= 0 || settings.cells_y <= 0 || settings.cell_size <= 0.f)
  {
    LOG_ERROR(ZGame, "NavMesh::Build: invalid grid settings (%dx%d, size %.2f)",
              settings.cells_x, settings.cells_y, settings.cell_size);
    return 0;
  }

  if (raycast == nullptr)
  {
    LOG_ERROR(ZGame, "NavMesh::Build: no raycast callback supplied");
    return 0;
  }

  m_Settings = settings;
  m_Cells.resize(static_cast<size_t>(settings.cells_x) * static_cast<size_t>(settings.cells_y));

  const float half_extent_x = settings.cells_x * settings.cell_size * 0.5f;
  const float half_extent_y = settings.cells_y * settings.cell_size * 0.5f;
  const float probe_length = settings.probe_height + settings.probe_depth;

  int walkable_count = 0;

  for (int y = 0; y < settings.cells_y; ++y)
  {
    for (int x = 0; x < settings.cells_x; ++x)
    {
      const int index = GetCellIndex(x, y);
      NavCell& cell = m_Cells[index];

      const float world_x = settings.origin.x - half_extent_x + (x + 0.5f) * settings.cell_size;
      const float world_y = settings.origin.y - half_extent_y + (y + 0.5f) * settings.cell_size;
      const float world_z = settings.origin.z + settings.probe_height;

      const Vector3 probe_origin(world_x, world_y, world_z);
      Vector3 floor_hit;

      // Down is -Z here; see the axis note at the top of this file.
      if (!raycast(probe_origin, Vector3(0.f, 0.f, -1.f), probe_length, floor_hit))
      {
        // No floor under this cell.
        cell.walkable = false;
        cell.center = Vector3(world_x, world_y, settings.origin.z);
        continue;
      }

      // Clearance: cast horizontally (in XY) from chest height in four
      // directions. A hit within agent_radius means a wall is too close for
      // an agent to stand here. Casting sideways (rather than down) is what
      // makes this a wall test instead of another floor test.
      bool blocked = false;
      for (int i = 0; i < 4 && !blocked; ++i)
      {
        const Vector3 direction(k_neighbour_dx[i], k_neighbour_dy[i], 0.f);
        const Vector3 clearance_origin(floor_hit.x,
                                      floor_hit.y,
                                      floor_hit.z + settings.agent_height);
        Vector3 wall_hit;
        if (raycast(clearance_origin, direction, settings.agent_radius, wall_hit))
        {
          blocked = true;
        }
      }

      cell.walkable = !blocked;
      cell.center = floor_hit;
      if (cell.walkable)
      {
        ++walkable_count;
      }
    }
  }

  m_WalkableCount = walkable_count;
  m_IsBuilt = true;

  LOG_INFO(ZGame, "NavMesh::Build: {}/{} walkable cells", walkable_count,
           settings.cells_x * settings.cells_y);

  return walkable_count;
}

void NavMesh::Clear()
{
  m_Cells.clear();
  m_WalkableCount = 0;
  m_IsBuilt = false;
}

int NavMesh::FindNearestWalkableCell(const Vector3& world_position) const
{
  return FindNearestWalkableCell(world_position, m_Settings.max_step_height);
}

int NavMesh::FindNearestWalkableCell(const Vector3& world_position, float max_height_delta) const
{
  if (!m_IsBuilt || m_Cells.empty())
  {
    return k_invalid_index;
  }

  const float half_extent_x = m_Settings.cells_x * m_Settings.cell_size * 0.5f;
  const float half_extent_y = m_Settings.cells_y * m_Settings.cell_size * 0.5f;

  const int center_x = static_cast<int>(
      std::floor((world_position.x - (m_Settings.origin.x - half_extent_x)) / m_Settings.cell_size));
  const int center_y = static_cast<int>(
      std::floor((world_position.y - (m_Settings.origin.y - half_extent_y)) / m_Settings.cell_size));

  const int max_radius = std::max(m_Settings.cells_x, m_Settings.cells_y);
  for (int radius = 0; radius <= max_radius; ++radius)
  {
    for (int dy = -radius; dy <= radius; ++dy)
    {
      for (int dx = -radius; dx <= radius; ++dx)
      {
        // Test only the ring: inner cells were covered by earlier iterations.
        if (radius > 0 && std::abs(dx) != radius && std::abs(dy) != radius)
        {
          continue;
        }

        const int candidate = GetCellIndex(center_x + dx, center_y + dy);
        if (candidate == k_invalid_index || !m_Cells[candidate].walkable)
        {
          continue;
        }

        // Reject surfaces too far above or below the queried height, so an
        // elevated island is never chosen for a ground-level query.
        if (std::fabs(m_Cells[candidate].center.z - world_position.z) > max_height_delta)
        {
          continue;
        }

        return candidate;
      }
    }
  }

  return k_invalid_index;
}

bool NavMesh::IsStandable(const Vector3& world_position) const
{
  // Exactly which cell the position falls in -- not the nearest walkable one.
  // Searching outward would make every position "standable" as long as any
  // cell anywhere is walkable, which would let agents path through walls.
  const int index = GetCellIndexAt(world_position);
  if (index == k_invalid_index)
  {
    return false;
  }

  const NavCell& cell = m_Cells[index];
  if (!cell.walkable)
  {
    return false;
  }

  // Height must match too: a wall top in the same grid column is walkable
  // geometry but is not the floor this query is asking about.
  if (std::fabs(cell.center.z - world_position.z) > m_Settings.max_step_height)
  {
    return false;
  }

  return true;
}

NavPath NavMesh::FindPath(const Vector3& start, const Vector3& goal) const
{
  NavPath result;

  if (!m_IsBuilt || m_Cells.empty())
  {
    result.status = NavPath::Status::no_navmesh;
    return result;
  }

  const int start_index = FindNearestWalkableCell(start);
  const int goal_index = FindNearestWalkableCell(goal);

  if (start_index == k_invalid_index)
  {
    result.status = NavPath::Status::start_unwalkable;
    return result;
  }
  if (goal_index == k_invalid_index)
  {
    result.status = NavPath::Status::goal_unwalkable;
    return result;
  }

  if (start_index == goal_index)
  {
    // Already there: one waypoint lets the motor drive straight in.
    result.status = NavPath::Status::success;
    result.waypoints.push_back(goal);
    return result;
  }

  const int cell_count = static_cast<int>(m_Cells.size());
  eastl::vector<AStarNode> nodes(cell_count);

  const int goal_x = goal_index % m_Settings.cells_x;
  const int goal_y = goal_index / m_Settings.cells_x;

  nodes[start_index].g_score = 0.f;
  nodes[start_index].f_score = Heuristic(start_index % m_Settings.cells_x,
                                        start_index / m_Settings.cells_x, goal_x, goal_y);
  nodes[start_index].opened = true;
  nodes[start_index].parent_index = k_invalid_index;

  while (true)
  {
    int current = k_invalid_index;
    float best_f = 0.f;
    for (int i = 0; i < cell_count; ++i)
    {
      if (nodes[i].opened && !nodes[i].closed)
      {
        if (current == k_invalid_index || nodes[i].f_score < best_f)
        {
          current = i;
          best_f = nodes[i].f_score;
        }
      }
    }

    if (current == k_invalid_index)
    {
      // Open list exhausted without reaching the goal.
      result.status = NavPath::Status::unreachable;
      return result;
    }

    if (current == goal_index)
    {
      break;
    }

    nodes[current].closed = true;

    const int current_x = current % m_Settings.cells_x;
    const int current_y = current / m_Settings.cells_x;

    for (int i = 0; i < 8; ++i)
    {
      const int nx = current_x + k_neighbour_dx[i];
      const int ny = current_y + k_neighbour_dy[i];
      const int neighbour_index = GetCellIndex(nx, ny);

      if (neighbour_index == k_invalid_index || nodes[neighbour_index].closed)
      {
        continue;
      }

      const bool diagonal = (i >= k_diagonal_begin);

      // Block diagonals that would cut between two blocked orthogonals,
      // otherwise agents squeeze through wall corners.
      if (diagonal)
      {
        if (!IsWalkable(current_x + k_neighbour_dx[i], current_y) ||
            !IsWalkable(current_x, current_y + k_neighbour_dy[i]))
        {
          continue;
        }
      }

      if (!IsConnected(current, neighbour_index, diagonal))
      {
        continue;
      }

      const float step_cost = diagonal ? k_sqrt_two : 1.f;
      const float tentative_g = nodes[current].g_score + step_cost;

      if (!nodes[neighbour_index].opened || tentative_g < nodes[neighbour_index].g_score)
      {
        nodes[neighbour_index].parent_index = current;
        nodes[neighbour_index].g_score = tentative_g;
        nodes[neighbour_index].f_score = tentative_g + Heuristic(nx, ny, goal_x, goal_y);
        nodes[neighbour_index].opened = true;
      }
    }
  }

  // Walk the parent chain back from the goal.
  eastl::vector<int> chain;
  int cursor = goal_index;
  while (cursor != k_invalid_index)
  {
    chain.push_back(cursor);
    if (cursor == start_index)
    {
      break;
    }
    cursor = nodes[cursor].parent_index;
  }

  if (chain.empty() || chain.back() != start_index)
  {
    result.status = NavPath::Status::unreachable;
    return result;
  }

  // Reverse into start -> goal order and drop the start cell: the agent is
  // already standing on it, and including it makes the motor back up first.
  for (size_t i = chain.size(); i-- > 1;)
  {
    result.waypoints.push_back(m_Cells[chain[i]].center);
  }

  // End on the exact goal so agents arrive on the target rather than on the
  // nearest cell centre.
  if (!result.waypoints.empty())
  {
    result.waypoints.back() = goal;
  }

  result.status = NavPath::Status::success;
  return result;
}

bool NavMesh::HasLineOfWalk(const Vector3& from, const Vector3& to) const
{
  Vector3 delta = to - from;
  const float distance = delta.length();
  if (distance <= m_Settings.cell_size * 0.5f)
  {
    return true;
  }

  const int step_count = static_cast<int>(std::ceil(distance / (m_Settings.cell_size * 0.5f)));
  for (int i = 1; i <= step_count; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(step_count);
    const Vector3 sample = from + delta * t;
    if (!IsStandable(sample))
    {
      return false;
    }
  }

  return true;
}

eastl::vector<Vector3> NavMesh::SmoothPath(const eastl::vector<Vector3>& raw_waypoints) const
{
  eastl::vector<Vector3> smoothed;

  if (raw_waypoints.size() <= 2)
  {
    return raw_waypoints;
  }

  // String pulling: keep the furthest waypoint still reachable by a straight
  // walkable line, then resume from there.
  size_t anchor = 0;
  smoothed.push_back(raw_waypoints[anchor]);

  while (anchor < raw_waypoints.size() - 1)
  {
    size_t furthest = anchor + 1;
    for (size_t candidate = raw_waypoints.size() - 1; candidate > anchor + 1; --candidate)
    {
      if (HasLineOfWalk(raw_waypoints[anchor], raw_waypoints[candidate]))
      {
        furthest = candidate;
        break;
      }
    }

    smoothed.push_back(raw_waypoints[furthest]);
    anchor = furthest;
  }

  return smoothed;
}
