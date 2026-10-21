// =============================================================================
// Lumen global-illumination smoke test.
//
// GI was an empty shell. ComputeGlobalIllumination() was a TODO that returned
// true without touching anything, so nothing ever wrote m_LightingCache --
// which meant SpatialReuse() (a linear scan over that permanently empty cache)
// always returned zero, which meant every GetIndirectLighting() call fell
// through to a fresh stochastic trace. TemporalAccumulation() was likewise a
// TODO with an empty body, so there was nowhere for the noise to go.
//
// This test pins the rebuilt version:
//
//   * ComputeGlobalIllumination fills a world-space irradiance probe grid
//     anchored to the distance field's world box -- not to the camera, or a
//     moving camera would throw the history away every frame.
//   * probe updates are budgeted and rotate, so one frame never traces the
//     whole grid;
//   * temporal accumulation actually lags (rate 0 freezes a probe, a large
//     rate lets it catch up), which is the observable proof the blend is real;
//   * the colour in the grid comes from the surface cache -- recolour the floor
//     from red to blue and the probes follow;
//   * GetIndirectLighting reads the grid back through trilinear interpolation
//     instead of re-tracing.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_GLOBAL_ILLUMINATION_SMOKE_TEST=ON
//   cmake --build build --target LumenGlobalIlluminationSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Lumen/LumenDistanceField.h"
#include "Runtime/Function/Render/Lumen/LumenGlobalIllumination.h"
#include "Runtime/Function/Render/Lumen/LumenRayTracing.h"
#include "Runtime/Function/Render/Lumen/LumenSurfaceCache.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderType.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    // Only the CPU geometry cache matters here; every GPU upload is a no-op.
    class StubRenderResource : public RenderResourceBase
    {
    public:
        void clear() override { ClearMeshGeometryCache(); }

        void UploadGlobalRenderResource(RHI*, LevelResourceDesc) override {}

        void UploadGameObjectRenderResource(RHI*, RenderEntity, RenderMeshData, RenderMaterialData) override {}

        void UploadGameObjectRenderResource(RHI*, RenderEntity, RenderMeshData) override {}

        void UploadGameObjectRenderResource(RHI*, RenderEntity, RenderMaterialData) override {}

        void UpdatePerFrameBuffer(std::shared_ptr<RenderScene>, std::shared_ptr<RenderCamera>) override {}

        void RegisterMesh(size_t asset_id, const RenderMeshData& mesh_data, const AxisAlignedBox& local_bounds)
        {
            CacheMeshGeometry(asset_id, mesh_data, local_bounds);
        }
    };

    RenderMeshData MakeQuad(const Vector3& v0, const Vector3& v1, const Vector3& v2, const Vector3& v3)
    {
        RenderMeshData mesh_data;
        mesh_data.m_StaticMeshData.m_VertexBuffer =
            std::make_shared<BufferData>(4 * sizeof(MeshVertexDataDefinition));
        std::memset(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data, 0, 4 * sizeof(MeshVertexDataDefinition));
        MeshVertexDataDefinition* vertices =
            static_cast<MeshVertexDataDefinition*>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data);

        const Vector3 corners[4] = {v0, v1, v2, v3};
        for (size_t i = 0; i < 4; ++i)
        {
            vertices[i].x = corners[i].x;
            vertices[i].y = corners[i].y;
            vertices[i].z = corners[i].z;
        }

        mesh_data.m_StaticMeshData.m_IndexBuffer = std::make_shared<BufferData>(6 * sizeof(uint16_t));
        uint16_t* indices = static_cast<uint16_t*>(mesh_data.m_StaticMeshData.m_IndexBuffer->m_Data);
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 0;
        indices[4] = 2;
        indices[5] = 3;

        return mesh_data;
    }

    AxisAlignedBox MakeBox(const Vector3& min_corner, const Vector3& max_corner)
    {
        AxisAlignedBox box;
        box.Merge(min_corner);
        box.Merge(max_corner);
        return box;
    }

    // A floor quad at y = 0 spanning [-6, 6] on x and z.
    RenderEntity MakeFloorEntity(uint32_t instance_id, size_t mesh_asset_id, const Vector4& base_color)
    {
        RenderEntity entity;
        entity.m_InstanceId = instance_id;
        entity.m_MeshAssetId = mesh_asset_id;
        entity.m_ModelMatrix = Matrix4x4::IDENTITY;
        entity.m_BoundingBox = MakeBox(Vector3(-6.0f, 0.0f, -6.0f), Vector3(6.0f, 0.0f, 6.0f));
        entity.m_BaseColorFactor = base_color;
        entity.m_RoughnessFactor = 0.8f;
        entity.m_MetallicFactor = 0.0f;
        entity.m_EmissiveFactor = Vector3(0.0f, 0.0f, 0.0f);
        return entity;
    }

    // gi_sample_count is the number of rays per probe; the probe value is a mean
    // over stochastic directions, so crank it up to make the hue comparisons
    // below deterministic in practice.
    LumenConfig MakeConfig()
    {
        LumenConfig config;
        config.distance_field_resolution = 32;
        config.use_hierarchical_distance_field = true;
        config.gi_sample_count = 32;
        config.max_surface_cache_pages = 16;
        return config;
    }

    struct GridSummary
    {
        uint32_t updated {0};
        Vector3 sum {0.0f, 0.0f, 0.0f};
    };

    GridSummary Summarize(const LumenProbeGrid& grid)
    {
        GridSummary summary;
        for (uint32_t i = 0; i < grid.GetProbeCount(); ++i)
        {
            if (grid.last_updated_frame[i] == 0)
            {
                continue;
            }
            summary.sum += grid.irradiance[i];
            ++summary.updated;
        }
        return summary;
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // -------------------------------------------------------------------------
    // Scene: a single floor quad at y = 0.
    //
    // World bounds (20 x 10 x 20) with gi_probe_spacing = 4 give a known grid:
    //   resolution = (round(20/4), round(10/4), round(20/4)) = (5, 3, 5) = 75 probes
    //   spacing    = (4.0, 3.333, 4.0)
    // -------------------------------------------------------------------------
    const LumenConfig config = MakeConfig();
    const AxisAlignedBox world_bounds = MakeBox(Vector3(-10.0f, -2.0f, -10.0f), Vector3(10.0f, 8.0f, 10.0f));

    StubRenderResource render_resource;
    const AxisAlignedBox quad_local_bounds = MakeBox(Vector3(-6.0f, 0.0f, -6.0f), Vector3(6.0f, 0.0f, 6.0f));
    render_resource.RegisterMesh(
        /*asset_id=*/1,
        MakeQuad(Vector3(-6.0f, 0.0f, -6.0f),
                 Vector3(6.0f, 0.0f, -6.0f),
                 Vector3(6.0f, 0.0f, 6.0f),
                 Vector3(-6.0f, 0.0f, 6.0f)),
        quad_local_bounds);

    auto render_scene = std::make_shared<RenderScene>();
    render_scene->m_RenderEntities.push_back(
        MakeFloorEntity(/*instance_id=*/1, /*mesh_asset_id=*/1, Vector4(1.0f, 0.0f, 0.0f, 1.0f)));  // red floor

    auto camera = std::make_shared<RenderCamera>();
    camera->m_Position = Vector3(0.0f, 2.0f, 0.0f);

    // -------------------------------------------------------------------------
    // Wiring: distance field -> surface cache -> ray tracing -> GI.
    // GI itself never touches the scene; it only reads the two subsystems.
    // -------------------------------------------------------------------------
    LumenDistanceField distance_field;
    Check(distance_field.Initialize(nullptr, config), "distance field initializes without an RHI");
    Check(distance_field.GenerateFromScene(render_scene, &render_resource, world_bounds),
          "distance field generates from the scene");

    LumenSurfaceCacheManager surface_cache;
    Check(surface_cache.Initialize(nullptr, config), "surface cache initializes without an RHI");
    Check(surface_cache.BuildFromScene(render_scene, &render_resource, camera->position()),
          "surface cache builds from the scene");

    LumenRayTracing ray_tracing;
    Check(ray_tracing.Initialize(nullptr, config), "ray tracing initializes without an RHI");
    ray_tracing.setDistanceField(&distance_field);
    ray_tracing.setSurfaceCache(&surface_cache);

    LumenGlobalIllumination global_illumination;
    Check(global_illumination.Initialize(nullptr, config), "GI initializes without an RHI");
    global_illumination.setRayTracing(&ray_tracing);
    global_illumination.setSurfaceCache(&surface_cache);
    global_illumination.setDistanceField(&distance_field);

    // -------------------------------------------------------------------------
    // 1. ComputeGlobalIllumination actually produces a probe grid.
    //
    // Before, this call was a TODO that returned true and did nothing at all.
    // -------------------------------------------------------------------------
    Check(global_illumination.getProbeGrid().GetProbeCount() == 0, "a fresh GI holds no probes");

    Check(global_illumination.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "ComputeGlobalIllumination succeeds");

    const LumenProbeGrid& grid = global_illumination.getProbeGrid();
    Check(grid.IsValid(), "the probe grid is valid after one update (was: nothing was ever built)");
    Check(grid.GetProbeCount() == 5 * 3 * 5, "the grid has 5 x 3 x 5 = 75 probes for the configured spacing");
    Check(grid.resolution_x == 5 && grid.resolution_y == 3 && grid.resolution_z == 5,
          "each axis' resolution follows gi_probe_spacing");
    Check(std::fabs(grid.probe_spacing.y - 10.0f / 3.0f) < 0.01f,
          "the clamped axis stretches its spacing rather than overflowing the box");

    const GridSummary first = Summarize(grid);
    Check(first.updated > 0, "at least one probe was updated on the first frame (was: never)");
    Check(first.sum.length() > 0.0f, "the updated probes carry non-zero irradiance");
    Check(first.sum.x > first.sum.z, "the irradiance is red-dominant, matching the red floor");

    // -------------------------------------------------------------------------
    // 2. The grid is anchored to the distance field's world box, not the camera.
    //
    // A camera-anchored grid would be rebuilt whenever the camera moves, which
    // would throw away every probe's history and make temporal accumulation
    // meaningless -- the whole reason the grid exists.
    // -------------------------------------------------------------------------
    Check(grid.world_bounds.getMinCorner() == world_bounds.getMinCorner() &&
              grid.world_bounds.getMaxCorner() == world_bounds.getMaxCorner(),
          "the grid covers the distance field's world bounds");

    const uint32_t resolution_before = grid.resolution_x;
    const uint32_t updated_before = Summarize(grid).updated;

    camera->m_Position = Vector3(500.0f, 500.0f, 500.0f);
    Check(global_illumination.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "GI still runs with the camera far away");
    Check(global_illumination.getProbeGrid().GetProbeCount() == grid.GetProbeCount(),
          "moving the camera does not resize the grid");
    Check(global_illumination.getProbeGrid().resolution_x == resolution_before,
          "moving the camera does not change the resolution");
    Check(global_illumination.getProbeGrid().world_bounds.getMinCorner() == world_bounds.getMinCorner(),
          "moving the camera does not move the grid");
    Check(Summarize(global_illumination.getProbeGrid()).updated == updated_before,
          "moving the camera does not discard accumulated probe history");

    camera->m_Position = Vector3(0.0f, 2.0f, 0.0f);

    // -------------------------------------------------------------------------
    // 3. Probe updates are budgeted and rotate.
    // -------------------------------------------------------------------------
    LumenConfig budgeted_config = config;
    budgeted_config.gi_probe_update_budget = 8;

    LumenGlobalIllumination budgeted_gi;
    budgeted_gi.Initialize(nullptr, budgeted_config);
    budgeted_gi.setRayTracing(&ray_tracing);
    budgeted_gi.setSurfaceCache(&surface_cache);
    budgeted_gi.setDistanceField(&distance_field);

    Check(budgeted_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "a budgeted GI runs");
    Check(budgeted_gi.getProbeGrid().GetProbeCount() == 75, "the budget does not change the grid size");
    Check(Summarize(budgeted_gi.getProbeGrid()).updated == 8,
          "one frame updates exactly gi_probe_update_budget probes, not all 75");

    for (int frame = 0; frame < 11; ++frame)
    {
        budgeted_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
    }
    Check(Summarize(budgeted_gi.getProbeGrid()).updated == 75,
          "the rotating cursor eventually covers every probe");

    // -------------------------------------------------------------------------
    // 4. Resolution is clamped, however small the requested spacing is.
    // -------------------------------------------------------------------------
    LumenConfig dense_config = config;
    dense_config.gi_probe_spacing = 0.01f;  // would ask for 2000 probes per axis

    LumenGlobalIllumination dense_gi;
    dense_gi.Initialize(nullptr, dense_config);
    dense_gi.setDistanceField(&distance_field);
    Check(dense_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "a GI with an absurdly small probe spacing still runs");
    Check(dense_gi.getProbeGrid().resolution_x == LumenConstants::kMaxGiProbeResolution &&
              dense_gi.getProbeGrid().resolution_y == LumenConstants::kMaxGiProbeResolution,
          "the per-axis resolution is clamped to kMaxGiProbeResolution instead of exploding");

    // -------------------------------------------------------------------------
    // 5. Temporal accumulation: the blend rate controls how fast a probe follows
    //    a change. This is the observable proof the accumulation is real --
    //    before, TemporalAccumulation() had an empty body.
    //
    // Both instances see the red floor first, then the floor is recoloured blue.
    // -------------------------------------------------------------------------
    LumenConfig frozen_config = config;
    frozen_config.gi_temporal_blend_rate = 0.0f;  // alpha == 0: a probe keeps its first value

    LumenGlobalIllumination frozen_gi;
    frozen_gi.Initialize(nullptr, frozen_config);
    frozen_gi.setRayTracing(&ray_tracing);
    frozen_gi.setSurfaceCache(&surface_cache);
    frozen_gi.setDistanceField(&distance_field);

    LumenConfig fast_config = config;
    fast_config.gi_temporal_blend_rate = 1000.0f;  // alpha ~ 1: a probe tracks the sample

    LumenGlobalIllumination fast_gi;
    fast_gi.Initialize(nullptr, fast_config);
    fast_gi.setRayTracing(&ray_tracing);
    fast_gi.setSurfaceCache(&surface_cache);
    fast_gi.setDistanceField(&distance_field);

    Check(frozen_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "the frozen GI runs once");
    Check(fast_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "the fast GI runs once");

    const GridSummary frozen_red = Summarize(frozen_gi.getProbeGrid());
    const GridSummary fast_red = Summarize(fast_gi.getProbeGrid());
    Check(frozen_red.sum.x > frozen_red.sum.z, "the frozen GI sees a red floor");
    Check(fast_red.sum.x > fast_red.sum.z, "the fast GI sees a red floor");

    // Recolour the floor. The scene hash changes, so the surface cache rebuilds
    // its pages; the distance field (geometry only) is untouched, so the probe
    // grids survive and can be compared before/after.
    render_scene->m_RenderEntities[0].m_BaseColorFactor = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
    surface_cache.UpdateSurfaceCache(render_scene, &render_resource, camera->position());
    Check(surface_cache.getActivePageCount() == 1, "the recoloured floor still has a page");

    LumenSurfaceCachePage recolored_page;
    Check(surface_cache.GetSurfaceInfo(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), recolored_page),
          "the surface cache rebuilds its page after the recolour");
    Check(recolored_page.albedo.z > 0.9f && recolored_page.albedo.x < 0.1f,
          "the rebuilt page reports the new blue albedo");

    for (int frame = 0; frame < 4; ++frame)
    {
        frozen_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        fast_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
    }

    const GridSummary frozen_after = Summarize(frozen_gi.getProbeGrid());
    const GridSummary fast_after = Summarize(fast_gi.getProbeGrid());

    Check(frozen_after.sum.x > frozen_after.sum.z,
          "a zero blend rate freezes the probes: they still report the old red floor");
    Check(fast_after.sum.z > fast_after.sum.x,
          "a large blend rate lets the probes follow the recolour to blue (was: accumulation was a no-op)");

    // Restore the red floor for the checks below.
    render_scene->m_RenderEntities[0].m_BaseColorFactor = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    surface_cache.UpdateSurfaceCache(render_scene, &render_resource, camera->position());

    // -------------------------------------------------------------------------
    // 6. GetIndirectLighting reads the grid back through trilinear interpolation.
    //
    // Sampling exactly at a probe centre must return that probe's own value --
    // that is the trilinear weight collapsing onto one corner.
    // -------------------------------------------------------------------------
    // Pick the brightest updated probe. Probe values are stochastic means, so
    // "the first one" is not a stable thing to assert on -- a probe in a corner
    // of the box can legitimately see nothing at all.
    const LumenProbeGrid& filled = global_illumination.getProbeGrid();
    uint32_t sample_index = 0;
    float brightest = -1.0f;
    for (uint32_t i = 0; i < filled.GetProbeCount(); ++i)
    {
        if (filled.last_updated_frame[i] == 0)
        {
            continue;
        }
        const float magnitude = filled.irradiance[i].length();
        if (magnitude > brightest)
        {
            brightest = magnitude;
            sample_index = i;
        }
    }
    Check(brightest > 0.0f, "the main GI has at least one updated probe carrying light");

    const Vector3 probe_position = filled.GetProbeWorldPosition(sample_index);
    const Vector3 at_probe = global_illumination.GetIndirectLighting(probe_position, Vector3(0.0f, 1.0f, 0.0f));
    Check(std::fabs(at_probe.x - filled.irradiance[sample_index].x) < 0.001f &&
              std::fabs(at_probe.y - filled.irradiance[sample_index].y) < 0.001f &&
              std::fabs(at_probe.z - filled.irradiance[sample_index].z) < 0.001f,
          "sampling at a probe centre returns that probe's value (trilinear collapses to one corner)");

    // A point halfway to the neighbouring probe must come out *between* the two
    // values, not snapped to either -- that is what distinguishes interpolation
    // from a nearest-probe lookup.
    const int32_t x_index = static_cast<int32_t>(sample_index % filled.resolution_x);
    const float step = x_index + 1 < static_cast<int32_t>(filled.resolution_x) ? 0.5f : -0.5f;
    const uint32_t neighbour_index = step > 0.0f ? sample_index + 1 : sample_index - 1;

    const Vector3 between = probe_position + Vector3(filled.probe_spacing.x * step, 0.0f, 0.0f);
    const Vector3 interpolated = global_illumination.GetIndirectLighting(between, Vector3(0.0f, 1.0f, 0.0f));

    const float self_value = filled.irradiance[sample_index].x;
    const float neighbour_value = filled.irradiance[neighbour_index].x;
    const float low = std::min(self_value, neighbour_value) - 0.001f;
    const float high = std::max(self_value, neighbour_value) + 0.001f;

    Check(interpolated.length() > 0.0f, "a point between probes interpolates to non-zero irradiance");
    Check(interpolated.x >= low && interpolated.x <= high,
          "the interpolated value lies between the two neighbouring probes (not snapshotted to one)");

    // Outside the grid the query clamps instead of reading out of bounds.
    const Vector3 outside = global_illumination.GetIndirectLighting(Vector3(100000.0f, 100000.0f, 100000.0f),
                                                                    Vector3(0.0f, 1.0f, 0.0f));
    Check(std::isfinite(outside.x) && std::isfinite(outside.y) && std::isfinite(outside.z),
          "a query far outside the grid clamps and stays finite");

    // -------------------------------------------------------------------------
    // 7. Without a distance field there is no world box, so there is no grid.
    // -------------------------------------------------------------------------
    LumenGlobalIllumination orphan_gi;
    orphan_gi.Initialize(nullptr, config);
    orphan_gi.setRayTracing(&ray_tracing);
    Check(!orphan_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "GI without a distance field reports failure instead of inventing a grid");
    Check(orphan_gi.getProbeGrid().GetProbeCount() == 0, "GI without a distance field holds no probes");

    // Null arguments are rejected, not dereferenced.
    Check(!global_illumination.ComputeGlobalIllumination(nullptr, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "a null scene is rejected");
    Check(!global_illumination.ComputeGlobalIllumination(render_scene, nullptr, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "a null camera is rejected");

    // An unloaded distance field behaves the same way: no box, no probes.
    LumenDistanceField unloaded_field;
    unloaded_field.Initialize(nullptr, config);
    Check(!unloaded_field.isLoaded(), "a freshly initialized distance field is not loaded");
    LumenGlobalIllumination unloaded_gi;
    unloaded_gi.Initialize(nullptr, config);
    unloaded_gi.setRayTracing(&ray_tracing);
    unloaded_gi.setDistanceField(&unloaded_field);
    Check(!unloaded_gi.ComputeGlobalIllumination(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f),
          "an unloaded distance field yields no probe grid");

    // -------------------------------------------------------------------------
    // 8. clear() empties the grid.
    // -------------------------------------------------------------------------
    global_illumination.clear();
    Check(global_illumination.getProbeGrid().GetProbeCount() == 0, "clear() empties the probe grid");
    Check(!global_illumination.getProbeGrid().IsValid(), "clear() leaves the grid invalid");

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
