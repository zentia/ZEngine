// Headless test for the navigation grid: build, standability, pathing and
// smoothing on a synthetic Z-up level.
//
// Why this exists separately from AISmokeTest: AISmokeTest covers the
// decision layer (blackboard + behaviour tree) and never touches NavMesh, so
// it passed cleanly while the whole grid was built in the wrong plane. This
// TU exercises the geometry itself, which is what actually catches that
// class of bug.
//
// The synthetic raycaster stands in for PhysicsScene the same way
// NavigationSystem's bridge does in the real engine: NavMesh takes a plain
// function pointer precisely so it can be driven headless like this.

#include "Runtime/Function/AI/Navigation/NavMesh.h"

#include <cmath>
#include <cstdio>
#include <vector>

#ifdef ZENGINE_TEST_LEVEL_PATH
#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <map>
#include <string>
#endif

namespace
{
  int g_failures = 0;

  void Check(bool condition, const char* what)
  {
    if (condition)
    {
      std::printf("[ ok ] %s\n", what);
      std::fflush(stdout);
      return;
    }
    std::printf("[FAIL] %s\n", what);
    std::fflush(stdout);
    ++g_failures;
  }

  // ---- synthetic Z-up level -------------------------------------------------

  // A floor slab in the XY plane at z = 0, plus axis-aligned wall boxes.
  // Z is up, so the floor is thin in Z and walls are tall in Z -- matching
  // how 1-1.level.json actually authors its geometry (floor half extents
  // (43, 24, 0.5); walls (0.15, 5, 3.5) offset to sit on the floor).
  struct Box
  {
    float min_x {0.f};
    float max_x {0.f};
    float min_y {0.f};
    float max_y {0.f};
    float min_z {0.f};
    float max_z {0.f};
  };

  std::vector<Box> g_boxes;
  // Walls only, kept separately from g_boxes so containment checks do not
  // count the floor slab (which spans z in [-1, 0] and therefore "contains"
  // every waypoint sitting at z = 0).
  std::vector<Box> g_walls;

  bool RayBoxHit(const Vector3& origin,
                 const Vector3& direction,
                 float length,
                 const Box& box,
                 float& out_t)
  {
    // Slab method. Returns the nearest positive hit inside [0, length].
    float t_min = 0.f;
    float t_max = length;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {direction.x, direction.y, direction.z};
    const float b_min[3] = {box.min_x, box.min_y, box.min_z};
    const float b_max[3] = {box.max_x, box.max_y, box.max_z};

    for (int axis = 0; axis < 3; ++axis)
    {
      if (std::fabs(d[axis]) < 1e-8f)
      {
        if (o[axis] < b_min[axis] || o[axis] > b_max[axis])
        {
          return false;
        }
        continue;
      }
      const float inv = 1.f / d[axis];
      float t1 = (b_min[axis] - o[axis]) * inv;
      float t2 = (b_max[axis] - o[axis]) * inv;
      if (t1 > t2)
      {
        const float swap = t1;
        t1 = t2;
        t2 = swap;
      }
      if (t1 > t_min)
      {
        t_min = t1;
      }
      if (t2 < t_max)
      {
        t_max = t2;
      }
      if (t_min > t_max)
      {
        return false;
      }
    }

    out_t = t_min;
    return true;
  }

  bool SyntheticRaycast(const Vector3& origin,
                        const Vector3& direction,
                        float length,
                        Vector3& out_hit_position)
  {
    float best_t = length;
    bool found = false;
    for (const Box& box : g_boxes)
    {
      float t = 0.f;
      if (RayBoxHit(origin, direction, length, box, t) && t < best_t)
      {
        best_t = t;
        found = true;
      }
    }
    if (!found)
    {
      return false;
    }
    out_hit_position = origin + direction * best_t;
    return true;
  }

