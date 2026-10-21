// =============================================================================
// Nanite cluster-builder smoke test.
//
// NaniteClusterBuilder produced nothing. Not "a coarse approximation" -- nothing:
//
//   * BuildClustersFromMesh() cleared vertex_data/index_data at the top and then
//     never wrote them again. The mesh resource walked out with plausible-looking
//     cluster metadata (offsets and counts were computed correctly) and ZERO
//     geometry bytes. Any downstream consumer -- GPU upload, render batches,
//     visibility stats -- had nothing to read.
//
//   * RecursiveClusterSplit() returned early when depth hit kMaxHierarchyDepth,
//     silently dropping every triangle under that node. Deep meshes lost a chunk
//     of geometry with no error.
//
//   * BuildHierarchy() was an O(n^2) guess: each cluster picked as its parent the
//     first other cluster with a lower lod_level whose centre was within
//     2 * radius. Which cluster that is depends on array order, and because the
//     loop `break`s, a parent could collect the same child repeatedly. And
//     root_cluster_index was hard-coded to 0 -- but cluster 0 is the first LEAF
//     in depth-first order, never the root.
//
//   * error_metric was never assigned by anyone, and NaniteCluster had no default
//     member initialisers, so SelectLOD() read uninitialised memory.
//
// This test pins the four fixes plus the invariants the rest of Nanite will be
// built on: the hierarchy is a real tree, the leaf windows tile the geometry
// exactly once, and the error metric is monotone from leaf to root.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_NANITE_CLUSTER_SMOKE_TEST=ON
//   cmake --build build --target NaniteClusterBuilderSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Nanite/NaniteCluster.h"
#include "Runtime/Function/Render/Nanite/NaniteTypes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

    // A flat grid on the XZ plane: cells_per_side^2 quads = 2 * cells_per_side^2
    // triangles. Plenty of triangles to force real splitting, and every triangle
    // is the same size so nothing degenerates into a zero-area cluster.
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
                mesh.positions.push_back(Vector3(origin + static_cast<float>(x) * step, 0.0f,
                                                 origin + static_cast<float>(z) * step));
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

    // 越界时返回零而不是读 nullptr：字节段为空是这次要抓的回归，让它报一条干净的
    // FAIL，而不是在 memcpy 里崩掉把整个测试的输出一起带走。
    Vector3 ReadVertex(const NaniteMeshResource& mesh, uint32_t index)
    {
        Vector3 value(0.0f, 0.0f, 0.0f);
        const size_t offset = static_cast<size_t>(index) * sizeof(Vector3);
        if (offset + sizeof(Vector3) <= mesh.vertex_data.size())
        {
            std::memcpy(&value, mesh.vertex_data.data() + offset, sizeof(Vector3));
        }
        return value;
    }

    uint32_t ReadIndex(const NaniteMeshResource& mesh, uint32_t index)
    {
        uint32_t value = 0;
        const size_t offset = static_cast<size_t>(index) * sizeof(uint32_t);
        if (offset + sizeof(uint32_t) <= mesh.index_data.size())
        {
            std::memcpy(&value, mesh.index_data.data() + offset, sizeof(uint32_t));
        }
        return value;
    }

    // The whole point of this test: the byte segments used to be empty.
    void CheckGeometryBytes(const NaniteMeshResource& mesh, const char* label)
    {
        Check(mesh.vertex_data.size() == static_cast<size_t>(mesh.total_vertex_count) * sizeof(Vector3),
              "vertex_data holds total_vertex_count Vector3s (was: cleared and never filled)");
        Check(mesh.index_data.size() == static_cast<size_t>(mesh.total_triangle_count) * 3 * sizeof(uint32_t),
              "index_data holds total_triangle_count * 3 uint32s (was: cleared and never filled)");
        (void)label;
    }
}  // namespace

