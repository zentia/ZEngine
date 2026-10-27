#include "NaniteCluster.h"

#include "NaniteTypes.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// 一个 cluster 的几何字节（vertex + index 段里属于该 cluster 窗口的部分）。页池预算按此计。
// Raw: 12B/顶点 + 4B/索引；Quantized: 6B/顶点 + 2B/索引（见 GeometryCodec）。
uint32_t ClusterGeometryBytes(const NaniteMeshResource& mesh, const NaniteCluster& c)
{
    const uint32_t vertex_bytes = (mesh.geometry_codec == GeometryCodec::Quantized) ? 6u : 12u;
    const uint32_t index_bytes = (mesh.geometry_codec == GeometryCodec::Quantized) ? 2u : 4u;
    return c.vertex_count * vertex_bytes + c.triangle_count * 3u * index_bytes;
}

// 把几何写進资源的字节段，按 config.geometry_codec 选择编码。
//
// 历史：这两个字节段曾在函数开头被 clear 之后再也不写 —— NaniteMeshResource 带着一堆
// 集群元数据（offset/count 都算得挺对）却**零几何字节**。现已修好。
//
// 编码格式见 NaniteTypes.h 的 GeometryCodec（version: zengine-nanite-geom/1）：
//   Raw       —— 未压缩的 Vector3(12B) + uint32(4B)，保持既有字节契约。
//   Quantized —— 有界有损量化：AABB 线性映射到 int16 位置(6B) + uint16 索引(2B)。
//
// 注意：量化是**有损**的（任意 32-bit float 无法无损压进更少比特）。解码位置 ≈
// quant_min + q * quant_scale，每分量误差 ≤ quant_scale/2。层次/边界/误差都在量化
// 之前用全精度算好，不受影响。
void AppendGeometryBytes(const std::vector<Vector3>& positions,
                         const std::vector<uint32_t>& indices,
                         NaniteMeshResource& out_mesh_resource,
                         const NaniteConfig& config)
{
    out_mesh_resource.geometry_codec = config.geometry_codec;

    if (config.geometry_codec == GeometryCodec::Quantized)
    {
        // 量化范围用全精度全局 AABB。留一点余量避免浮点舍入把端点顶出 int16 范围。
        const Vector3& bb_min = out_mesh_resource.global_bounds.getMinCorner();
        const Vector3& bb_max = out_mesh_resource.global_bounds.getMaxCorner();
        const Vector3 range = bb_max - bb_min;
        // 6 字节/顶点（3×int16）能表示 65536 级；用 32767 作分母给符号位留对称空间。
        const float kQuantScaleDenom = 32767.0f;
        out_mesh_resource.quant_min = bb_min;
        out_mesh_resource.quant_scale = Vector3(
            range.x / kQuantScaleDenom,
            range.y / kQuantScaleDenom,
            range.z / kQuantScaleDenom);

        // 头 24 字节：quant_min(12B) + quant_scale(12B)，供解码方读取。
        const size_t header_bytes = 2 * sizeof(Vector3);
        out_mesh_resource.vertex_data.resize(header_bytes + positions.size() * 3 * sizeof(int16_t));
        std::memcpy(out_mesh_resource.vertex_data.data(), &out_mesh_resource.quant_min, sizeof(Vector3));
        std::memcpy(out_mesh_resource.vertex_data.data() + sizeof(Vector3),
                    &out_mesh_resource.quant_scale, sizeof(Vector3));

        int16_t* quant = reinterpret_cast<int16_t*>(out_mesh_resource.vertex_data.data() + header_bytes);
        for (size_t i = 0; i < positions.size(); ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                // 退化轴（range==0，例如平铺在 XZ 平面上的网格 Y 分量恒为 0）：
                // 不能除以 0。这类轴上所有顶点坐标都等于 bb_min，量化成 0，解码时
                // 直接还原为 bb_min —— 精确、无误差。
                if (out_mesh_resource.quant_scale[c] == 0.0f)
                {
                    quant[i * 3 + c] = 0;
                    continue;
                }
                const float f = (&positions[i].x)[c];
                // 四舍五入到最近的量化级（不是截断）—— 截断的误差可达 scale，四舍五入
                // 把误差压到 scale/2，即下面的契约上界。
                const float q = std::round((f - bb_min[c]) / out_mesh_resource.quant_scale[c]);
                // clamp 到 int16 对称范围，防止 AABB 端点因舍入越界。
                quant[i * 3 + c] = static_cast<int16_t>(std::clamp(q, -32768.0f, 32767.0f));
            }
        }

        // 索引：集群顶点数远小于 65536，16-bit 足够（越界会在下方校验被拒）。
        out_mesh_resource.index_data.resize(indices.size() * sizeof(uint16_t));
        uint16_t* idx16 = reinterpret_cast<uint16_t*>(out_mesh_resource.index_data.data());
        for (size_t i = 0; i < indices.size(); ++i)
        {
            idx16[i] = static_cast<uint16_t>(indices[i]);
        }
        return;
    }

    // Raw：未压缩的原始布局。
    out_mesh_resource.quant_min = Vector3(0.0f, 0.0f, 0.0f);
    out_mesh_resource.quant_scale = Vector3(0.0f, 0.0f, 0.0f);
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