  Box MakeBox(float cx, float cy, float hx, float hy, float min_z, float max_z)
  {
    Box box;
    box.min_x = cx - hx;
    box.max_x = cx + hx;
    box.min_y = cy - hy;
    box.max_y = cy + hy;
    box.min_z = min_z;
    box.max_z = max_z;
    return box;
  }

  void ResetLevel()
  {
    g_boxes.clear();
    g_walls.clear();
    // Floor slab: 40x40 centred on the origin, top surface at z = 0.
    g_boxes.push_back(MakeBox(0.f, 0.f, 20.f, 20.f, -1.f, 0.f));
  }

  void AddWall(float cx, float cy, float hx, float hy)
  {
    const Box wall = MakeBox(cx, cy, hx, hy, 0.f, 3.f);
    g_boxes.push_back(wall);
    g_walls.push_back(wall);
  }

  NavMeshBuildSettings FlatSettings()
  {
    NavMeshBuildSettings settings;
    settings.origin = Vector3(0.f, 0.f, 0.f);
    settings.cell_size = 1.f;
    settings.cells_x = 20;
    settings.cells_y = 20;
    settings.probe_height = 10.f;
    settings.probe_depth = 20.f;
    settings.agent_radius = 0.4f;
    settings.agent_height = 0.9f;
    settings.max_step_height = 0.6f;
    settings.max_slope = 0.7f;
    return settings;
  }
}  // namespace