int main()
{
    // ---------------------------------------------------------------------
    // A grid big enough to split. 12x12 cells = 288 triangles.
    // ---------------------------------------------------------------------
    const TestMesh grid = MakeGrid(12, 12.0f);
    const uint32_t grid_triangles = static_cast<uint32_t>(grid.indices.size() / 3);

    NaniteMeshResource mesh;
    NaniteClusterBuilder builder;
    const bool built = builder.BuildClustersFromMesh(
        grid.positions, std::vector<Vector3>(), std::vector<Vector2>(), grid.indices, mesh, NaniteConfig());

    Check(built, "BuildClustersFromMesh succeeds on a 288-triangle grid");
    Check(!mesh.clusters.empty(), "clusters were produced");
    Check(mesh.total_cluster_count == mesh.clusters.size(), "total_cluster_count matches clusters.size()");
    Check(mesh.clusters.size() <= NaniteConstants::kMaxClustersPerMesh, "cluster count is within kMaxClustersPerMesh");

    // No triangle may be lost. Reaching kMaxHierarchyDepth used to `return`
    // without emitting anything, so this is the check that catches it.
    Check(mesh.total_triangle_count == grid_triangles,
          "every input triangle survived the split (was: deep nodes dropped their triangles)");
    Check(mesh.total_vertex_count == grid_triangles * 3,
          "vertices are emitted per-triangle (3 per triangle, no dedup)");

    CheckGeometryBytes(mesh, "grid");

    // ---------------------------------------------------------------------
    // Global bounds and the decoded geometry.
    // ---------------------------------------------------------------------
    bool bounds_cover_all = true;
    Vector3 decoded_sum(0.0f, 0.0f, 0.0f);
    for (uint32_t i = 0; i < mesh.total_vertex_count; ++i)
    {
        const Vector3 vertex = ReadVertex(mesh, i);
        decoded_sum += vertex;
        if (!mesh.global_bounds.Contains(vertex))
        {
            bounds_cover_all = false;
        }
    }
    Check(bounds_cover_all, "global_bounds contains every decoded vertex");

    Vector3 expected_sum(0.0f, 0.0f, 0.0f);
    for (uint32_t index : grid.indices)
    {
        expected_sum += grid.positions[index];
    }
    Check((decoded_sum - expected_sum).length() < 1e-3f,
          "the decoded vertices are the input triangles (position sums agree)");

    bool indices_sequential = true;
    for (uint32_t i = 0; i < mesh.total_triangle_count * 3; ++i)
    {
        if (ReadIndex(mesh, i) != i)
        {
            indices_sequential = false;
        }
    }
    Check(indices_sequential, "indices are the sequential 0..n-1 window (per-triangle vertices)");

    // ---------------------------------------------------------------------
    // Every cluster: sane window, sane metric, and leaves tile the geometry.
    // ---------------------------------------------------------------------
    uint32_t leaf_count = 0;
    uint32_t internal_count = 0;
    uint32_t leaf_triangle_sum = 0;
    bool windows_in_range = true;
    bool leaf_vertex_rule = true;
    bool leaf_triangle_limit = true;
    bool finite_metrics = true;
    bool positive_radius = true;
    bool error_covers_radius = true;
    bool leaf_has_no_children = true;
    bool internal_has_children = true;

    for (const NaniteCluster& cluster : mesh.clusters)
    {
        if (cluster.vertex_offset + cluster.vertex_count > mesh.total_vertex_count ||
            cluster.index_offset + cluster.triangle_count * 3 > mesh.total_triangle_count * 3)
        {
            windows_in_range = false;
        }

        if (!std::isfinite(cluster.error_metric) || cluster.error_metric <= 0.0f)
        {
            finite_metrics = false;
        }
        if (cluster.radius <= 0.0f || !std::isfinite(cluster.radius))
        {
            positive_radius = false;
        }
        if (cluster.error_metric + 1e-6f < cluster.radius)
        {
            error_covers_radius = false;
        }

        if (cluster.is_leaf)
        {
            ++leaf_count;
            leaf_triangle_sum += cluster.triangle_count;
            if (cluster.vertex_count != cluster.triangle_count * 3)
            {
                leaf_vertex_rule = false;
            }
            if (cluster.triangle_count > NaniteConstants::kMaxTrianglesPerCluster)
            {
                leaf_triangle_limit = false;
            }
            if (!cluster.child_cluster_indices.empty())
            {
                leaf_has_no_children = false;
            }
        }
        else
        {
            ++internal_count;
            if (cluster.child_cluster_indices.empty())
            {
                internal_has_children = false;
            }
        }
    }

    Check(windows_in_range, "every cluster's geometry window stays inside the buffers");
    Check(finite_metrics, "error_metric is assigned and finite on every cluster (was: uninitialised)");
    Check(positive_radius, "every cluster has a positive radius (OptimizeClusters floors degenerate ones)");
    Check(error_covers_radius, "error_metric >= radius on every cluster");
    Check(leaf_vertex_rule, "leaf clusters emit exactly 3 vertices per triangle");
    Check(leaf_triangle_limit, "leaf clusters stay within kMaxTrianglesPerCluster");
    Check(leaf_has_no_children, "leaf clusters have no children");
    Check(internal_has_children, "internal nodes have at least one child");
    Check(leaf_count >= 2 && internal_count >= 1, "the grid produced a real tree (leaves and internal nodes)");
    Check(leaf_triangle_sum == mesh.total_triangle_count,
          "leaf clusters tile the geometry exactly once (no overlaps, no gaps)");

    // ---------------------------------------------------------------------
    // The hierarchy is a real tree.
    // ---------------------------------------------------------------------
    Check(mesh.root_cluster_index != NaniteConstants::kInvalidClusterIndex, "root_cluster_index is set");
    Check(mesh.root_cluster_index < mesh.clusters.size(), "root_cluster_index is in range");

    const NaniteCluster& root = mesh.clusters[mesh.root_cluster_index];
    Check(root.lod_level == 0, "the root sits at lod_level 0 (was: root was hard-coded to cluster 0, a leaf)");
    Check(root.parent_cluster_index == NaniteConstants::kInvalidClusterIndex, "the root has no parent");
    Check(root.triangle_count == mesh.total_triangle_count,
          "the root aggregates the whole mesh (internal nodes carry a real window)");
    Check(root.vertex_count == mesh.total_vertex_count, "the root's vertex window covers every vertex");

    bool max_depth_matches = true;
    for (const NaniteCluster& cluster : mesh.clusters)
    {
        if (cluster.lod_level > mesh.hierarchy_depth)
        {
            max_depth_matches = false;
        }
    }
    Check(max_depth_matches, "hierarchy_depth is the deepest lod_level present");

    bool parent_links_consistent = true;
    bool parent_is_one_level_up = true;
    bool error_monotone = true;
    uint32_t parents_seen = 0;

    for (uint32_t index = 0; index < mesh.clusters.size(); ++index)
    {
        const NaniteCluster& cluster = mesh.clusters[index];
        if (cluster.parent_cluster_index == NaniteConstants::kInvalidClusterIndex)
        {
            // Only the root may be parentless.
            if (index != mesh.root_cluster_index)
            {
                parent_links_consistent = false;
            }
            continue;
        }
        ++parents_seen;

        if (cluster.parent_cluster_index >= mesh.clusters.size())
        {
            parent_links_consistent = false;
            continue;
        }

        const NaniteCluster& parent = mesh.clusters[cluster.parent_cluster_index];

        // The parent must list this cluster, and sit exactly one level above.
        bool listed = false;
        for (uint32_t child : parent.child_cluster_indices)
        {
            if (child == index)
            {
                listed = true;
            }
        }
        if (!listed)
        {
            parent_links_consistent = false;
        }
        if (parent.lod_level + 1 != cluster.lod_level)
        {
            parent_is_one_level_up = false;
        }

        if (parent.error_metric + 1e-6f < cluster.error_metric)
        {
            error_monotone = false;
        }
    }

    Check(parent_links_consistent, "parent/child links agree in both directions (was: an O(n^2) guess)");
    Check(parents_seen == mesh.clusters.size() - 1, "every non-root cluster has a parent");
    Check(parent_is_one_level_up, "a parent sits one level above its children");
    Check(error_monotone, "error_metric is monotone: parent >= child (LOD selection depends on this)");

    // Leaf bounds must match the vertices in their window -- otherwise the
    // bounds are decorative and frustum culling would cull the wrong things.
    bool leaf_bounds_match = true;
    for (const NaniteCluster& cluster : mesh.clusters)
    {
        if (!cluster.is_leaf || cluster.vertex_count == 0)
        {
            continue;
        }

        Vector3 min_corner = ReadVertex(mesh, cluster.vertex_offset);
        Vector3 max_corner = min_corner;
        for (uint32_t i = 1; i < cluster.vertex_count; ++i)
        {
            const Vector3 vertex = ReadVertex(mesh, cluster.vertex_offset + i);
            min_corner.makeFloor(vertex);
            max_corner.makeCeil(vertex);
        }

        if ((cluster.bounds.getMinCorner() - min_corner).length() > 1e-4f ||
            (cluster.bounds.getMaxCorner() - max_corner).length() > 1e-4f)
        {
            leaf_bounds_match = false;
        }
    }
    Check(leaf_bounds_match, "leaf bounds match the vertices actually stored in their window");

    // ---------------------------------------------------------------------
    // A single triangle: no split, one leaf that is also the root.
    // ---------------------------------------------------------------------
    {
        NaniteMeshResource single;
        const std::vector<Vector3> positions = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f),
                                                Vector3(0.0f, 1.0f, 0.0f)};
        const std::vector<uint32_t> indices = {0, 1, 2};
        const bool ok = builder.BuildClustersFromMesh(
            positions, std::vector<Vector3>(), std::vector<Vector2>(), indices, single, NaniteConfig());

        Check(ok, "a single triangle builds");
        Check(single.clusters.size() == 1, "a single triangle produces exactly one cluster");
        Check(single.clusters[0].is_leaf, "that cluster is a leaf");
        Check(single.root_cluster_index == 0, "that cluster is also the root");
        Check(single.hierarchy_depth == 0, "hierarchy_depth is 0 for a single cluster");
        Check(single.total_triangle_count == 1 && single.total_vertex_count == 3,
              "the single triangle is counted once");
        Check(single.vertex_data.size() == 3 * sizeof(Vector3), "single-triangle vertex_data is 36 bytes");
        Check(single.index_data.size() == 3 * sizeof(uint32_t), "single-triangle index_data is 12 bytes");
        Check(ReadVertex(single, 1) == Vector3(1.0f, 0.0f, 0.0f),
              "the decoded geometry is the input geometry (vertex 1 round-trips)");
    }

    // ---------------------------------------------------------------------
    // Degenerate input is rejected instead of producing garbage.
    // ---------------------------------------------------------------------
    {
        NaniteMeshResource scratch;
        const std::vector<Vector3> three = {Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f),
                                            Vector3(0.0f, 1.0f, 0.0f)};

        Check(!builder.BuildClustersFromMesh(std::vector<Vector3>(),
                                             std::vector<Vector3>(),
                                             std::vector<Vector2>(),
                                             {0, 1, 2},
                                             scratch,
                                             NaniteConfig()),
              "empty positions are rejected");

        Check(!builder.BuildClustersFromMesh(three,
                                             std::vector<Vector3>(),
                                             std::vector<Vector2>(),
                                             std::vector<uint32_t>(),
                                             scratch,
                                             NaniteConfig()),
              "empty indices are rejected");

        Check(!builder.BuildClustersFromMesh(three,
                                             std::vector<Vector3>(),
                                             std::vector<Vector2>(),
                                             {0, 1},
                                             scratch,
                                             NaniteConfig()),
              "a non-multiple-of-3 index count is rejected");

        Check(!builder.BuildClustersFromMesh(three,
                                             std::vector<Vector3>(),
                                             std::vector<Vector2>(),
                                             {0, 1, 7},
                                             scratch,
                                             NaniteConfig()),
              "an out-of-range index is rejected (was: read out of bounds)");
    }

    // ---------------------------------------------------------------------
    // The manager serves what the builder produced.
    // ---------------------------------------------------------------------
    {
        NaniteClusterManager manager;
        Check(manager.Initialize(nullptr, NaniteConfig()), "the cluster manager initialises without an RHI");

        const uint64_t mesh_id = 42;
        Check(manager.RegisterMeshResource(mesh_id, mesh), "RegisterMeshResource accepts the mesh");
        Check(manager.GetCluster(mesh_id, 0) != nullptr, "GetCluster returns the first cluster");
        Check(manager.GetCluster(mesh_id, mesh.clusters.size()) == nullptr,
              "GetCluster returns nullptr past the end");
        Check(manager.GetCluster(mesh_id + 1, 0) == nullptr, "GetCluster returns nullptr for an unknown mesh");
        Check(manager.GetVisibleClusters(mesh_id).empty(), "no clusters are visible before any culling runs");

        manager.UnregisterMeshResource(mesh_id);
        Check(manager.GetCluster(mesh_id, 0) == nullptr, "UnregisterMeshResource drops the mesh");

        manager.clear();
        Check(manager.GetCluster(mesh_id, 0) == nullptr, "clear() empties the manager");
    }

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