// ==================== 几何解码（屏蔽 codec 细节） ====================

Vector3 NaniteDecodeVertex(const NaniteMeshResource& mesh, uint32_t index)
{
    if (mesh.geometry_codec == GeometryCodec::Quantized)
    {
        const size_t header_bytes = 2 * sizeof(Vector3);
        const int16_t* quant =
            reinterpret_cast<const int16_t*>(mesh.vertex_data.data() + header_bytes);
        Vector3 v;
        for (int c = 0; c < 3; ++c)
        {
            v[c] = mesh.quant_min[c] + static_cast<float>(quant[index * 3 + c]) * mesh.quant_scale[c];
        }
        return v;
    }

    Vector3 value(0.0f, 0.0f, 0.0f);
    const size_t offset = static_cast<size_t>(index) * sizeof(Vector3);
    if (offset + sizeof(Vector3) <= mesh.vertex_data.size())
    {
        std::memcpy(&value, mesh.vertex_data.data() + offset, sizeof(Vector3));
    }
    return value;
}

uint32_t NaniteDecodeIndex(const NaniteMeshResource& mesh, uint32_t index)
{
    if (mesh.geometry_codec == GeometryCodec::Quantized)
    {
        const uint16_t* idx16 = reinterpret_cast<const uint16_t*>(mesh.index_data.data());
        return static_cast<uint32_t>(idx16[index]);
    }

    uint32_t value = 0;
    const size_t offset = static_cast<size_t>(index) * sizeof(uint32_t);
    if (offset + sizeof(uint32_t) <= mesh.index_data.size())
    {
        std::memcpy(&value, mesh.index_data.data() + offset, sizeof(uint32_t));
    }
    return value;
}

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

    // 压缩并存储几何数据（编码格式由 config.geometry_codec 决定，见 GeometryCodec）。
    AppendGeometryBytes(cluster_positions, cluster_indices, out_mesh_resource, config);

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
    // 页池簿记状态一并清空，避免残留登记在下次 Initialize 后仍占预算。
    m_PagePool = nanite_pages::PagePool{m_Config.streaming_pool_budget_bytes > 0
                                            ? m_Config.streaming_pool_budget_bytes
                                            : 1};
    m_ClusterPageIds.clear();
    m_NextPageId = 1;
    m_StreamingFrame = 0;
}

