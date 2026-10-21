// =============================================================================
// Lumen surface-cache smoke test.
//
// The surface cache was a dead subsystem: BuildFromScene was a TODO, so no page
// was ever created, m_PositionToPageMap stayed empty and GetSurfaceInfo -- which
// had zero callers -- always returned false. The software ray tracer therefore
// handed back a hard-coded albedo of 0.5, which means every surface in the scene
// bounced back identical indirect light.
//
// On top of that, the page key was arithmetic nonsense:
//
//     (uint64_t(x * 1000) << 32) | (uint64_t(y * 1000) << 16) | uint64_t(z * 1000)
//
// x alone overflows the 32 bits it is shifted into, y's 16-bit field overlaps
// x's low bits, z is not shifted at all, and a negative coordinate converted to
// uint64_t is UB. Distinct positions collide and identical positions can map to
// different keys.
//
// This test pins the rebuilt version: pages come from the scene with real
// materials, lookup is spatial (uniform grid over each page's world AABB),
// LRU eviction works, and -- end to end -- a ray that lands on a red floor
// reports a red albedo instead of the hard-coded grey.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_SURFACE_CACHE_SMOKE_TEST=ON
//   cmake --build build --target LumenSurfaceCacheSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Lumen/LumenDistanceField.h"
#include "Runtime/Function/Render/Lumen/LumenRayTracing.h"
#include "Runtime/Function/Render/Lumen/LumenSurfaceCache.h"
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
        config.gi_sample_count = 16;
        config.max_surface_cache_pages = 16;
        return config;
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
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

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

    // -------------------------------------------------------------------------
    // Scene: a red floor (instance 1) and a small blue box hanging above it
    // (instance 2, translated to y = 3).
    // -------------------------------------------------------------------------
    auto render_scene = std::make_shared<RenderScene>();

    RenderEntity red_floor = MakeFloorEntity(/*instance_id=*/1, /*mesh_asset_id=*/1, Vector4(1.0f, 0.0f, 0.0f, 1.0f));

    RenderEntity blue_box = MakeFloorEntity(/*instance_id=*/2, /*mesh_asset_id=*/1, Vector4(0.0f, 0.0f, 1.0f, 1.0f));
    blue_box.m_ModelMatrix = Matrix4x4::GetTrans(Vector3(0.0f, 3.0f, 0.0f));
    blue_box.m_RoughnessFactor = 0.2f;

    render_scene->m_RenderEntities.push_back(red_floor);
    render_scene->m_RenderEntities.push_back(blue_box);

    // -------------------------------------------------------------------------
    // 1. BuildFromScene actually creates pages.
    // -------------------------------------------------------------------------
    LumenSurfaceCacheManager surface_cache;
    Check(surface_cache.Initialize(nullptr, config), "surface cache initializes without an RHI");
    Check(surface_cache.getActivePageCount() == 0, "a freshly initialized cache holds no pages");

    Check(surface_cache.BuildFromScene(render_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f)),
          "buildFromScene succeeds");
    Check(surface_cache.getActivePageCount() == 2, "both instances got a page (was: zero, the whole cache was a TODO)");
    Check(!surface_cache.needsUpdate(), "building clears the needs_update flag");

    const LumenSurfaceCache& cache = surface_cache.getSurfaceCache();
    Check(cache.world_bounds.IsValid(), "the cache knows the scene's world bounds");

    // -------------------------------------------------------------------------
    // 2. Pages carry the real material from RenderEntity.
    // -------------------------------------------------------------------------
    LumenSurfaceCachePage floor_page;
    Check(surface_cache.GetSurfaceInfo(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), floor_page),
          "a point on the floor resolves to a page (was: always false)");
    Check(floor_page.instance_id == 1, "the page reports which instance it came from");
    Check(floor_page.albedo.x > 0.9f && floor_page.albedo.y < 0.1f && floor_page.albedo.z < 0.1f,
          "the floor page took its albedo from m_BaseColorFactor (red)");
    Check(std::fabs(floor_page.roughness - 0.8f) < 0.001f, "the floor page took its roughness from the entity");
    Check(floor_page.emissive.length() < 0.001f, "a non-emissive surface reports zero emissive");

    LumenSurfaceCachePage box_page;
    Check(surface_cache.GetSurfaceInfo(Vector3(0.0f, 3.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), box_page),
          "a point on the hanging box resolves to a page");
    Check(box_page.instance_id == 2, "the box point maps to instance 2, not the floor");
    Check(box_page.albedo.z > 0.9f && box_page.albedo.x < 0.1f, "the box page is blue");
    Check(std::fabs(box_page.roughness - 0.2f) < 0.001f, "the box page kept its own roughness");
    Check(box_page.page_id != floor_page.page_id, "the two instances live in different pages");

    // -------------------------------------------------------------------------
    // 3. Lookup is spatial, not a hash of the position's floats.
    //
    // These three positions were the ones the old key mangled beyond recognition:
    // huge coordinates, negative coordinates, and coordinates a hair apart.
    // -------------------------------------------------------------------------
    LumenSurfaceCachePage far_page;
    Check(!surface_cache.GetSurfaceInfo(Vector3(100000.0f, 100000.0f, 100000.0f), Vector3(0.0f, 1.0f, 0.0f), far_page),
          "a point far outside the scene resolves to nothing");

    // Negative coordinates must not be silently mangled into a valid lookup.
    LumenSurfaceCachePage negative_page;
    const bool negative_hit =
        surface_cache.GetSurfaceInfo(Vector3(-5.0f, 0.0f, -5.0f), Vector3(0.0f, 1.0f, 0.0f), negative_page);
    Check(negative_hit && negative_page.instance_id == 1,
          "negative coordinates still resolve to the right page (old key: UB on negative -> uint64_t)");

    // A 1e-3 nudge must not land in a different bucket.
    LumenSurfaceCachePage nudged_page;
    const bool nudged_hit =
        surface_cache.GetSurfaceInfo(Vector3(-5.0f + 0.001f, 0.0f, -5.0f), Vector3(0.0f, 1.0f, 0.0f), nudged_page);
    Check(nudged_hit && nudged_page.instance_id == negative_page.instance_id,
          "a 1mm nudge still resolves to the same page");

    // A point that is outside every page's box (the distance field stops rays about
    // half a voxel *off* the surface) must fall back to the nearest page rather than
    // giving up: y = 1 sits between the floor (y = 0) and the box (y = 3).
    LumenSurfaceCachePage between;
    Check(surface_cache.GetSurfaceInfo(Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), between) &&
              between.instance_id == 1,
          "a point off every surface falls back to the nearest page");

    // -------------------------------------------------------------------------
    // 4. The scene hash drives the rebuild gate.
    // -------------------------------------------------------------------------
    const uint32_t pages_before = surface_cache.getActivePageCount();

    // Nothing changed -> no rebuild, pages survive.
    surface_cache.UpdateSurfaceCache(render_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f));
    Check(surface_cache.getActivePageCount() == pages_before, "an unchanged scene keeps its pages");

    // Changing a material must invalidate the cache -- otherwise GI keeps bouncing
    // the old colour.
    render_scene->m_RenderEntities[0].m_BaseColorFactor = Vector4(0.0f, 1.0f, 0.0f, 1.0f);
    surface_cache.UpdateSurfaceCache(render_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f));
    LumenSurfaceCachePage recolored;
    Check(surface_cache.GetSurfaceInfo(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), recolored),
          "the recoloured scene still resolves");
    Check(recolored.albedo.y > 0.9f && recolored.albedo.x < 0.1f,
          "changing m_BaseColorFactor rebuilds the page (floor is now green)");

    // Put it back so the ray-tracing checks below see the red floor.
    render_scene->m_RenderEntities[0].m_BaseColorFactor = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
    surface_cache.UpdateSurfaceCache(render_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f));

    // -------------------------------------------------------------------------
    // 5. LRU eviction frees idle pages, and the cache refills afterwards.
    // -------------------------------------------------------------------------
    LumenSurfaceCachePage touched;
    Check(surface_cache.GetSurfaceInfo(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), touched),
          "the floor page is present before eviction");
    const uint64_t frame_after_touch =
        surface_cache.getSurfaceCache().pages[touched.page_id].last_access_time;

    // Run forward without querying anything until the pages go idle. Stop as soon as
    // they are gone -- running further would trip the "cache is empty -> rebuild"
    // branch and refill them.
    uint64_t frames_waited = 0;
    while (surface_cache.getActivePageCount() != 0 && frames_waited < LumenConstants::kMaxSurfaceCachePageIdleFrames + 10)
    {
        surface_cache.UpdateSurfaceCache(render_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f));
        ++frames_waited;
    }
    Check(surface_cache.getActivePageCount() == 0, "pages idle past the threshold are evicted");
    Check(frames_waited >= LumenConstants::kMaxSurfaceCachePageIdleFrames,
          "eviction waited for the full idle window rather than dropping pages immediately");
    Check(surface_cache.getFrameIndex() > frame_after_touch, "the frame counter advances every update");

    // The empty cache must rebuild on the next update instead of staying empty forever.
    surface_cache.UpdateSurfaceCache(render_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f));
    Check(surface_cache.getActivePageCount() == 2, "an emptied cache refills on the next update");

    // -------------------------------------------------------------------------
    // 6. End to end: a ray that lands on the red floor reports red albedo.
    //
    // Before the surface cache existed, TraceRaySoftware hard-coded albedo 0.5 and
    // every surface in the scene bounced identical grey light.
    //
    // The ray starts *below* the hanging blue box (y = 1.5) so the first thing it
    // meets going down is the floor.
    // -------------------------------------------------------------------------
    LumenDistanceField distance_field;
    distance_field.Initialize(nullptr, config);
    Check(distance_field.GenerateFromScene(render_scene, &render_resource, world_bounds),
          "the distance field builds for the ray-tracing checks");

    LumenRayTracing with_cache;
    with_cache.Initialize(nullptr, config);
    with_cache.setDistanceField(&distance_field);
    with_cache.setSurfaceCache(&surface_cache);

    const LumenRayTracingResult hit_floor =
        with_cache.TraceRaySoftware(Vector3(0.0f, 1.5f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), 100.0f, &distance_field);
    Check(hit_floor.hit, "the downward ray hits the floor");
    Check(hit_floor.albedo.x > 0.9f && hit_floor.albedo.y < 0.1f && hit_floor.albedo.z < 0.1f,
          "the hit reports the floor's red albedo (was: hard-coded 0.5 grey)");
    Check(hit_floor.instance_id == 1, "the hit identifies the instance it landed on");
    Check(std::fabs(hit_floor.roughness - 0.8f) < 0.001f, "the hit carries the surface roughness");

    // Without a surface cache the ray still hits, but only with the neutral default.
    LumenRayTracing without_cache;
    without_cache.Initialize(nullptr, config);
    without_cache.setDistanceField(&distance_field);
    const LumenRayTracingResult grey_hit =
        without_cache.TraceRaySoftware(Vector3(0.0f, 1.5f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), 100.0f, &distance_field);
    Check(grey_hit.hit, "a ray still hits without a surface cache");
    Check(std::fabs(grey_hit.albedo.x - 0.5f) < 0.001f && std::fabs(grey_hit.albedo.y - 0.5f) < 0.001f,
          "without a surface cache the albedo falls back to the neutral 0.5 grey");

    // -------------------------------------------------------------------------
    // 7. An empty scene produces an empty cache, not a crash.
    // -------------------------------------------------------------------------
    auto empty_scene = std::make_shared<RenderScene>();
    LumenSurfaceCacheManager empty_cache;
    empty_cache.Initialize(nullptr, config);
    Check(empty_cache.BuildFromScene(empty_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f)),
          "an empty scene builds without failing");
    Check(empty_cache.getActivePageCount() == 0, "an empty scene holds no pages");

    LumenSurfaceCachePage nothing;
    Check(!empty_cache.GetSurfaceInfo(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), nothing),
          "querying an empty cache returns false");

    Check(!empty_cache.BuildFromScene(nullptr, &render_resource, Vector3(0.0f, 0.0f, 0.0f)),
          "a null scene reports failure");

    // -------------------------------------------------------------------------
    // 8. Instances without any bounds are skipped instead of poisoning the grid.
    // -------------------------------------------------------------------------
    auto boundless_scene = std::make_shared<RenderScene>();
    RenderEntity boundless = red_floor;
    boundless.m_InstanceId = 7;
    boundless.m_BoundingBox = AxisAlignedBox();  // invalid, and no geometry for asset 99
    boundless.m_MeshAssetId = 99;
    boundless_scene->m_RenderEntities.push_back(boundless);

    LumenSurfaceCacheManager boundless_cache;
    boundless_cache.Initialize(nullptr, config);
    Check(boundless_cache.BuildFromScene(boundless_scene, &render_resource, Vector3(0.0f, 0.0f, 0.0f)),
          "an instance with no bounds does not break the build");
    Check(boundless_cache.getActivePageCount() == 0, "an instance with no bounds is skipped");

    // -------------------------------------------------------------------------
    // 9. clear() empties everything.
    // -------------------------------------------------------------------------
    surface_cache.clear();
    Check(surface_cache.getActivePageCount() == 0 && surface_cache.getSurfaceCache().pages.empty(),
          "clear() empties the surface cache");

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
