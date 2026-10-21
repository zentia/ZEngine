#include "NaniteCluster.h"

#include "NaniteTypes.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// 把几何写進资源的字节段。压缩仍是 TODO，所以现在存的就是未压缩的原始布局
// （见 NaniteTypes.h 里 NaniteMeshResource 的注释）。
//
// **以前这两个字节段在函数开头被 clear 之后就再也没人写过** —— 于是
// NaniteMeshResource 带着一堆集群元数据（offset/count 都算得挺对）却**零几何字节**，
// 任何下游（GPU 上传、渲染批次、可见性统计）拿到的都是空。
void AppendGeometryBytes(const std::vector<Vector3>& positions,
                         const std::vector<uint32_t>& indices,
                         NaniteMeshResource& out_mesh_resource)
{
    out_mesh_resource.vertex_data.resize(positions.size() * sizeof(Vector3));
    if (!positions.empty())
    {
        std::memcpy(out_mesh_resource.vertex_data.data(), positions.data(), out_mesh_resource.vertex_data.size());
    }

    out_mesh_resource.index_data.resize(indices.size() * sizeof(uint32_t));
    if (!indices.empty())
    {
        std::memcpy(out_mesh_resource.index_data.data(), indices.data(), out_mesh_resource.index_data.size());
    }
}
}  // namespace

// ==================== NaniteClusterBuilder ====================

bool NaniteClusterBuilder::BuildClustersFromMesh(const std::vector<Vector3>& positions,
                                                 const std::vector<Vector3>& normals,
                                                 const std::vector<Vector2>& uvs,
                                                 const std::vector<uint32_t>& indices,
                                                 NaniteMeshResource& out_mesh_resource,
                                                 const NaniteConfig& config)
{
    if (positions.empty() || indices.empty() || (indices.size() % 3) != 0)
    {
        return false;
    }

    // 越界索引以前会一路走到 positions[indices[i]] 读越界，显式拒掉。
    for (uint32_t index : indices)
    {
        if (index >= positions.size())
        {
            return false;
        }
    }

    out_mesh_resource.clusters.clear();
    out_mesh_resource.cluster_groups.clear();
    out_mesh_resource.vertex_data.clear();
    out_mesh_resource.index_data.clear();
    out_mesh_resource.root_cluster_index = NaniteConstants::kInvalidClusterIndex;
    out_mesh_resource.hierarchy_depth = 0;
    out_mesh_resource.total_vertex_count = 0;
    out_mesh_resource.total_triangle_count = 0;
    out_mesh_resource.total_cluster_count = 0;

    // 计算全局边界
    Vector3 min_pos = positions[0];
    Vector3 max_pos = positions[0];
    for (const auto& pos : positions)
    {
        min_pos.makeFloor(pos);
        max_pos.makeCeil(pos);
    }
    out_mesh_resource.global_bounds = AxisAlignedBox((min_pos + max_pos) * 0.5f, (max_pos - min_pos) * 0.5f);

    // 递归分割网格
    std::vector<Vector3> cluster_positions;
    std::vector<uint32_t> cluster_indices;
    const uint32_t root_cluster_index = RecursiveClusterSplit(positions,
                                                              indices,
                                                              out_mesh_resource.global_bounds,
                                                              0,
                                                              NaniteConstants::kMaxHierarchyDepth,
                                                              out_mesh_resource.clusters,
                                                              cluster_positions,
                                                              cluster_indices);
    if (root_cluster_index == NaniteConstants::kInvalidClusterIndex)
    {
        return false;
    }
    out_mesh_resource.root_cluster_index = root_cluster_index;

    // 构建层次结构
    if (!BuildHierarchy(out_mesh_resource))
    {
        return false;
    }

    // 规范化集群（半径下限、不变量校验）
    OptimizeClusters(out_mesh_resource);

    // 计算统计信息
    out_mesh_resource.total_vertex_count = static_cast<uint32_t>(cluster_positions.size());
    out_mesh_resource.total_triangle_count = static_cast<uint32_t>(cluster_indices.size() / 3);
    out_mesh_resource.total_cluster_count = static_cast<uint32_t>(out_mesh_resource.clusters.size());

    // 压缩并存储几何数据
    // TODO: 实现几何压缩（在此之前 AppendGeometryBytes 存的是未压缩的原始布局）
    AppendGeometryBytes(cluster_positions, cluster_indices, out_mesh_resource);

    return true;
}