bool NaniteClusterManager::RegisterMeshResource(uint64_t mesh_id, const NaniteMeshResource& mesh_resource)
{
    MeshResourceEntry entry;
    entry.mesh_resource = mesh_resource;
    // 没有流式加载（UpdateStreaming 是 TODO），一个网格注册进来就是整个驻留在内存里。
    // 以前这里一律填 false ⇒ IsClusterLoaded 永远返回 false，好像什么都没加载过。
    entry.cluster_loaded_flags.resize(mesh_resource.clusters.size(), true);
    entry.visible_clusters.clear();

    // 页池：给每个叶子 cluster 分配一个 page_id，登记进池子（byte_size = 该 cluster 的
    // 几何字节）。结构页(internal)也登记但标 is_leaf=false（永不驱逐）。未启用页池时
    // 这些登记无害——UpdateStreaming 不会查池子。
    // 重复注册同一 mesh_id：先撤掉旧页，避免旧 cluster 数 > 新 cluster 数时残留悬空登记。
    auto stale = m_ClusterPageIds.find(mesh_id);
    if (stale != m_ClusterPageIds.end())
    {
        for (uint32_t page_id : stale->second)
            m_PagePool.RemovePage(page_id);
    }
    std::vector<uint32_t>& page_ids = m_ClusterPageIds[mesh_id];
    page_ids.clear();
    page_ids.resize(mesh_resource.clusters.size(), 0);
    for (uint32_t ci = 0; ci < mesh_resource.clusters.size(); ++ci)
    {
        const NaniteCluster& c = mesh_resource.clusters[ci];
        const uint32_t page_id = m_NextPageId++;
        page_ids[ci] = page_id;
        nanite_pages::PageRecord rec;
        rec.page_id = page_id;
        rec.mesh_id = mesh_id;
        rec.byte_size = ClusterGeometryBytes(mesh_resource, c);
        rec.is_leaf = c.is_leaf;
        m_PagePool.UpsertPage(rec);
    }

    m_MeshResources[mesh_id] = entry;
    return true;
}

void NaniteClusterManager::UnregisterMeshResource(uint64_t mesh_id)
{
    // 从页池撤掉该 mesh 的所有页，避免悬空登记累积。
    auto it = m_ClusterPageIds.find(mesh_id);
    if (it != m_ClusterPageIds.end())
    {
        for (uint32_t page_id : it->second)
            m_PagePool.RemovePage(page_id);
        m_ClusterPageIds.erase(it);
    }
    m_MeshResources.erase(mesh_id);
}

