// =============================================================================
// Nanite culling / LOD smoke test.
//
// Everything in NaniteCullingSystem that mattered was a stub:
//
//   * FrustumCull() was `return true;` with two TODOs inside -- and its single
//     caller treated the result as "visible", while OcclusionCull() in the same
//     file returns "culled". Two opposite conventions, one file.
//   * The instance bounds it was handed were a default-constructed box
//     (`AxisAlignedBox instance_bounds;  // 需要从mesh资源获取`), so even a
//     working frustum test would have had nothing to test.
//   * ExtractFrustumPlanes() / PointFrustumTest() / AabbFrustumTest() were
//     private and had zero callers -- which is why nobody noticed that the plane
//     extraction uses the Ogre / column-major form on matrices that are
//     row-major (`m_Mat[row][col]`, `clip = M * v`). Six planes, all wrong.
//   * SelectLOD() moved the level the WRONG WAY (small screen error => finer,
//     large => coarser), divided by a distance that can be zero, and read the
//     member m_Config instead of its own `config` argument.
//   * There was no cluster-level culling at all, and BuildRenderBatches()
//     emitted batches with cluster_count = 0 -- the only thing it ever produced.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_NANITE_CULLING_SMOKE_TEST=ON
//   cmake --build build --target NaniteCullingSmokeTest
// =============================================================================

#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Render/Nanite/NaniteCluster.h"
#include "Runtime/Function/Render/Nanite/NaniteCulling.h"
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#include "Runtime/Function/Render/Nanite/NaniteTypes.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

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

    struct TestMesh
    {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
    };

    TestMesh MakeGrid(uint32_t cells_per_side, float extent)
    {
        TestMesh mesh;
        const uint32_t vertex_side = cells_per_side + 1;
        const float step = extent / static_cast<float>(cells_per_side);
        const float origin = -extent * 0.5f;

        for (uint32_t z = 0; z < vertex_side; ++z)
        {
            for (uint32_t x = 0; x < vertex_side; ++x)
            {
                mesh.positions.push_back(
                    Vector3(origin + static_cast<float>(x) * step, 0.0f, origin + static_cast<float>(z) * step));
            }
        }

        for (uint32_t z = 0; z < cells_per_side; ++z)
        {
            for (uint32_t x = 0; x < cells_per_side; ++x)
            {
                const uint32_t i0 = z * vertex_side + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + vertex_side;
                const uint32_t i3 = i2 + 1;

                mesh.indices.push_back(i0);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i1);
                mesh.indices.push_back(i2);
                mesh.indices.push_back(i3);
            }
        }

        return mesh;
    }

    // A right-handed view/projection pair, built by hand so the frustum maths is
    // pinned independently of RenderCamera's own state machine.
    Matrix4x4 MakeTestViewProjection()
    {
        const Matrix4x4 view = Math::MakeLookAtMatrix(Vector3(0.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f),
                                                      Vector3(0.0f, 1.0f, 0.0f));
        const Matrix4x4 proj = Math::MakePerspectiveMatrix(Radian(Degree(60.0f)), 16.0f / 9.0f, 0.1f, 1000.0f);
        return proj * view;
    }

    // 在 z = 0 平面上，距离相机 10，半宽 = 10 * tan(30°) * aspect ≈ 10.26。
    constexpr float kHalfWidthAtOrigin = 10.26f;

    std::shared_ptr<RenderCamera> MakeCamera(const Vector3& eye, const Vector3& target)
    {
        auto camera = std::make_shared<RenderCamera>();
        camera->m_Position = eye;
        camera->LookAt(eye, target, Vector3(0.0f, 1.0f, 0.0f));
        camera->SetAspect(16.0f / 9.0f);
        camera->setFOVx(60.0f);
        camera->m_Znear = 0.1f;
        camera->m_Zfar = 1000.0f;
        return camera;
    }
}  // namespace