uint32_t NaniteClusterBuilder::RecursiveClusterSplit(const std::vector<Vector3>& positions,
                                                     const std::vector<uint32_t>& indices,
                                                     const AxisAlignedBox& bounds,
                                                     uint32_t depth,
                                                     uint32_t max_depth,
                                                     std::vector<NaniteCluster>& out_clusters,
                                                     std::vector<Vector3>& out_positions,
                                                     std::vector<uint32_t>& out_indices)
{
    if (indices.empty())
    {
        return NaniteConstants::kInvalidClusterIndex;
    }

    // 这个节点覆盖的几何区间。深度优先的遍历顺序保证它是**连续**的一段，所以内部
    // 节点只要记下起止，就能把自己子树里的几何整段聚合起来。
    const uint32_t vertex_begin = static_cast<uint32_t>(out_positions.size());
    const uint32_t index_begin = static_cast<uint32_t>(out_indices.size());

    const uint32_t triangle_count = static_cast<uint32_t>(indices.size() / 3);

    // 触发叶子：三角形已经够少，或者深度到底。
    // **以前"深度到底"是直接 return**，那一整棵子树的三角形被静默丢掉 ——
    // 网格超过 16 层分割时（kMaxHierarchyDepth）产出的几何会少一块。
    const bool make_leaf = (depth >= max_depth) || (triangle_count <= NaniteConstants::kMaxTrianglesPerCluster);

    if (make_leaf)
    {
        NaniteCluster cluster;
        cluster.vertex_count = 0;
        cluster.triangle_count = triangle_count;
        cluster.vertex_offset = vertex_begin;
        cluster.index_offset = index_begin;
        cluster.lod_level = static_cast<uint8_t>(std::min<uint32_t>(depth, 255u));
        cluster.parent_cluster_index = NaniteConstants::kInvalidClusterIndex;
        cluster.material_id = 0;
        cluster.is_visible = false;
        cluster.is_loaded = false;
        cluster.is_leaf = true;

        // 计算边界
        CalculateClusterBounds(positions, indices, 0, static_cast<uint32_t>(indices.size()), cluster);

        // 复制几何数据。每个三角形独占三个顶点（不做去重），所以
        // vertex_count == triangle_count * 3，索引也一定是 0,1,2,... 的连续序列。
        for (uint32_t i = 0; i < indices.size(); i += 3)
        {
            uint32_t idx0 = indices[i];
            uint32_t idx1 = indices[i + 1];
            uint32_t idx2 = indices[i + 2];

            // 检查顶点是否已存在（简单去重）
            // TODO: 实现更高效的顶点去重

            out_indices.push_back(static_cast<uint32_t>(out_positions.size()));
            out_positions.push_back(positions[idx0]);
            cluster.vertex_count++;

            out_indices.push_back(static_cast<uint32_t>(out_positions.size()));
            out_positions.push_back(positions[idx1]);
            cluster.vertex_count++;

            out_indices.push_back(static_cast<uint32_t>(out_positions.size()));
            out_positions.push_back(positions[idx2]);
            cluster.vertex_count++;
        }

        cluster.error_metric = CalculateClusterError(cluster);

        const uint32_t cluster_index = static_cast<uint32_t>(out_clusters.size());
        out_clusters.push_back(cluster);
        return cluster_index;
    }

    // 否则，分割边界框
    Vector3 center = bounds.getCenter();
    Vector3 half_extent = bounds.getHalfExtent();

    // 找到最长的轴
    float max_extent = Vector3::GetMaxElement(half_extent);
    int split_axis = 0;
    if (half_extent.y > half_extent.x && half_extent.y > half_extent.z)
        split_axis = 1;
    else if (half_extent.z > half_extent.x)
        split_axis = 2;

    // 分割
    float split_pos = center[split_axis];
    std::vector<uint32_t> left_indices, right_indices;

    for (uint32_t i = 0; i < indices.size(); i += 3)
    {
        Vector3 v0 = positions[indices[i]];
        Vector3 v1 = positions[indices[i + 1]];
        Vector3 v2 = positions[indices[i + 2]];

        // 简单的分割策略：根据三角形中心点
        Vector3 tri_center = (v0 + v1 + v2) / 3.0f;
        if (tri_center[split_axis] < split_pos)
        {
            left_indices.push_back(indices[i]);
            left_indices.push_back(indices[i + 1]);
            left_indices.push_back(indices[i + 2]);
        }
        else
        {
            right_indices.push_back(indices[i]);
            right_indices.push_back(indices[i + 1]);
            right_indices.push_back(indices[i + 2]);
        }
    }

    // 递归处理左右两部分。各自返回它那棵子树的根 —— 那是当前节点**唯一**的直接
    // 子节点，子树里更深的节点由更深的那一层自己认领。
    uint32_t left_root_index = NaniteConstants::kInvalidClusterIndex;
    if (!left_indices.empty())
    {
        Vector3 left_min = center;
        left_min[split_axis] = center[split_axis] - half_extent[split_axis];
        Vector3 left_max = center;
        left_max[split_axis] = split_pos;
        AxisAlignedBox left_bounds((left_min + left_max) * 0.5f, (left_max - left_min) * 0.5f);

        left_root_index = RecursiveClusterSplit(
            positions, left_indices, left_bounds, depth + 1, max_depth, out_clusters, out_positions, out_indices);
    }

    uint32_t right_root_index = NaniteConstants::kInvalidClusterIndex;
    if (!right_indices.empty())
    {
        Vector3 right_min = center;
        right_min[split_axis] = split_pos;
        Vector3 right_max = center;
        right_max[split_axis] = center[split_axis] + half_extent[split_axis];
        AxisAlignedBox right_bounds((right_min + right_max) * 0.5f, (right_max - right_min) * 0.5f);

        right_root_index = RecursiveClusterSplit(
            positions, right_indices, right_bounds, depth + 1, max_depth, out_clusters, out_positions, out_indices);
    }

    // 后序：子节点都建好了，才为当前节点建一个内部节点。它的几何区间就是
    // 子树里那段连续的窗口；它自己不emit任何顶点，所以 total_*_count 不会被重复计。
    //
    // 子节点只能是**两次递归各自返回的那个根**。子树在区间里还会留下更深的节点，
    // 但它们的父指针已经被更深的那层写好了 —— 按区间回填会把那些指针全部覆盖成
    // 当前节点，整棵树会退化成一个"所有节点都挂在同一个父上"的假结构。
    if (left_root_index == NaniteConstants::kInvalidClusterIndex &&
        right_root_index == NaniteConstants::kInvalidClusterIndex)
    {
        // 两个半区都为空（退化输入），这个节点不产生集群
        return NaniteConstants::kInvalidClusterIndex;
    }

    NaniteCluster node;
    node.is_leaf = false;
    node.lod_level = static_cast<uint8_t>(std::min<uint32_t>(depth, 255u));
    node.vertex_offset = vertex_begin;
    node.vertex_count = static_cast<uint32_t>(out_positions.size()) - vertex_begin;
    node.index_offset = index_begin;
    node.triangle_count = (static_cast<uint32_t>(out_indices.size()) - index_begin) / 3;

    AxisAlignedBox union_bounds;
    for (uint32_t child_index : {left_root_index, right_root_index})
    {
        if (child_index == NaniteConstants::kInvalidClusterIndex)
        {
            continue;
        }
        const NaniteCluster& child = out_clusters[child_index];
        if (child.bounds.IsValid())
        {
            union_bounds.Merge(child.bounds.getMinCorner());
            union_bounds.Merge(child.bounds.getMaxCorner());
        }
        node.child_cluster_indices.push_back(child_index);
    }
    // 子节点的边界可能因"按重心分派、顶点越界"而略微不匹配传入的 bounds，
    // 所以取子节点包围盒的并集；并集无效（所有子节点都退化）时才退回节点自身的盒。
    node.bounds = union_bounds.IsValid() ? union_bounds : bounds;
    node.center = node.bounds.getCenter();
    node.radius = (node.bounds.getMaxCorner() - node.bounds.getMinCorner()).length() * 0.5f;
    node.error_metric = CalculateClusterError(node);

    const uint32_t node_index = static_cast<uint32_t>(out_clusters.size());
    out_clusters.push_back(node);

    // 父指针要在节点入数组之后再写：push_back 可能重分配，之前拿的引用会失效。
    for (uint32_t child_index : node.child_cluster_indices)
    {
        out_clusters[child_index].parent_cluster_index = node_index;
    }

    return node_index;
}