int main()
{
  // ---- axis convention: the grid must lie in XY, not XZ --------------------
  //
  // This is the regression guard for the bug this test was written to catch.
  // On a flat floor at z = 0 the correct build finds walkable floor under
  // every cell; a build that probes along -Y instead of -Z (or lays the grid
  // in XZ) finds nothing, because the floor slab is thin in Z.
  {
    ResetLevel();
    NavMesh mesh;
    const int walkable = mesh.Build(FlatSettings(), &SyntheticRaycast);
    Check(walkable == 400, "flat 20x20 floor yields 400 walkable cells");

    // Sampled positions must sit on the floor surface: z == 0, and both .x
    // and .y must vary across the grid (a grid accidentally built in XZ
    // would leave every .y identical).
    const eastl::vector<NavCell>& cells = mesh.GetCells();
    bool all_on_floor = true;
    bool y_varies = false;
    bool x_varies = false;
    float first_x = cells[0].center.x;
    float first_y = cells[0].center.y;
    for (const NavCell& cell : cells)
    {
      if (std::fabs(cell.center.z) > 0.001f)
      {
        all_on_floor = false;
      }
      if (std::fabs(cell.center.y - first_y) > 0.5f)
      {
        y_varies = true;
      }
      if (std::fabs(cell.center.x - first_x) > 0.5f)
      {
        x_varies = true;
      }
    }
    Check(all_on_floor, "sample heights land on the floor (z == 0)");
    Check(x_varies, "grid varies along X");
    Check(y_varies, "grid varies along Y (not collapsed into XZ)");

    Check(mesh.IsStandable(Vector3(0.f, 0.f, 0.f)), "origin is standable");
    Check(mesh.IsStandable(Vector3(-9.f, 7.f, 0.f)), "off-centre point is standable");

    // Outside the volume there is no mesh, not a walkable default.
    Check(!mesh.IsStandable(Vector3(100.f, 100.f, 0.f)), "point outside the volume is not standable");
  }

  // ---- walls block clearance -----------------------------------------------
  {
    ResetLevel();
    // A wall through the middle of the grid, along Y, at x = 0.
    AddWall(0.f, 0.f, 0.2f, 10.f);

    NavMesh mesh;
    const int walkable = mesh.Build(FlatSettings(), &SyntheticRaycast);
    Check(walkable > 0 && walkable < 400, "wall removes some walkable cells");
    Check(!mesh.IsStandable(Vector3(0.f, 0.f, 0.f)), "cell inside the wall is not standable");
    Check(mesh.IsStandable(Vector3(-5.f, 0.f, 0.f)), "floor clear of the wall stays standable");
  }

  // ---- pathing across open ground -------------------------------------------
  {
    ResetLevel();
    NavMesh mesh;
    mesh.Build(FlatSettings(), &SyntheticRaycast);

    const NavPath path = mesh.FindPath(Vector3(-8.f, -8.f, 0.f), Vector3(8.f, 8.f, 0.f));
    Check(path.status == NavPath::Status::success, "open-ground path succeeds");
    Check(!path.waypoints.empty(), "open-ground path has waypoints");
    if (!path.waypoints.empty())
    {
      const Vector3 last = path.waypoints.back();
      Check(last.squaredDistance(Vector3(8.f, 8.f, 0.f)) < 0.01f, "path ends on the goal");
    }
    // Every waypoint must be on walkable floor.
    bool all_walkable = true;
    for (const Vector3& point : path.waypoints)
    {
      if (!mesh.IsStandable(point))
      {
        all_walkable = false;
      }
    }
    Check(all_walkable, "every waypoint is standable");
  }

  // ---- a sealed wall makes the far side unreachable -------------------------
  {
    ResetLevel();
    // Wall spanning the full grid height in Y, sealing left from right.
    AddWall(0.f, 0.f, 0.2f, 20.f);

    NavMesh mesh;
    mesh.Build(FlatSettings(), &SyntheticRaycast);

    const NavPath path = mesh.FindPath(Vector3(-8.f, 0.f, 0.f), Vector3(8.f, 0.f, 0.f));
    Check(path.status == NavPath::Status::unreachable, "sealed wall reports unreachable, not success");
    Check(path.waypoints.empty(), "failed path carries no waypoints");
  }

  // ---- a wall with a gap forces a detour ------------------------------------
  {
    ResetLevel();
    // Wall along Y at x = 0 with a gap in the middle: two segments, leaving
    // a corridor around y in [-2, 2].
    AddWall(0.f, 6.f, 0.2f, 4.f);    // y from 2 to 10
    AddWall(0.f, -6.f, 0.2f, 4.f);   // y from -10 to -2

    NavMesh mesh;
    const int walkable = mesh.Build(FlatSettings(), &SyntheticRaycast);
    Check(walkable > 0, "gapped wall still leaves walkable cells");

    const NavPath path = mesh.FindPath(Vector3(-8.f, 0.f, 0.f), Vector3(8.f, 0.f, 0.f));
    Check(path.status == NavPath::Status::success, "path through the gap succeeds");

    // No waypoint may sit inside a wall box (checking g_walls, not g_boxes:
    // the floor slab contains every point at z = 0).
    bool inside_wall = false;
    for (const Vector3& point : path.waypoints)
    {
      for (const Box& box : g_walls)
      {
        if (point.x > box.min_x && point.x < box.max_x && point.y > box.min_y &&
            point.y < box.max_y && point.z >= box.min_z && point.z <= box.max_z)
        {
          inside_wall = true;
          std::printf("       waypoint (%.3f, %.3f, %.3f) inside wall "
                      "x[%.3f,%.3f] y[%.3f,%.3f]\n",
                      point.x, point.y, point.z, box.min_x, box.max_x, box.min_y, box.max_y);
          std::fflush(stdout);
        }
      }
    }
    Check(!inside_wall, "no waypoint lies inside a wall");

    // Smoothing must not lengthen the path or leave the walkable set.
    const eastl::vector<Vector3> smoothed = mesh.SmoothPath(path.waypoints);
    Check(smoothed.size() <= path.waypoints.size(), "smoothing does not add waypoints");
    bool smoothed_ok = true;
    for (const Vector3& point : smoothed)
    {
      if (!mesh.IsStandable(point))
      {
        smoothed_ok = false;
      }
    }
    Check(smoothed_ok, "smoothed waypoints remain standable");
  }

  // ---- failure modes are distinguishable ------------------------------------
  {
    NavMesh empty_mesh;
    const NavPath no_mesh = empty_mesh.FindPath(Vector3(0.f, 0.f, 0.f), Vector3(1.f, 1.f, 0.f));
    Check(no_mesh.status == NavPath::Status::no_navmesh, "unbuilt mesh reports no_navmesh");
    Check(!no_mesh.IsValid(), "no_navmesh path is not valid");
  }

  // ---- real shipped level: 1-1 ---------------------------------------------
  //
  // Synthetic boxes only prove the algorithm is self-consistent. They cannot
  // catch a wrong assumption about the real assets (floor extents, wall
  // height, whether walls are actually axis-aligned). This block loads the
  // shipped level and checks both the assumption itself and the mesh built
  // from it.
#ifdef ZENGINE_TEST_LEVEL_PATH
  {
    std::printf("\n--- real level: %s ---\n", ZENGINE_TEST_LEVEL_PATH);
    std::fflush(stdout);

    FILE* level_file = fopen(ZENGINE_TEST_LEVEL_PATH, "rb");
    if (level_file == nullptr)
    {
      std::printf("       (skipped: level file not found)\n");
      std::fflush(stdout);
    }
    else
    {
      char read_buffer[65536];
      rapidjson::FileReadStream stream(level_file, read_buffer, sizeof(read_buffer));
      rapidjson::Document doc;
      doc.ParseStream(stream);
      fclose(level_file);

      if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("objects"))
      {
        std::printf("       (skipped: level JSON unparseable)\n");
        std::fflush(stdout);
      }
      else
      {
        // Wall-class objects in the shipped level, by definition filename. The
        // prefix match "Wall_" alone is wrong: it catches the 17 plain walls but
        // silently misses wall_with_door (5), wall_block (2) and
        // wall_with_window (2), i.e. 9 real obstacles the mesh must respect.
        auto IsWallDefinition = [](const std::string& definition) {
          if (definition.rfind("asset/objects/environment/wall/wall", 0) != 0)
          {
            return false;
          }
          return definition == "asset/objects/environment/wall/wall.object.json" ||
                 definition == "asset/objects/environment/wall/wall_with_door.object.json" ||
                 definition == "asset/objects/environment/wall/wall_block.object.json" ||
                 definition == "asset/objects/environment/wall/wall_with_window.object.json";
        };

        auto LevelWallYawDeg = [](const rapidjson::Value& transform) {
          const auto& rotation = transform["rotation"];
          const float z = rotation.HasMember("z") ? rotation["z"].GetFloat() : 0.f;
          const float w = rotation.HasMember("w") ? rotation["w"].GetFloat() : 1.f;
          return 2.f * std::atan2(z, w) * 180.f / 3.14159265358979f;
        };

        {
          int wall_count = 0;
          int non_axis_aligned = 0;
          float yaw_of_first_offender = 0.f;

          for (const auto& object : doc["objects"].GetArray())
          {
            if (!object.HasMember("definition") || !object.HasMember("instanced_components"))
            {
              continue;
            }
            if (!IsWallDefinition(object["definition"].GetString()))
            {
              continue;
            }
            ++wall_count;

            const auto& transform = object["instanced_components"][0]["$context"]["transform"];
            const float yaw_deg = LevelWallYawDeg(transform);
            const float mod = std::fabs(std::fmod(yaw_deg, 90.f));
            if (mod > 0.5f && mod < 89.5f)
            {
              ++non_axis_aligned;
              yaw_of_first_offender = yaw_deg;
            }
          }

          std::printf("       wall-class objects: %d, non-axis-aligned: %d", wall_count,
                      non_axis_aligned);
          if (non_axis_aligned > 0)
          {
            std::printf(" (first at yaw %.1f deg)", yaw_of_first_offender);
          }
          std::printf("\n");
          std::fflush(stdout);

          Check(wall_count == 26, "real level has all 26 wall-class objects (17 wall + 5 door + 2 block + 2 window)");
          Check(non_axis_aligned == 0,
                "all real-level walls are axis-aligned (4-direction clearance is valid)");
        }

        // Build the mesh over the real floor extent and confirm it finds
        // walkable ground. The floor is 43x24 half-extents, so a volume of
        // 80x44 centred at the origin sits inside it.
        ResetLevel();
        // Floor slab matching the shipped asset: top surface at z = 0.
        g_boxes.push_back(MakeBox(0.f, 0.f, 43.f, 24.f, -1.f, 0.f));
        for (const auto& object : doc["objects"].GetArray())
        {
          if (!object.HasMember("definition") || !object.HasMember("instanced_components"))
          {
            continue;
          }
          if (!IsWallDefinition(object["definition"].GetString()))
          {
            continue;
          }
          const auto& transform = object["instanced_components"][0]["$context"]["transform"];
          const float px = transform["position"]["x"].GetFloat();
          const float py = transform["position"]["y"].GetFloat();
          const float z = transform["rotation"].HasMember("z")
                              ? transform["rotation"]["z"].GetFloat()
                              : 0.f;
          const float w = transform["rotation"].HasMember("w")
                              ? transform["rotation"]["w"].GetFloat()
                              : 1.f;
          const float yaw_deg = LevelWallYawDeg(transform);

          // Wall box: half extents (0.15, 5, 3.5) at local offset z = 3.5.
          // Rotated by yaw about +Z, the horizontal footprint swaps.
          const bool swapped = std::fabs(std::fmod(std::fabs(yaw_deg), 180.f) - 90.f) < 1.f;
          const float hx = swapped ? 5.f : 0.15f;
          const float hy = swapped ? 0.15f : 5.f;
          AddWall(px, py, hx, hy);
        }

        NavMeshBuildSettings real_settings;
        real_settings.origin = Vector3(0.f, 0.f, 0.f);
        real_settings.cell_size = 1.f;
        real_settings.cells_x = 80;
        real_settings.cells_y = 44;
        real_settings.probe_height = 10.f;
        real_settings.probe_depth = 20.f;
        real_settings.agent_radius = 0.4f;
        real_settings.agent_height = 0.9f;
        real_settings.max_step_height = 0.6f;
        real_settings.max_slope = 0.7f;

        NavMesh real_mesh;
        const int real_walkable = real_mesh.Build(real_settings, &SyntheticRaycast);
        std::printf("       real level: %d/%d walkable\n", real_walkable, 80 * 44);
        std::fflush(stdout);
        Check(real_walkable > 0, "real level yields walkable cells");
        Check(real_walkable < 80 * 44, "real level walls block some cells");

        // Are any walkable cells floating above the floor? The downward probe
        // hits whatever it finds first, so a cell whose centre falls inside a
        // wall footprint lands on the wall TOP (z = 3) and would be marked
        // walkable -- a floating island disconnected from the floor by
        // max_step_height. Count them.
        int above_floor = 0;
        float max_walkable_z = -1000.f;
        for (const NavCell& cell : real_mesh.GetCells())
        {
          if (cell.walkable && cell.center.z > max_walkable_z)
          {
            max_walkable_z = cell.center.z;
          }
          if (cell.walkable && cell.center.z > 0.01f)
          {
            ++above_floor;
          }
        }
        // Wall tops are legitimately walkable surfaces -- the mesh is right to
        // sample them. What matters is that they stay ISLANDS: disconnected
        // from the floor by max_step_height (0.6) versus the 3.0 wall height,
        // so no agent can path onto one. Asserting "no elevated cells exist"
        // would be wrong; asserting "none is reachable from the floor" is the
        // real invariant.
        std::printf("       elevated (wall-top) cells: %d (max z = %.3f)\n",
                    above_floor, max_walkable_z);
        std::fflush(stdout);
        if (above_floor > 0)
        {
          // Pick an elevated cell and confirm the floor cannot path to it.
          int elevated_index = -1;
          for (size_t i = 0; i < real_mesh.GetCells().size(); ++i)
          {
            const NavCell& cell = real_mesh.GetCells()[i];
            if (cell.walkable && cell.center.z > 0.01f)
            {
              elevated_index = static_cast<int>(i);
              break;
            }
          }
          const NavCell& elevated = real_mesh.GetCells()[elevated_index];
          const NavPath to_top =
              real_mesh.FindPath(Vector3(3.f, 3.f, 0.f), elevated.center);
          std::printf("       floor -> wall-top (%.1f,%.1f,%.1f): status=%d\n",
                      elevated.center.x, elevated.center.y, elevated.center.z,
                      static_cast<int>(to_top.status));
          std::fflush(stdout);
          Check(to_top.status != NavPath::Status::success,
                "wall-top islands are unreachable from the floor");
        }

        // Clearance adequacy: wall half-thickness is 0.15 and agent_radius is
        // 0.4, so the probe (0.4 long, from the cell centre) must reach the
        // wall face from any cell centre within 0.4 of it. Verify that cells
        // directly beside a wall are blocked, i.e. the clearance is not too
        // small to ever fire.
        int blocked_near_wall = 0;
        int walkable_near_wall = 0;
        for (const auto& object : doc["objects"].GetArray())
        {
          if (!object.HasMember("definition") || !object.HasMember("instanced_components"))
          {
            continue;
          }
          if (!IsWallDefinition(object["definition"].GetString()))
          {
            continue;
          }
          const auto& transform = object["instanced_components"][0]["$context"]["transform"];
          const float px = transform["position"]["x"].GetFloat();
          const float py = transform["position"]["y"].GetFloat();
          const float z = transform["rotation"].HasMember("z")
                              ? transform["rotation"]["z"].GetFloat()
                              : 0.f;
          const float w = transform["rotation"].HasMember("w")
                              ? transform["rotation"]["w"].GetFloat()
                              : 1.f;
          const float yaw_deg = LevelWallYawDeg(transform);
          const bool swapped = std::fabs(std::fmod(std::fabs(yaw_deg), 180.f) - 90.f) < 1.f;

          // Sample points just off the wall's long faces, at floor level.
          const float offset = swapped ? 0.15f + 0.25f : 0.15f + 0.25f;
          const float test_offsets[2] = {offset, -offset};
          for (int side = 0; side < 2; ++side)
          {
            const float d = test_offsets[side];
            // For a wall whose long axis is Y (not swapped), its faces are at
            // x = px +/- 0.15; sample along x. When swapped, faces are along y.
            const float sx = swapped ? px : px + d;
            const float sy = swapped ? py + d : py;
            const Vector3 probe(sx, sy, 0.f);
            if (real_mesh.IsStandable(probe))
            {
              ++walkable_near_wall;
            }
            else
            {
              ++blocked_near_wall;
            }
          }
        }
        std::printf("       near-wall samples: blocked=%d walkable=%d\n",
                    blocked_near_wall, walkable_near_wall);
        std::fflush(stdout);
        Check(blocked_near_wall > 0,
              "clearance blocks cells adjacent to walls (agent_radius 0.4 vs wall 0.15)");

        // A real path across the level must exist and stay on the floor.
        const NavPath real_path =
            real_mesh.FindPath(Vector3(3.f, 3.f, 0.f), Vector3(-20.f, 10.f, 0.f));
        std::printf("       cross-level path status=%d waypoints=%zu\n",
                    static_cast<int>(real_path.status), real_path.waypoints.size());
        std::fflush(stdout);
        bool real_path_ok = true;
        for (const Vector3& point : real_path.waypoints)
        {
          if (std::fabs(point.z) > 0.6f)
          {
            real_path_ok = false;
          }
        }
        Check(real_path_ok, "cross-level path stays on floor level (no wall-top teleport)");

        // The player spawn (3, 3, 0) must be standable.
        Check(real_mesh.IsStandable(Vector3(3.f, 3.f, 0.f)), "player spawn (3,3,0) is standable");
      }
    }
  }
#endif

  std::printf("\n%s (failures: %d)\n",
              (g_failures == 0) ? "NavMesh test PASSED" : "NavMesh test FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}