int main()
{
    NaniteCullingSystem culling;
    Check(culling.Initialize(nullptr, NaniteConfig()), "the culling system initialises without an RHI");

    // ---------------------------------------------------------------------
    // Frustum planes: the point tests are what make the extraction falsifiable.
    // ---------------------------------------------------------------------
    const Matrix4x4 view_projection = MakeTestViewProjection();

    std::vector<Vector4> planes;
    culling.ExtractFrustumPlanes(view_projection, planes);
    Check(planes.size() == 6, "six planes were extracted");

    bool planes_finite = true;
    bool planes_normalised = true;
    for (const Vector4& plane : planes)
    {
        if (!std::isfinite(plane.x) || !std::isfinite(plane.y) || !std::isfinite(plane.z) || !std::isfinite(plane.w))
        {
            planes_finite = false;
        }
        // (a,b,c) must be unit length, or "distance" is not a distance
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (std::fabs(length - 1.0f) > 1e-3f)
        {
            planes_normalised = false;
        }
    }
    Check(planes_finite, "every plane is finite (was: whatever the Ogre-form extraction produced)");
    Check(planes_normalised, "the (a,b,c) part of every plane is unit length");

    Check(culling.PointFrustumTest(Vector3(0.0f, 0.0f, 0.0f), planes) == 1,
          "the look-at target tests inside the frustum");
    Check(culling.PointFrustumTest(Vector3(0.0f, 0.0f, 20.0f), planes) == -1,
          "a point behind the camera tests outside (was: the near/far planes were meaningless)");
    Check(culling.PointFrustumTest(Vector3(0.0f, 0.0f, -2000.0f), planes) == -1,
          "a point past the far plane tests outside");
    Check(culling.PointFrustumTest(Vector3(1.0f, 0.0f, 0.0f), planes) == 1, "a point near the axis tests inside");
    Check(culling.PointFrustumTest(Vector3(100.0f, 0.0f, 0.0f), planes) == -1,
          "a point far off to the side tests outside");

    // ---------------------------------------------------------------------
    // AABB vs frustum: inside / outside / straddling
    // ---------------------------------------------------------------------
    Check(culling.AabbFrustumTest(AxisAlignedBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f)),
                                  Matrix4x4::IDENTITY,
                                  planes) == 1,
          "a box at the origin is fully inside");
    Check(culling.AabbFrustumTest(AxisAlignedBox(Vector3(0.0f, 0.0f, 20.0f), Vector3(0.5f, 0.5f, 0.5f)),
                                  Matrix4x4::IDENTITY,
                                  planes) == -1,
          "a box behind the camera is fully outside");
    // 跨在右平面上（半宽 ≈ 10.26）：既不是全内也不是全外
    Check(culling.AabbFrustumTest(
              AxisAlignedBox(Vector3(kHalfWidthAtOrigin, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)),
              Matrix4x4::IDENTITY,
              planes) == 0,
          "a box straddling the right plane reports partial");

    // ---------------------------------------------------------------------
    // FrustumCull: "true = culled", and it actually culls now.
    // ---------------------------------------------------------------------
    const AxisAlignedBox at_origin(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
    const AxisAlignedBox behind(Vector3(0.0f, 0.0f, 20.0f), Vector3(1.0f, 1.0f, 1.0f));

    Check(!culling.FrustumCull(at_origin, Matrix4x4::IDENTITY, view_projection),
          "a box in front of the camera is NOT culled (was: the function was `return true;`)");
    Check(culling.FrustumCull(behind, Matrix4x4::IDENTITY, view_projection),
          "a box behind the camera IS culled");
    Check(!culling.FrustumCull(AxisAlignedBox(), Matrix4x4::IDENTITY, view_projection),
          "an unknown (invalid) box is not culled -- culling what you cannot measure is worse");
    Check(!culling.FrustumCull(at_origin, Matrix4x4::IDENTITY, std::shared_ptr<RenderCamera>()),
          "a null camera culls nothing");

    // The same answer through the camera overload, so the VP wiring is pinned too.
    auto camera = MakeCamera(Vector3(0.0f, 0.0f, 10.0f), Vector3(0.0f, 0.0f, 0.0f));
    Check(!culling.FrustumCull(at_origin, Matrix4x4::IDENTITY, camera),
          "the camera overload agrees: a box in front is kept");
    Check(culling.FrustumCull(behind, Matrix4x4::IDENTITY, camera),
          "the camera overload agrees: a box behind is culled");

    // ---------------------------------------------------------------------
    // Build a mesh so there are real clusters with a real error metric.
    // ---------------------------------------------------------------------
    const TestMesh grid = MakeGrid(12, 12.0f);

    NaniteMeshResource mesh;
    NaniteClusterBuilder builder;
    Check(builder.BuildClustersFromMesh(
              grid.positions, std::vector<Vector3>(), std::vector<Vector2>(), grid.indices, mesh, NaniteConfig()),
          "the test mesh built");

    const NaniteCluster* first_leaf = nullptr;
    const NaniteCluster* an_internal_node = nullptr;
    for (const NaniteCluster& cluster : mesh.clusters)
    {
        if (cluster.is_leaf && first_leaf == nullptr)
        {
            first_leaf = &cluster;
        }
        if (!cluster.is_leaf && an_internal_node == nullptr)
        {
            an_internal_node = &cluster;
        }
    }
    Check(first_leaf != nullptr, "the mesh has a leaf cluster to test LOD against");
    Check(an_internal_node != nullptr, "the mesh has an internal node to test LOD against");

    // ---------------------------------------------------------------------
    // SelectLOD: direction, the divide-by-zero, and which config it reads.
    // ---------------------------------------------------------------------
    NaniteConfig lod_config;
    lod_config.max_screen_error = 1.0f;

    const Vector3 cluster_world = first_leaf->center;
    const Vector3 very_near = cluster_world + Vector3(0.0f, 0.01f, 0.0f);
    const Vector3 very_far = cluster_world + Vector3(0.0f, 0.0f, 100000.0f);

    Check(culling.SelectLOD(*first_leaf, very_near, Matrix4x4::IDENTITY, lod_config) > first_leaf->lod_level,
          "up close the selection asks for a FINER level (was: the branches were reversed)");
    Check(culling.SelectLOD(*first_leaf, very_far, Matrix4x4::IDENTITY, lod_config) < first_leaf->lod_level ||
              first_leaf->lod_level == 0,
          "far away the selection asks for a COARSER level (was: the branches were reversed)");

    // 相机正好落在集群中心：以前 error/distance 会得到 inf，判据彻底失效。
    Check(culling.SelectLOD(*first_leaf, cluster_world, Matrix4x4::IDENTITY, lod_config) == first_leaf->lod_level,
          "a zero distance keeps the cluster's own level instead of dividing by zero");

    NaniteConfig lod_off = lod_config;
    lod_off.enable_lod = false;
    Check(culling.SelectLOD(*first_leaf, very_far, Matrix4x4::IDENTITY, lod_off) == first_leaf->lod_level,
          "with LOD off the level is left alone (was: it returned 0, i.e. 'as coarse as possible')");

    // **读参数而不是成员**：两个只差 max_screen_error 的 config 必须给出不同结果。
    NaniteConfig picky = lod_config;
    picky.max_screen_error = 1e-6f;
    NaniteConfig sloppy = lod_config;
    sloppy.max_screen_error = 1e6f;
    Check(culling.SelectLOD(*first_leaf, very_near, Matrix4x4::IDENTITY, picky) >
              culling.SelectLOD(*first_leaf, very_near, Matrix4x4::IDENTITY, sloppy),
          "the config ARGUMENT drives the result (was: it read the member m_Config)");

    // ---------------------------------------------------------------------
    // IsClusterVisible
    // ---------------------------------------------------------------------
    Check(culling.IsClusterVisible(*first_leaf, very_near, Matrix4x4::IDENTITY, lod_config),
          "a leaf up close is visible");
    Check(!culling.IsClusterVisible(*first_leaf, very_far, Matrix4x4::IDENTITY, lod_config),
          "a leaf far away is dropped -- the only honest LOD we have without a simplifier");
    Check(!culling.IsClusterVisible(*an_internal_node, very_near, Matrix4x4::IDENTITY, lod_config),
          "an internal node is never submitted (its geometry window is just its subtree's aggregate)");

    // ---------------------------------------------------------------------
    // End to end through the renderer.
    // ---------------------------------------------------------------------
    {
        NaniteConfig render_config;
        render_config.use_gpu_culling = false;
        render_config.max_streaming_distance = 10000.0f;

        NaniteRenderer renderer;
        Check(renderer.Initialize(nullptr, nullptr, render_config), "the renderer initialises without an RHI");
        Check(renderer.RegisterMeshResource(1, mesh), "the mesh resource registers");

        NaniteInstance front;
        front.instance_id = 1;
        front.mesh_id = 1;
        front.transform = Matrix4x4::IDENTITY;
        renderer.RegisterInstance(front);

        NaniteInstance back = front;
        back.instance_id = 2;
        back.transform = Matrix4x4::IDENTITY;
        back.transform.setTrans(Vector3(0.0f, 0.0f, 40.0f));
        renderer.RegisterInstance(back);

        // 注册时没填 local_bounds ⇒ 渲染器从网格资源补上。以前裁剪拿到的是空盒。
        Check(renderer.GetInstances()[0].local_bounds.IsValid(),
              "RegisterInstance fills local_bounds from the mesh resource (was: an empty box)");

        renderer.Render(nullptr, nullptr, camera, render_config);

        const NaniteVisibilityResult& visibility = renderer.GetLastVisibilityResult();
        Check(visibility.visible_instance_count == 1,
              "only the instance in front of the camera survives (was: FrustumCull returned true for everything)");
        Check(visibility.visible_instance_indices.size() == 1 && visibility.visible_instance_indices[0] == 0,
              "the surviving instance is the one at the origin");

        const std::vector<NaniteRenderBatch>& batches = renderer.GetRenderBatches();
        Check(!batches.empty(), "batches were produced");

        bool no_empty_batches = true;
        bool ranges_in_bounds = true;
        uint32_t counted = 0;
        for (const NaniteRenderBatch& batch : batches)
        {
            if (batch.cluster_count == 0)
            {
                no_empty_batches = false;
            }
            if (batch.cluster_start + batch.cluster_count > renderer.GetVisibleClusters().size())
            {
                ranges_in_bounds = false;
            }
            counted += batch.cluster_count;
        }
        Check(no_empty_batches, "no batch has cluster_count == 0 (was: that was the only output)");
        Check(ranges_in_bounds, "every batch range stays inside the visible-cluster array");
        Check(counted == renderer.GetVisibleClusters().size(), "the batches cover the visible clusters exactly once");
        Check(renderer.GetVisibleClusters().size() <= mesh.total_cluster_count,
              "the visible clusters are a subset of the mesh's clusters");

        bool all_leaves = true;
        for (uint32_t cluster_index : renderer.GetVisibleClusters())
        {
            if (!mesh.clusters[cluster_index].is_leaf)
            {
                all_leaves = false;
            }
        }
        Check(all_leaves, "every visible cluster is a leaf");

        // 相机转到侧面（+X）⇒ 两个实例都偏离视锥轴 90°，什么都不该剩。
        // 这是"裁剪是真的"的最强证据。注意别往 +Y 转：那会让 LookAt 的 up 与视线共线，
        // 叉积退化为零向量。
        auto looking_away = MakeCamera(Vector3(0.0f, 0.0f, 10.0f), Vector3(1000.0f, 0.0f, 10.0f));
        renderer.Render(nullptr, nullptr, looking_away, render_config);
        Check(renderer.GetLastVisibilityResult().visible_instance_count == 0,
              "turning the camera away culls everything");
        Check(renderer.GetRenderBatches().empty(), "no batches are emitted when nothing is visible");

        // 关掉 LOD ⇒ 所有叶子都该留下（数量只增不减）。
        NaniteConfig lod_off_render = render_config;
        lod_off_render.enable_lod = false;
        renderer.Render(nullptr, nullptr, camera, lod_off_render);
        const uint32_t clusters_with_lod_on = [&]() {
            NaniteConfig with_lod = render_config;
            with_lod.enable_lod = true;
            renderer.Render(nullptr, nullptr, camera, with_lod);
            return static_cast<uint32_t>(renderer.GetVisibleClusters().size());
        }();
        const uint32_t clusters_with_lod_off = static_cast<uint32_t>(renderer.GetVisibleClusters().size());
        renderer.Render(nullptr, nullptr, camera, lod_off_render);
        const uint32_t clusters_lod_off_again = static_cast<uint32_t>(renderer.GetVisibleClusters().size());

        Check(clusters_with_lod_off == clusters_lod_off_again, "the LOD-off result is stable across frames");
        Check(clusters_with_lod_off >= clusters_with_lod_on,
              "LOD off keeps at least as many clusters as LOD on");
        Check(clusters_with_lod_off == clusters_lod_off_again && clusters_lod_off_again > 0,
              "LOD off still produces clusters to draw");

        // 裁剪结果写回了集群管理器（那个字段以前从来没人写过）。
        Check(!renderer.GetRenderBatches().empty(), "batches exist after the final pass");
    }

    {
        // 注册即驻留：没有流式加载，IsClusterLoaded 不该永远 false。
        NaniteClusterManager manager;
        manager.Initialize(nullptr, NaniteConfig());
        manager.RegisterMeshResource(7, mesh);
        Check(manager.IsClusterLoaded(7, 0), "a registered mesh's clusters count as loaded");
        Check(!manager.IsClusterLoaded(7, mesh.clusters.size()), "an out-of-range cluster is not loaded");
        Check(manager.GetClusterCount(7) == mesh.total_cluster_count, "GetClusterCount reports the mesh");
        Check(manager.GetMeshResource(7) != nullptr, "GetMeshResource returns the registered mesh");
        Check(manager.GetMeshResource(8) == nullptr, "GetMeshResource returns nullptr for an unknown mesh");

        manager.SetVisibleClusters(7, {0, 1});
        Check(manager.GetVisibleClusters(7).size() == 2, "visible clusters can be written back (was: never written)");
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