void NaniteClusterBuilder::CalculateClusterBounds(const std::vector<Vector3>& positions,
                                                  const std::vector<uint32_t>& indices,
                                                  uint32_t start_index,
                                                  uint32_t count,
                                                  NaniteCluster& cluster)
{
    if (count == 0)
        return;

    Vector3 min_pos = positions[indices[start_index]];
    Vector3 max_pos = positions[indices[start_index]];

    uint32_t end_index = start_index + count;
    for (uint32_t i = start_index; i < end_index && i < indices.size(); ++i)
    {
        const Vector3& pos = positions[indices[i]];
        min_pos.makeFloor(pos);
        max_pos.makeCeil(pos);
    }

    cluster.bounds = AxisAlignedBox((min_pos + max_pos) * 0.5f, (max_pos - min_pos) * 0.5f);
    cluster.center = cluster.bounds.getCenter();
    cluster.radius = (max_pos - min_pos).length() * 0.5f;
}

bool NaniteClusterBuilder::BuildHierarchy(NaniteMeshResource& mesh_resource)
{
    if (mesh_resource.clusters.empty())
        return false;

    // 真实的父子关系已经在递归分割里建好了（内部节点后序创建并回填子节点的父指针），
    // 这里只做两件全局的事：算层次深度、把误差单调化。
    //
    // 以前这里是 O(n²) 的启发式：给每个集群找"第一个层级更低且中心距离 < 2*radius 的
    // 集群"当父亲，选中谁取决于数组顺序，而且 `break` 之后同一个父会重复收到子节点。
    // 更糟的是 root_cluster_index 被写死成 0 —— 而 0 号集群是深度优先的第一个**叶子**，
    // 从来不是根。
    if (mesh_resource.root_cluster_index >= mesh_resource.clusters.size())
    {
        return false;
    }

    uint8_t max_level = 0;
    for (const auto& cluster : mesh_resource.clusters)
    {
        max_level = std::max(max_level, cluster.lod_level);
    }
    mesh_resource.hierarchy_depth = max_level;

    // 误差单调化：父节点恒 >= 子节点。空间二分本来就该让半径单调，但三角形是
    // **按重心**分派的，一个三角形可以跨越分割面，子节点的包围盒因此可能略大于父盒。
    // LOD 选择依赖这个单调性（往父走 = 误差变大），所以显式兜一遍。
    // 从最深的一层往上扫，保证处理某个节点时它的子节点都已经处理过了。
    std::vector<uint32_t> order(mesh_resource.clusters.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        order[i] = static_cast<uint32_t>(i);
    }
    std::sort(order.begin(), order.end(), [&mesh_resource](uint32_t a, uint32_t b) {
        return mesh_resource.clusters[a].lod_level > mesh_resource.clusters[b].lod_level;
    });

    for (uint32_t index : order)
    {
        const NaniteCluster& cluster = mesh_resource.clusters[index];
        if (cluster.parent_cluster_index >= mesh_resource.clusters.size())
        {
            continue;
        }
        NaniteCluster& parent = mesh_resource.clusters[cluster.parent_cluster_index];
        parent.error_metric = std::max(parent.error_metric, cluster.error_metric);
    }

    return true;
}

