// =============================================================================
// Lumen reflections smoke test.
//
// LumenReflections had real-looking code that nobody ever ran:
// LumenRenderer::ComputeReflections() was an empty TODO, so
// LumenReflections::ComputeReflection() had **zero callers**, and
// LumenReflectionData never left the Lumen directory -- no shader, no
// RenderScene field, no consumer. Reflections were a dead end.
//
// This test pins the two halves of fixing that:
//
// 1. The computation itself had bugs that only show up once someone calls it:
//
//    * BlendReflections() chose a source using max_trace_distance -- a CONSTANT.
//      With max_trace_distance < 100 and roughness < 0.3 it picked screen-space
//      reflections, and ComputeScreenSpaceReflection() is a TODO that returns
//      nothing, so a distance-field hit that had already been traced was thrown
//      away and replaced with black. Weighting is now "did this actually hit".
//
//    * ComputeReflection() computed the mirror direction into
//      result.reflection_direction and then overwrote `result` wholesale with
//      BlendReflections()'s output, which never set that field -- the direction
//      was silently lost.
//
//    * The roughness TODO was never implemented, so a rough surface still traced
//      one perfect mirror ray; and the result was then multiplied by
//      (1 - roughness), which double-counts roughness (the BRDF lobe in the
//      shader already handles it) and sends roughness = 1 to exactly black.
//      Roughness now widens a cone and averages several rays; the radiance is
//      no longer attenuated.
//
//    * "Did we hit anything" was inferred from the colour, so a legitimately
//      black hit (a black surface) was indistinguishable from a miss. There is
//      now an explicit `valid` flag.
//
// 2. The accumulation: UpdateReflections() walks the surface-cache pages (they
//    already carry a world position and a normal, i.e. they ARE shading points),
//    budgeted and round-robin, with the same frame-rate-independent temporal
//    blend as the GI probe grid -- rate 0 freezes, a large rate catches up.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_REFLECTIONS_SMOKE_TEST=ON
//   cmake --build build --target LumenReflectionsSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Lumen/LumenDistanceField.h"
#include "Runtime/Function/Render/Lumen/LumenRayTracing.h"
#include "Runtime/Function/Render/Lumen/LumenReflections.h"
#include "Runtime/Function/Render/Lumen/LumenSurfaceCache.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

    RenderEntity MakeEntity(uint32_t instance_id,
                            size_t mesh_asset_id,
                            const Vector4& base_color,
                            float roughness,
                            const Vector3& min_corner,
                            const Vector3& max_corner)
    {
        RenderEntity entity;
        entity.m_InstanceId = instance_id;
        entity.m_MeshAssetId = mesh_asset_id;
        entity.m_ModelMatrix = Matrix4x4::IDENTITY;
        entity.m_BoundingBox = MakeBox(min_corner, max_corner);
        entity.m_BaseColorFactor = base_color;
        entity.m_RoughnessFactor = roughness;
        entity.m_MetallicFactor = 0.0f;
        entity.m_EmissiveFactor = Vector3(0.0f, 0.0f, 0.0f);
        return entity;
    }

    LumenConfig MakeConfig()
    {
        LumenConfig config;
        config.distance_field_resolution = 32;
        config.max_surface_cache_pages = 16;
        // 粗糙表面会在一个锥里打多条光线，样本多一些结果才稳定（全 miss 的概率 ~1e-10）
        config.reflection_sample_count = 32;
        return config;
    }

    // 页面 = 一个实例，自带世界位置（AABB 中心）和法线（恒为 +Y），天然就是一个着色点。
    // 按 instance_id 精确取，不取"第一个有效页面" —— 页面槽位的分配顺序不是断言对象。
    const LumenSurfaceCachePage* FindPageOfInstance(const LumenSurfaceCacheManager& surface_cache,
                                                    uint64_t instance_id)
    {
        for (const LumenSurfaceCachePage& page : surface_cache.getSurfaceCache().pages)
        {
            if (page.is_valid && page.instance_id == instance_id)
            {
                return &page;
            }
        }
        return nullptr;
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // -------------------------------------------------------------------------
    // Scene: a big red floor at y = 0 (12 x 12) plus a small blue patch, both
    // inside a known world box. Two instances -> two surface-cache pages, which
    // is what makes the update budget observable.
    // -------------------------------------------------------------------------
    const LumenConfig config = MakeConfig();
    const AxisAlignedBox world_bounds = MakeBox(Vector3(-10.0f, -2.0f, -10.0f), Vector3(10.0f, 8.0f, 10.0f));

    StubRenderResource render_resource;
    render_resource.RegisterMesh(/*asset_id=*/1,
                                 MakeQuad(Vector3(-6.0f, 0.0f, -6.0f),
                                          Vector3(6.0f, 0.0f, -6.0f),
                                          Vector3(6.0f, 0.0f, 6.0f),
                                          Vector3(-6.0f, 0.0f, 6.0f)),
                                 MakeBox(Vector3(-6.0f, 0.0f, -6.0f), Vector3(6.0f, 0.0f, 6.0f)));
    render_resource.RegisterMesh(/*asset_id=*/2,
                                 MakeQuad(Vector3(7.0f, 0.0f, -2.0f),
                                          Vector3(10.0f, 0.0f, -2.0f),
                                          Vector3(10.0f, 0.0f, 2.0f),
                                          Vector3(7.0f, 0.0f, 2.0f)),
                                 MakeBox(Vector3(7.0f, 0.0f, -2.0f), Vector3(10.0f, 0.0f, 2.0f)));

    auto render_scene = std::make_shared<RenderScene>();
    render_scene->m_RenderEntities.push_back(MakeEntity(/*instance_id=*/1,
                                                       /*mesh_asset_id=*/1,
                                                       Vector4(1.0f, 0.0f, 0.0f, 1.0f),  // red
                                                       /*roughness=*/0.05f,
                                                       Vector3(-6.0f, 0.0f, -6.0f),
                                                       Vector3(6.0f, 0.0f, 6.0f)));
    render_scene->m_RenderEntities.push_back(MakeEntity(/*instance_id=*/2,
                                                       /*mesh_asset_id=*/2,
                                                       Vector4(0.0f, 0.0f, 1.0f, 1.0f),  // blue
                                                       /*roughness=*/0.05f,
                                                       Vector3(7.0f, 0.0f, -2.0f),
                                                       Vector3(10.0f, 0.0f, 2.0f)));

    auto camera = std::make_shared<RenderCamera>();
    camera->m_Position = Vector3(0.0f, 2.0f, 0.0f);

    // -------------------------------------------------------------------------
    // Wiring: distance field -> surface cache -> ray tracing -> reflections.
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

    LumenReflections reflections;
    Check(reflections.Initialize(nullptr, config), "reflections initialize without an RHI");
    reflections.setRayTracing(&ray_tracing);
    reflections.setDistanceField(&distance_field);

    const LumenSurfaceCachePage* floor_page = FindPageOfInstance(surface_cache, /*instance_id=*/1);
    Check(floor_page != nullptr, "the red floor got its own surface-cache page");
    if (floor_page == nullptr)
    {
        std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
        return 1;
    }

    // -------------------------------------------------------------------------
    // 0. Nothing has been computed yet.
    // -------------------------------------------------------------------------
    {
        Vector3 color(0.0f, 0.0f, 0.0f);
        uint32_t count = 0;
        Check(!reflections.GetAverageReflection(color, count),
              "a fresh LumenReflections has no average reflection");
        Check(count == 0, "...and reports zero samples");
    }

    // -------------------------------------------------------------------------
    // 1. ComputeReflection actually traces -- and keeps the direction it computed.
    //
    // The mirror direction is straight down (camera above a +Y floor), so the
    // ray lands back on the floor: a red hit at roughly the offset distance.
    // -------------------------------------------------------------------------
    {
        const Vector3 to_camera = camera->position() - floor_page->world_position;
        const Vector3 view_direction = to_camera.normalisedCopy();
        const LumenReflectionData result = reflections.ComputeReflection(
            floor_page->world_position, floor_page->normal, view_direction, floor_page->roughness, &ray_tracing);

        Check(result.valid, "ComputeReflection reports a hit on a floor that is right there");
        Check(result.reflection_color.x > result.reflection_color.z,
              "the reflection picks up the floor's red albedo");
        Check(result.reflection_distance > 0.0f, "the hit distance is positive");

        // 以前这个字段被 BlendReflections() 的返回值整体覆盖掉（它从不设置方向）
        const Vector3 expected_mirror =
            view_direction - floor_page->normal * 2.0f * view_direction.dotProduct(floor_page->normal);
        Check((result.reflection_direction - expected_mirror).length() < 1e-4f,
              "the mirror direction survives the blend (it used to be lost)");
        // 相机在 +Y，地板法线也是 +Y，镜像方向就是竖直向下
        Check(result.reflection_direction.y < -0.9f,
              "the mirror direction points straight back down at the floor");
    }

    // -------------------------------------------------------------------------
    // 2. Roughness spreads the ray, it does not dim the result.
    //
    // 以前：roughness 只用来把颜色乘以 (1 - roughness)，roughness = 1 时结果恒为
    // 纯黑 —— 而"粗糙度"物理上是把能量摊开，不是让它消失。
    // -------------------------------------------------------------------------
    {
        const Vector3 to_camera = camera->position() - floor_page->world_position;
        const Vector3 view_direction = to_camera.normalisedCopy();

        const LumenReflectionData rough = reflections.ComputeReflection(
            floor_page->world_position, floor_page->normal, view_direction, 1.0f, &ray_tracing);
        Check(rough.valid, "a fully rough surface still finds something");
        Check(rough.reflection_color.x > 0.0f,
              "roughness 1.0 no longer multiplies the radiance to exactly black");
    }

    // -------------------------------------------------------------------------
    // 3. The screen-space-reflection regression.
    //
    // 以前 BlendReflections 用 max_trace_distance（一个常量）挑分支：< 100 且
    // roughness < 0.3 时选中 SSR，而 SSR 是恒空的 TODO，于是已经追到的命中被换成黑色。
    // -------------------------------------------------------------------------
    {
        LumenConfig short_range = config;
        short_range.max_trace_distance = 50.0f;

        LumenReflections short_range_reflections;
        short_range_reflections.Initialize(nullptr, short_range);
        short_range_reflections.setRayTracing(&ray_tracing);
        short_range_reflections.setDistanceField(&distance_field);

        const Vector3 to_camera = camera->position() - floor_page->world_position;
        const Vector3 view_direction = to_camera.normalisedCopy();
        const LumenReflectionData result = short_range_reflections.ComputeReflection(
            floor_page->world_position, floor_page->normal, view_direction, 0.1f, &ray_tracing);

        Check(result.valid, "a 50-unit trace range still reflects (the SSR branch used to zero it)");
        Check(result.reflection_color.x > 0.0f,
              "...and the reflected colour is not black");
    }

    // -------------------------------------------------------------------------
    // 4. No ray tracing -> no result, and it must say so.
    // -------------------------------------------------------------------------
    {
        const Vector3 to_camera = camera->position() - floor_page->world_position;
        const Vector3 view_direction = to_camera.normalisedCopy();
        const LumenReflectionData result = reflections.ComputeReflection(
            floor_page->world_position, floor_page->normal, view_direction, 0.1f, nullptr);
        Check(!result.valid, "without ray tracing there is no reflection, and valid says so");

        // 无效结果不能因为"颜色是黑的"就被当成"命中了黑色的东西"
        LumenReflectionData zero {};
        Check(!zero.valid && zero.reflection_color == Vector3::ZERO,
              "a default LumenReflectionData is invalid (miss and black hit are distinguishable)");
    }

    // -------------------------------------------------------------------------
    // 5. UpdateReflections fills the accumulation, and the budget limits it.
    // -------------------------------------------------------------------------
    {
        LumenConfig budgeted = config;
        budgeted.reflection_update_budget = 1;
        reflections.setConfig(budgeted);

        Vector3 color(0.0f, 0.0f, 0.0f);
        uint32_t count = 0;

        reflections.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(reflections.GetAverageReflection(color, count), "one update produced an average reflection");
        Check(count == 1, "budget 1 updates exactly one instance per frame");

        reflections.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(reflections.GetAverageReflection(color, count) && count == 2,
              "the cursor advances, so the second frame updates the other instance");

        reflections.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(reflections.GetAverageReflection(color, count) && count == 2,
              "there are only two instances, so the count stops at 2");
    }

    // -------------------------------------------------------------------------
    // 6. Temporal accumulation: rate 0 freezes, a large rate catches up.
    //
    // 把地板改成蓝色重建表面缓存，再看已累积的值跟不跟着走。
    // -------------------------------------------------------------------------
    {
        LumenConfig frozen = config;
        frozen.reflection_temporal_blend_rate = 0.0f;
        frozen.reflection_update_budget = 16;
        reflections.setConfig(frozen);

        Vector3 color(0.0f, 0.0f, 0.0f);
        uint32_t count = 0;
        reflections.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(reflections.GetAverageReflection(color, count), "the frozen run still has a value");
        const float red_before = color.x;
        const float blue_before = color.z;

        // 重新上色：红地板 -> 蓝地板
        render_scene->m_RenderEntities[0].m_BaseColorFactor = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
        surface_cache.BuildFromScene(render_scene, &render_resource, camera->position());

        reflections.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        reflections.GetAverageReflection(color, count);
        Check(std::fabs(color.x - red_before) < 1e-5f && std::fabs(color.z - blue_before) < 1e-5f,
              "rate 0 freezes the accumulated reflection (recolouring the floor changes nothing)");

        LumenConfig fast = frozen;
        fast.reflection_temporal_blend_rate = 4.0f;
        reflections.setConfig(fast);
        // dt = 1s, rate = 4 -> alpha = 1 - exp(-4) ~= 0.98，一次更新几乎完全采信新值
        reflections.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f);
        reflections.GetAverageReflection(color, count);
        Check(color.z > color.x,
              "a large blend rate lets the reflection catch up with the new blue floor");
    }

    // -------------------------------------------------------------------------
    // 7. Inert inputs.
    // -------------------------------------------------------------------------
    {
        LumenReflections isolated;
        isolated.Initialize(nullptr, config);
        isolated.setRayTracing(&ray_tracing);
        isolated.setDistanceField(&distance_field);

        Vector3 color(0.0f, 0.0f, 0.0f);
        uint32_t count = 0;

        isolated.UpdateReflections(nullptr, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(!isolated.GetAverageReflection(color, count), "a null scene updates nothing");

        isolated.UpdateReflections(render_scene, nullptr, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(!isolated.GetAverageReflection(color, count), "a null camera updates nothing");

        isolated.UpdateReflections(render_scene, camera, nullptr, &surface_cache, 1.0f / 60.0f);
        Check(!isolated.GetAverageReflection(color, count), "no ray tracing updates nothing");

        // 相机太远：所有页面都被距离裁剪掉
        LumenConfig near_sighted = config;
        near_sighted.reflection_max_distance = 1.0f;
        isolated.setConfig(near_sighted);
        isolated.UpdateReflections(render_scene, camera, &ray_tracing, &surface_cache, 1.0f / 60.0f);
        Check(!isolated.GetAverageReflection(color, count),
              "a 1-unit max distance skips every page (the camera is 2 units up)");
    }

    // -------------------------------------------------------------------------
    // 8. clear() really empties it.
    // -------------------------------------------------------------------------
    {
        Vector3 color(0.0f, 0.0f, 0.0f);
        uint32_t count = 0;
        reflections.clear();
        Check(!reflections.GetAverageReflection(color, count), "clear() empties the accumulation");
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
