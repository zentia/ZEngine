// =============================================================================
// Lumen scene-geometry smoke test.
//
// LumenDistanceField::GenerateFromScene used to carry a TODO where the geometry was
// supposed to be gathered, so m_SceneGeometryCache stayed empty, every voxel kept its
// initial FLT_MAX and the software ray marcher could never hit anything -- GI was
// mathematically zero. This test pins the whole path end to end:
//
//   RenderResourceBase CPU geometry cache -> SceneGeometryCache -> voxel distances
//
// Headless: no RHI, no window, no project. The only engine pieces it touches are the
// CPU-side mesh cache and the distance field voxelizer.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_SCENE_GEOMETRY_SMOKE_TEST=ON
//   cmake --build build --target LumenSceneGeometrySmokeTest
// =============================================================================

#include "Runtime/Function/Render/Lumen/LumenDistanceField.h"
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

    // A single triangle, exactly as the loaders hand geometry over: a vertex buffer of
    // MeshVertexDataDefinition and a uint16_t index buffer.
    RenderMeshData MakeTriangle(const Vector3& v0, const Vector3& v1, const Vector3& v2)
    {
        RenderMeshData mesh_data;
        mesh_data.m_StaticMeshData.m_VertexBuffer =
            std::make_shared<BufferData>(3 * sizeof(MeshVertexDataDefinition));
        std::memset(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data,
                    0,
                    3 * sizeof(MeshVertexDataDefinition));
        MeshVertexDataDefinition* vertices =
            static_cast<MeshVertexDataDefinition*>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data);
        vertices[0].x = v0.x;
        vertices[0].y = v0.y;
        vertices[0].z = v0.z;
        vertices[1].x = v1.x;
        vertices[1].y = v1.y;
        vertices[1].z = v1.z;
        vertices[2].x = v2.x;
        vertices[2].y = v2.y;
        vertices[2].z = v2.z;

        mesh_data.m_StaticMeshData.m_IndexBuffer = std::make_shared<BufferData>(3 * sizeof(uint16_t));
        uint16_t* indices = static_cast<uint16_t*>(mesh_data.m_StaticMeshData.m_IndexBuffer->m_Data);
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;

        return mesh_data;
    }

    AxisAlignedBox MakeBox(const Vector3& min_corner, const Vector3& max_corner)
    {
        AxisAlignedBox box;
        box.Merge(min_corner);
        box.Merge(max_corner);
        return box;
    }

    bool NearlyEqual(float a, float b, float tolerance = 0.02f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    LumenConfig MakeConfig(uint32_t resolution)
    {
        LumenConfig config;
        config.distance_field_resolution = resolution;
        config.use_hierarchical_distance_field = true;
        return config;
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // -------------------------------------------------------------------------
    // 1. The CPU geometry cache actually retains something, and only what it needs.
    // -------------------------------------------------------------------------
    StubRenderResource render_resource;
    const AxisAlignedBox triangle_local_bounds = MakeBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 2.0f, 0.0f));
    render_resource.RegisterMesh(
        /*asset_id=*/1,
        MakeTriangle(Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 0.0f, 0.0f), Vector3(0.0f, 2.0f, 0.0f)),
        triangle_local_bounds);

    const CpuMeshGeometry* cached = render_resource.FindMeshGeometry(1);
    Check(cached != nullptr, "cpu cache: mesh asset id 1 is retrievable");
    Check(cached != nullptr && cached->positions.size() == 3, "cpu cache: 3 positions retained");
    Check(cached != nullptr && cached->indices.size() == 3, "cpu cache: 3 indices retained");
    Check(cached != nullptr && NearlyEqual(cached->positions[1].x, 2.0f), "cpu cache: positions come from the asset");
    Check(cached != nullptr && cached->local_bounds.getMaxCorner().y == 2.0f, "cpu cache: local bounds retained");
    Check(render_resource.FindMeshGeometry(2) == nullptr, "cpu cache: unknown asset id resolves to nullptr");

    // -------------------------------------------------------------------------
    // 2. GenerateFromScene fills m_SceneGeometryCache.
    // -------------------------------------------------------------------------
    auto render_scene = std::make_shared<RenderScene>();

    RenderEntity entity_a;
    entity_a.m_InstanceId = 1;
    entity_a.m_MeshAssetId = 1;
    entity_a.m_ModelMatrix = Matrix4x4::IDENTITY;
    entity_a.m_BoundingBox = triangle_local_bounds;

    RenderEntity entity_b = entity_a;
    entity_b.m_InstanceId = 2;
    entity_b.m_ModelMatrix = Matrix4x4::GetTrans(Vector3(0.0f, 0.0f, 10.0f));

    // References an asset that was never uploaded: must be skipped, not crash.
    RenderEntity entity_missing;
    entity_missing.m_InstanceId = 3;
    entity_missing.m_MeshAssetId = 2;
    entity_missing.m_ModelMatrix = Matrix4x4::IDENTITY;

    render_scene->m_RenderEntities.push_back(entity_a);
    render_scene->m_RenderEntities.push_back(entity_b);
    render_scene->m_RenderEntities.push_back(entity_missing);

    LumenDistanceField distance_field;
    Check(distance_field.Initialize(nullptr, MakeConfig(16)), "initialize succeeds without an RHI");

    const AxisAlignedBox world_bounds = MakeBox(Vector3(-1.0f, -1.0f, -1.0f), Vector3(3.0f, 3.0f, 11.0f));
    Check(distance_field.GenerateFromScene(render_scene, &render_resource, world_bounds), "generateFromScene succeeds");

    const LumenDistanceField::SceneGeometryCache& cache = distance_field.getSceneGeometryCache();
    Check(cache.positions.size() == 3, "cache: geometry is de-duplicated to one copy of the asset");
    Check(cache.indices.size() == 3, "cache: one triangle worth of indices");
    Check(cache.GetTriangleCount() == 1, "cache: triangle count is 1 even with two instances");
    Check(cache.GetInstanceCount() == 2, "cache: entity without geometry is skipped (2 of 3 collected)");
    Check(cache.ranges.size() == cache.transforms.size() && cache.ranges.size() == cache.bounds.size(),
          "cache: ranges/transforms/bounds stay parallel");
    Check(cache.ranges.size() == 2 && cache.ranges[0].vertex_offset == cache.ranges[1].vertex_offset &&
              cache.ranges[0].vertex_count == cache.ranges[1].vertex_count,
          "cache: both instances point at the same geometry range");
    Check(cache.geometry_hash != 0, "cache: geometry hash is computed");

    Check(cache.bounds.size() == 2 && NearlyEqual(cache.bounds[0].getMinCorner().z, 0.0f) &&
              NearlyEqual(cache.bounds[0].getMaxCorner().x, 2.0f),
          "cache: instance 0 bounds are the untransformed local AABB");
    Check(cache.bounds.size() == 2 && NearlyEqual(cache.bounds[1].getMinCorner().z, 10.0f) &&
              NearlyEqual(cache.bounds[1].getMaxCorner().z, 10.0f),
          "cache: instance 1 bounds are transformed into world space");

    // -------------------------------------------------------------------------
    // 3. The voxelized field is no longer all FLT_MAX (the actual bug).
    // -------------------------------------------------------------------------
    const LumenDistanceFieldData& field = distance_field.getDistanceField();
    Check(field.is_loaded, "field: marked loaded");
    Check(field.distance_data.size() == 16u * 16u * 16u, "field: distance data has one value per voxel");

    const float infinite = std::numeric_limits<float>::max();
    const size_t infinite_count =
        static_cast<size_t>(std::count(field.distance_data.begin(), field.distance_data.end(), infinite));
    Check(infinite_count == 0, "field: no voxel is left at FLT_MAX (geometry reached the voxelizer)");

    const float min_distance = *std::min_element(field.distance_data.begin(), field.distance_data.end());
    const float max_distance = *std::max_element(field.distance_data.begin(), field.distance_data.end());
    Check(min_distance < 0.5f, "field: some voxel sits on the surface (min distance is small)");
    Check(max_distance > 1.0f, "field: some voxel is far from the surface (field is not constant)");
    Check(max_distance < infinite, "field: every distance is finite");

    // Nearest-surface queries only work when the field holds real distances.
    //
    // (0.5, 0.5, 0.1) falls in the voxel centred on (0.625, 0.625, 0.125); that voxel
    // centre projects inside triangle A, so its distance is exactly the 0.125 z offset.
    // This is the check that catches DistanceToTriangle getting its barycentric signs
    // backwards: with (point - v0) instead of (v0 - point) the same query returns 0.89
    // (the distance all the way back to the vertex at the origin).
    const float near_surface = distance_field.QueryDistance(Vector3(0.5f, 0.5f, 0.1f));
    const float far_from_surface = distance_field.QueryDistance(Vector3(2.9f, 2.9f, 0.1f));
    Check(NearlyEqual(near_surface, 0.125f, 0.001f), "query: a point on the triangle reports the exact surface distance");
    Check(far_from_surface > near_surface, "query: a point away from the triangle reports a larger distance");

    Vector3 surface_position;
    Vector3 surface_normal;
    Check(distance_field.QueryNearestSurface(Vector3(0.5f, 0.5f, 0.1f), surface_position, surface_normal),
          "query: nearest surface resolves");

    Check(!field.hierarchy_levels.empty(), "field: hierarchical levels were built");

    // -------------------------------------------------------------------------
    // 4. geometry_hash drives the "scene did not change" short circuit.
    // -------------------------------------------------------------------------
    const uint64_t hash_before = cache.geometry_hash;

    // Same scene, same bounds -> should not rebuild (and must not fail).
    Check(distance_field.GenerateFromScene(render_scene, &render_resource, world_bounds),
          "re-generating an unchanged scene succeeds");
    Check(distance_field.getSceneGeometryCache().geometry_hash == hash_before,
          "re-generating an unchanged scene keeps the same hash");

    // Moving an instance must invalidate the hash.
    render_scene->m_RenderEntities[1].m_ModelMatrix = Matrix4x4::GetTrans(Vector3(0.0f, 0.0f, 4.0f));
    Check(distance_field.GenerateFromScene(render_scene, &render_resource, world_bounds),
          "re-generating after a transform change succeeds");
    Check(distance_field.getSceneGeometryCache().geometry_hash != hash_before,
          "moving an instance changes the geometry hash");
    Check(distance_field.getSceneGeometryCache().bounds[1].getMinCorner().z == 4.0f,
          "moving an instance moves its world AABB");

    // -------------------------------------------------------------------------
    // 5. An empty scene fails loudly instead of producing a field of FLT_MAX.
    // -------------------------------------------------------------------------
    LumenDistanceField empty_field;
    empty_field.Initialize(nullptr, MakeConfig(8));
    auto empty_scene = std::make_shared<RenderScene>();
    Check(!empty_field.GenerateFromScene(empty_scene, &render_resource, world_bounds),
          "an empty scene reports failure instead of a bogus field");
    Check(!empty_field.getDistanceField().is_loaded, "an empty scene leaves the field unloaded");

    // No render resource at all -> same failure, no crash.
    Check(!empty_field.GenerateFromScene(render_scene, nullptr, world_bounds),
          "a null render resource reports failure");

    // -------------------------------------------------------------------------
    // 6. clear() empties the cache.
    // -------------------------------------------------------------------------
    distance_field.clear();
    Check(distance_field.getSceneGeometryCache().IsEmpty() &&
              distance_field.getSceneGeometryCache().positions.empty() &&
              distance_field.getSceneGeometryCache().geometry_hash == 0,
          "clear() empties the scene geometry cache");

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