float NaniteClusterBuilder::CalculateClusterError(const NaniteCluster& cluster) const
{
    // 错误度量 = 集群的空间尺度（包围球半径）。没有网格简化器，所以"这一档比原始网格
    // 差多少"无法真正计算；半径至少是一个随细分单调变小的量：丢掉一个半径为 r 的集群，
    // 洞的尺度就是 r。
    //
    // 以前这里乘了 (1 + lod_level * 0.1f)，方向与 LOD 约定相反 —— 层级越大（越细）
    // 反而误差越大，LOD 选择会因此往错误的方向走。
    return cluster.radius;
}

void NaniteClusterBuilder::OptimizeClusters(NaniteMeshResource& mesh_resource)
{
    // 合并小集群需要重排几何窗口并重写全部父子索引（还要重排 root），代价大而当前
    // 收益不明确，所以这一步只做**规范化**：保证每个集群满足下游的前提条件。
    //
    // 最关键的一条是半径下限。退化成一个点的集群半径为 0，凡是拿它当分母或基准的计算
    // （SelectLOD 里的 error / distance、视锥测试的包围球）都会直接失去意义 ——
    // 而 error_metric 以前根本没人赋值，读到的是未初始化内存。
    for (auto& cluster : mesh_resource.clusters)
    {
        if (!std::isfinite(cluster.radius) || cluster.radius <= 0.0f)
        {
            cluster.radius = NaniteConstants::kMinClusterSize;
        }
        if (!std::isfinite(cluster.error_metric) || cluster.error_metric < 0.0f)
        {
            cluster.error_metric = cluster.radius;
        }
        cluster.error_metric = std::max(cluster.error_metric, cluster.radius);
    }
}