void NaniteClusterManager::UpdateStreaming(const Vector3& camera_position, float streaming_distance)
{
    // 流送开关关掉时：一切驻留 —— 等价于"没有流式加载"，注册即整个驻留内存。
    if (!m_Config.enable_streaming)
    {
        for (auto& entry : m_MeshResources)
        {
            std::fill(entry.second.cluster_loaded_flags.begin(),
                      entry.second.cluster_loaded_flags.end(), true);
        }
        m_StreamingQueue.clear();
        return;
    }

    m_StreamingQueue.clear();

    const bool use_pool = m_Config.streaming_pool_budget_bytes > 0;
    if (use_pool)
    {
        m_PagePool.SetBudget(m_Config.streaming_pool_budget_bytes);
    }

    // 第一遍：算每个叶子 cluster 的距离，喂进页池（距离 + 字节），并收集"距离内"的叶子页
    // id 作为本帧 want-resident 集合。结构页(internal)在 Update 内部被强制驻留。
    std::vector<uint32_t> want_resident;
    for (auto& entry : m_MeshResources)
    {
        const NaniteMeshResource& mesh = entry.second.mesh_resource;
        const std::vector<uint32_t>& page_ids = m_ClusterPageIds[entry.first];

        for (uint32_t cluster_index = 0; cluster_index < mesh.clusters.size(); ++cluster_index)
        {
            const NaniteCluster& cluster = mesh.clusters[cluster_index];
            if (!cluster.is_leaf)
                continue;  // 结构页：距离不参与 want，Update 会强制驻留

            const float dist = camera_position.distance(cluster.bounds.getCenter());
            if (use_pool)
            {
                nanite_pages::PageRecord rec;
                rec.page_id = page_ids[cluster_index];
                rec.mesh_id = entry.first;
                rec.byte_size = ClusterGeometryBytes(mesh, cluster);
                rec.is_leaf = true;
                rec.distance = dist;
                m_PagePool.UpsertPage(rec);  // 刷新距离/字节（保留 LRU 时间戳）
            }
            if (dist <= streaming_distance)
            {
                want_resident.push_back(page_ids[cluster_index]);
            }
        }
    }

    if (use_pool)
    {
        // 页池按"距离优先 + LRU"在字节预算内决定最终驻留位；换入/换出队列交给渲染器
        // 驱动 NanitePageUploadPlanner 产出拷贝/释放计划（真正 UploadBytes 是渲染绑定）。
        // 存下增量队列供渲染器取走，同时同步 cluster_loaded_flags。
        m_LastPoolUpdate = m_PagePool.Update(want_resident, /*now=*/static_cast<double>(m_StreamingFrame++));

        for (auto& entry : m_MeshResources)
        {
            const std::vector<uint32_t>& page_ids = m_ClusterPageIds[entry.first];
            std::vector<bool>& loaded = entry.second.cluster_loaded_flags;
            for (uint32_t cluster_index = 0; cluster_index < loaded.size(); ++cluster_index)
            {
                // 结构页永远驻留；叶子页看池子的驻留位。
                const bool resident = !entry.second.mesh_resource.clusters[cluster_index].is_leaf
                                          ? true
                                          : m_PagePool.IsResident(page_ids[cluster_index]);
                if (loaded[cluster_index] != resident)
                {
                    loaded[cluster_index] = resident;
                    m_StreamingQueue.emplace_back(entry.first, cluster_index);
                }
            }
        }
        return;
    }

    // 未启用页池：退回纯距离裁剪（原语义，逐 cluster 标 flag）。
    for (auto& entry : m_MeshResources)
    {
        const NaniteMeshResource& mesh = entry.second.mesh_resource;
        std::vector<bool>& loaded = entry.second.cluster_loaded_flags;

        for (uint32_t cluster_index = 0; cluster_index < mesh.clusters.size(); ++cluster_index)
        {
            const NaniteCluster& cluster = mesh.clusters[cluster_index];

            // 内部节点不提交渲染（IsClusterVisible 对非叶子恒返回 false），它的几何
            // 窗口只是整棵子树的聚合。流送只决定**叶子**的驻留；内部节点永远保持
            // 驻留，维持"结构节点随时可读"的不变量 —— 卸载它们没有任何渲染收益，
            // 只会让层次查询拿到空缺。
            if (!cluster.is_leaf)
            {
                loaded[cluster_index] = true;
                continue;
            }

            // 驻留判据 = 集群包围盒中心到相机的距离。这里**没有**反馈式码率预算
            // （那需要屏幕误差积分 + 一个真实的页池），所以这是**距离裁剪**，不是
            // UE 那套 streaming pool。叶子中心在 streaming_distance 之内 ⇒ 驻留；
            // 之外 ⇒ 未驻留（本帧不提交渲染）。
            const float dist = camera_position.distance(cluster.bounds.getCenter());
            const bool resident = dist <= streaming_distance;

            if (loaded[cluster_index] != resident)
            {
                loaded[cluster_index] = resident;
                m_StreamingQueue.emplace_back(entry.first, cluster_index);
            }
        }
    }
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

std::unordered_map<uint32_t, nanite_pages::PageGeometryDesc>
NaniteClusterManager::CollectResidentPageGeometry() const
{
    // 只收集**当前驻留**的叶子页的几何描述符（offset/count 都在 mesh 的 vertex/index 流里）。
    // 渲染器用它做 PlanStreamingUploads 的拷贝源。未驻留/结构页不收（结构页不进页池上传路径）。
    std::unordered_map<uint32_t, nanite_pages::PageGeometryDesc> result;
    for (const auto& entry : m_MeshResources)
    {
        const NaniteMeshResource& mesh = entry.second.mesh_resource;
        const std::vector<uint32_t>& page_ids = m_ClusterPageIds.at(entry.first);
        for (uint32_t ci = 0; ci < mesh.clusters.size(); ++ci)
        {
            const NaniteCluster& c = mesh.clusters[ci];
            if (!c.is_leaf)
                continue;
            if (!m_PagePool.IsResident(page_ids[ci]))
                continue;
            nanite_pages::PageGeometryDesc desc;
            desc.mesh_id = entry.first;
            desc.vertex_offset = c.vertex_offset;
            desc.vertex_count = c.vertex_count;
            desc.index_offset = c.index_offset;
            desc.index_count = c.triangle_count * 3u;
            result[page_ids[ci]] = desc;
        }
    }
    return result;
}

uint32_t NaniteClusterManager::GetClusterPageId(uint64_t mesh_id, uint32_t cluster_index) const
{
    auto it = m_ClusterPageIds.find(mesh_id);
    if (it == m_ClusterPageIds.end() || cluster_index >= it->second.size())
        return 0;
    return it->second[cluster_index];
}