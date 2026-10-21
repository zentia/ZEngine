// =============================================================================
// Lumen software ray-tracing smoke test.
//
// Filling the distance field (see LumenSceneGeometrySmokeTest) was necessary but
// not sufficient: the software marcher still could not hit anything, for two
// independent reasons.
//
//   1. TraceRaySoftware() took a LumenDistanceField* and immediately returned
//      "no hit" when it was nullptr -- while all three call sites
//      (LumenGlobalIllumination x2, LumenReflections x1) passed literal nullptr.
//      The member set by setDistanceField() was never consulted.
//
//   2. DistanceFieldMarch() treated "distance < 0.01" as a hit. The field only
//      holds values at voxel centres, so the smallest distance any query can
//      return is on the order of half a voxel diagonal -- 0.01 is unreachable,
//      every ray burned all 256 steps and reported a miss.
//
// This test pins the fixed behaviour: the argument wins over the member, the hit
// threshold scales with the voxel size, rays are clipped to the field's world box,
// hit distances converge to the surface, normals stay finite, and -- end to end --
// GI returns non-zero indirect light.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_RAY_TRACING_SMOKE_TEST=ON
//   cmake --build build --target LumenRayTracingSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Lumen/LumenDistanceField.h"
#include "Runtime/Function/Render/Lumen/LumenGlobalIllumination.h"
#include "Runtime/Function/Render/Lumen/LumenRayTracing.h"
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

    LumenConfig MakeConfig()
    {
        LumenConfig config;
        config.distance_field_resolution = 32;
        config.use_hierarchical_distance_field = true;
        config.gi_sample_count = 16;  // GI is stochastic; average enough rays to be deterministic in practice
        return config;
    }

    bool IsFinite(const Vector3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // -------------------------------------------------------------------------
    // Scene: one quad lying in y = 0, spanning [-6, 6] on x and z.
    //
    // World bounds are chosen so the voxel size is known exactly:
    //   (20/32, 10/32, 20/32) = (0.625, 0.3125, 0.625) -> diagonal ~0.938
    //   hit threshold = max(min_trace_step 0.1, diagonal * 0.5 = 0.469) = 0.469
    // -------------------------------------------------------------------------
    const LumenConfig config = MakeConfig();

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
    RenderEntity floor_entity;
    floor_entity.m_InstanceId = 1;
    floor_entity.m_MeshAssetId = 1;
    floor_entity.m_ModelMatrix = Matrix4x4::IDENTITY;
    floor_entity.m_BoundingBox = quad_local_bounds;
    render_scene->m_RenderEntities.push_back(floor_entity);

    LumenDistanceField distance_field;
    Check(distance_field.Initialize(nullptr, config), "distance field initializes without an RHI");

    const AxisAlignedBox world_bounds = MakeBox(Vector3(-10.0f, -2.0f, -10.0f), Vector3(10.0f, 8.0f, 10.0f));
    Check(distance_field.GenerateFromScene(render_scene, &render_resource, world_bounds),
          "distance field generates from the scene");
    Check(distance_field.isLoaded(), "distance field reports itself loaded");

    const float voxel_diagonal = distance_field.GetVoxelDiagonal();
    Check(voxel_diagonal > 0.9f && voxel_diagonal < 1.0f, "voxel diagonal matches the configured bounds (0.938)");

    LumenRayTracing ray_tracing;
    Check(ray_tracing.Initialize(nullptr, config), "ray tracing initializes without an RHI");
    Check(!ray_tracing.isHardwareRayTracingSupported(), "no hardware ray tracing on a headless stub");

    // -------------------------------------------------------------------------
    // 1. A ray aimed at the floor hits it, at roughly the right distance.
    // -------------------------------------------------------------------------
    const Vector3 down(0.0f, -1.0f, 0.0f);
    const LumenRayTracingResult downward = ray_tracing.TraceRaySoftware(Vector3(0.0f, 5.0f, 0.0f),
                                                                       down,
                                                                       /*max_distance=*/100.0f,
                                                                       &distance_field);
    Check(downward.hit, "a downward ray hits the floor (was: always missed)");
    Check(downward.hit_distance > 4.0f && downward.hit_distance < 5.0f,
          "the hit distance is ~4.5, not the full 5.0 (march converges onto the surface)");
    Check(downward.hit_position.y > 0.2f && downward.hit_position.y < 0.8f,
          "the hit position sits within half a voxel of the floor");
    Check(IsFinite(downward.hit_position), "the hit position is finite");
    Check(IsFinite(downward.hit_normal), "the hit normal is finite (gradient is voxel-scaled, not NaN)");
    Check(downward.hit_normal.y > 0.5f, "the hit normal points away from the floor");
    Check(std::fabs(downward.hit_normal.length() - 1.0f) < 0.01f, "the hit normal is unit length");
    Check(downward.tracing_type == RayTracingType::Software, "software tracing is reported as such");

    // -------------------------------------------------------------------------
    // 2. A ray that leaves the scene without touching anything misses.
    // -------------------------------------------------------------------------
    const LumenRayTracingResult upward = ray_tracing.TraceRaySoftware(Vector3(0.0f, 5.0f, 0.0f),
                                                                     Vector3(0.0f, 1.0f, 0.0f),
                                                                     /*max_distance=*/100.0f,
                                                                     &distance_field);
    Check(!upward.hit, "a ray into empty space does not report a hit");

    const LumenRayTracingResult sideways_outside = ray_tracing.TraceRaySoftware(Vector3(0.0f, 50.0f, 0.0f),
                                                                               Vector3(1.0f, 0.0f, 0.0f),
                                                                               /*max_distance=*/100.0f,
                                                                               &distance_field);
    Check(!sideways_outside.hit, "a ray that never enters the world box does not report a hit");

    // -------------------------------------------------------------------------
    // 3. Rays that start outside the box are clipped to it.
    //
    // Without the box clip the marcher burns every step on FLT_MAX samples
    // (the field has no data outside world_bounds) and never reaches the floor.
    // -------------------------------------------------------------------------
    const LumenRayTracingResult from_far_above = ray_tracing.TraceRaySoftware(Vector3(0.0f, 50.0f, 0.0f),
                                                                             down,
                                                                             /*max_distance=*/100.0f,
                                                                             &distance_field);
    Check(from_far_above.hit, "a ray starting outside the world box still reaches the floor");
    Check(from_far_above.hit_distance > 48.0f && from_far_above.hit_distance < 51.0f,
          "the clipped ray reports a hit distance measured from its real origin (~49.5)");

    // -------------------------------------------------------------------------
    // 4. Short rays are honoured: max_distance still cuts the trace off.
    // -------------------------------------------------------------------------
    const LumenRayTracingResult too_short = ray_tracing.TraceRaySoftware(Vector3(0.0f, 5.0f, 0.0f),
                                                                        down,
                                                                        /*max_distance=*/2.0f,
                                                                        &distance_field);
    Check(!too_short.hit, "a ray shorter than the distance to the floor does not hit");

    // -------------------------------------------------------------------------
    // 5. Degenerate input is rejected instead of producing garbage.
    // -------------------------------------------------------------------------
    const LumenRayTracingResult zero_direction = ray_tracing.TraceRaySoftware(Vector3(0.0f, 5.0f, 0.0f),
                                                                             Vector3(0.0f, 0.0f, 0.0f),
                                                                             /*max_distance=*/100.0f,
                                                                             &distance_field);
    Check(!zero_direction.hit, "a zero-length direction is rejected");

    // -------------------------------------------------------------------------
    // 6. The distance_field argument wins; the member is only a fallback.
    //
    // This is the actual bug: the old code returned "no hit" whenever the
    // argument was nullptr, and every caller passed nullptr.
    // -------------------------------------------------------------------------
    LumenDistanceField unloaded_field;
    unloaded_field.Initialize(nullptr, config);
    Check(!unloaded_field.isLoaded(), "a freshly initialized field is not loaded");

    LumenRayTracing fallback_tracing;
    fallback_tracing.Initialize(nullptr, config);
    fallback_tracing.setDistanceField(&unloaded_field);

    const LumenRayTracingResult explicit_field = fallback_tracing.TraceRay(Vector3(0.0f, 5.0f, 0.0f),
                                                                          down,
                                                                          /*max_distance=*/100.0f,
                                                                          &distance_field);
    Check(explicit_field.hit, "an explicit distance field is used even when the member points elsewhere");

    const LumenRayTracingResult falls_back = fallback_tracing.TraceRay(Vector3(0.0f, 5.0f, 0.0f),
                                                                     down,
                                                                     /*max_distance=*/100.0f,
                                                                     nullptr);
    Check(!falls_back.hit, "nullptr falls back to the member, which here is an unloaded field");

    const LumenRayTracingResult unloaded_argument = ray_tracing.TraceRaySoftware(Vector3(0.0f, 5.0f, 0.0f),
                                                                                down,
                                                                                /*max_distance=*/100.0f,
                                                                                &unloaded_field);
    Check(!unloaded_argument.hit, "an unloaded field reports a miss instead of marching over empty data");

    LumenRayTracing member_tracing;
    member_tracing.Initialize(nullptr, config);
    member_tracing.setDistanceField(&distance_field);
    const LumenRayTracingResult via_member = member_tracing.TraceRay(Vector3(0.0f, 5.0f, 0.0f),
                                                                    down,
                                                                    /*max_distance=*/100.0f,
                                                                    nullptr);
    Check(via_member.hit, "nullptr + a loaded member field still traces (old call sites keep working)");

    // -------------------------------------------------------------------------
    // 7. Starting right above the surface lands on the surface, not on itself.
    // -------------------------------------------------------------------------
    const LumenRayTracingResult close_start = ray_tracing.TraceRaySoftware(Vector3(0.0f, 1.0f, 0.0f),
                                                                          down,
                                                                          /*max_distance=*/100.0f,
                                                                          &distance_field);
    Check(close_start.hit, "a ray starting 1 unit above the floor hits it");
    Check(close_start.hit_distance > 0.35f && close_start.hit_distance < 0.65f,
          "the hit distance is ~0.5 (half a voxel diagonal), not 0 and not 1.0");

    // -------------------------------------------------------------------------
    // 8. End to end: GI produces non-zero indirect light.
    //
    // The shading point hangs above the floor with its normal pointing down, so
    // every direction in its hemisphere runs into the floor. With either bug in
    // place the result was exactly zero.
    // -------------------------------------------------------------------------
    LumenGlobalIllumination global_illumination;
    Check(global_illumination.Initialize(nullptr, config), "GI initializes without an RHI");
    global_illumination.setRayTracing(&member_tracing);
    global_illumination.setDistanceField(&distance_field);

    const Vector3 shading_point(0.0f, 4.0f, 0.0f);
    const Vector3 shading_normal(0.0f, -1.0f, 0.0f);
    const Vector3 indirect = global_illumination.GetIndirectLighting(shading_point, shading_normal);

    Check(IsFinite(indirect), "indirect lighting is finite");
    Check(indirect.length() > 0.0f, "GI returns non-zero indirect light above a floor (was: identically zero)");

    // A surface facing away from the floor sees nothing but empty space.
    const Vector3 indirect_away = global_illumination.GetIndirectLighting(shading_point, Vector3(0.0f, 1.0f, 0.0f));
    Check(IsFinite(indirect_away) && indirect_away.length() == 0.0f,
          "GI returns zero when the hemisphere faces empty space");

    // -------------------------------------------------------------------------
    // 9. Without a distance field at all, GI stays zero instead of inventing light.
    // -------------------------------------------------------------------------
    LumenGlobalIllumination orphan_gi;
    orphan_gi.Initialize(nullptr, config);
    orphan_gi.setRayTracing(&fallback_tracing);
    const Vector3 orphan_indirect = orphan_gi.GetIndirectLighting(shading_point, shading_normal);
    Check(orphan_indirect.length() == 0.0f, "GI with no distance field returns zero rather than a bogus value");

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