bool NaniteClusterBuilder::ValidateCluster(const NaniteCluster& cluster)
{
    return cluster.vertex_count > 0 && cluster.triangle_count > 0 && cluster.radius > 0.0f;
}

// ==================== NaniteClusterManager ====================

NaniteClusterManager::NaniteClusterManager() = default;

NaniteClusterManager::~NaniteClusterManager()
{
    clear();
}

bool NaniteClusterManager::Initialize(RHI* rhi, const NaniteConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void NaniteClusterManager::clear()
{
    m_MeshResources.clear();
    m_StreamingQueue.clear();
}

bool NaniteClusterManager::RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource)
{
    MeshResourceEntry entry;
    entry.mesh_resource = mesh_resource;
    // 没有流式加载（UpdateStreaming 是 TODO），一个网格注册进来就是整个驻留在内存里。
    // 以前这里一律填 false ⇒ IsClusterLoaded 永远返回 false，好像什么都没加载过。
    entry.cluster_loaded_flags.resize(mesh_resource.clusters.size(), true);
    entry.visible_clusters.clear();

    m_MeshResources[mesh_id] = entry;
    return true;
}

void NaniteClusterManager::UnregisterMeshResource(uint64_t mesh_id)
{
    m_MeshResources.erase(mesh_id);
}

void NaniteClusterManager::UpdateStreaming(const Vector3& camera_position, float streaming_distance)
{
    // TODO: 实现流式加载逻辑
    m_StreamingQueue.clear();
}

const std::vector<uint32_t>& NaniteClusterManager::GetVisibleClusters(uint64_t mesh_id) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        return it->second.visible_clusters;
    }
    static std::vector<uint32_t> empty;
    return empty;
}

void NaniteClusterManager::SetVisibleClusters(uint64_t mesh_id, std::vector<uint32_t> visible_clusters)
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        it->second.visible_clusters = std::move(visible_clusters);
    }
}

const NaniteMeshResource* NaniteClusterManager::GetMeshResource(uint64_t mesh_id) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        return &it->second.mesh_resource;
    }
    return nullptr;
}

uint32_t NaniteClusterManager::GetClusterCount(uint64_t mesh_id) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        return static_cast<uint32_t>(it->second.mesh_resource.clusters.size());
    }
    return 0;
}

bool NaniteClusterManager::IsClusterLoaded(uint64_t mesh_id, uint32_t cluster_index) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        if (cluster_index < it->second.cluster_loaded_flags.size())
        {
            return it->second.cluster_loaded_flags[cluster_index];
        }
    }
    return false;
}

const NaniteCluster* NaniteClusterManager::GetCluster(uint64_t mesh_id, uint32_t cluster_index) const
{
    auto it = m_MeshResources.find(mesh_id);
    if (it != m_MeshResources.end())
    {
        if (cluster_index < it->second.mesh_resource.clusters.size())
        {
            return &it->second.mesh_resource.clusters[cluster_index];
        }
    }
    return nullptr;
}