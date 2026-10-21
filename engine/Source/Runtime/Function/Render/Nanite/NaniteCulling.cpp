#include "NaniteCulling.h"

#include "NaniteTypes.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <algorithm>
#include <cmath>

// ==================== NaniteCullingSystem ====================

NaniteCullingSystem::NaniteCullingSystem() = default;

NaniteCullingSystem::~NaniteCullingSystem()
{
    clear();
}

bool NaniteCullingSystem::Initialize(RHI* rhi, const NaniteConfig& config)
{
    m_Rhi = rhi;
    m_Config = config;
    return true;
}

void NaniteCullingSystem::clear()
{
    // TODO: 释放GPU资源
}

NaniteVisibilityResult NaniteCullingSystem::CullInstancesCPU(const std::vector<NaniteInstance>& instances,
                                                             const std::shared_ptr<RenderCamera>& camera,
                                                             const NaniteConfig& config)
{
    NaniteVisibilityResult result;
    result.visible_instance_count = 0;
    result.visible_cluster_count = 0;
    result.visible_instance_indices.clear();
    result.visible_cluster_indices.clear();

    if (camera == nullptr)
    {
        return result;
    }

    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto& instance = instances[i];

        // 视锥裁剪。边界框来自实例自己（NaniteRenderer 注册实例时从网格资源填上）——
        // 以前这里是个**默认构造的空盒**，而 FrustumCull 本身又恒返回 true，两层都没干活。
        if (FrustumCull(instance.local_bounds, instance.transform, camera))
        {
            continue;
        }

        // 距离裁剪
        Vector3 instance_position = instance.transform.transformCoord(Vector3::ZERO);
        float distance = instance_position.distance(camera->position());
        if (distance > config.max_streaming_distance)
        {
            continue;
        }

        // 遮挡剔除
        if (config.enable_culling && OcclusionCull(instance.local_bounds, instance.transform))
        {
            continue;
        }

        result.visible_instance_indices.push_back(static_cast<uint32_t>(i));
        result.visible_instance_count++;
    }

    return result;
}

NaniteVisibilityResult NaniteCullingSystem::CullInstancesGPU(const std::vector<NaniteInstance>& instances,
                                                             const std::shared_ptr<RenderCamera>& camera,
                                                             const NaniteConfig& config)
{
    // TODO: 实现GPU裁剪
    // 使用计算着色器进行并行裁剪
    return CullInstancesCPU(instances, camera, config);
}

bool NaniteCullingSystem::FrustumCull(const AxisAlignedBox& bounds,
                                      const Matrix4x4& transform,
                                      const std::shared_ptr<RenderCamera>& camera)
{
    if (camera == nullptr)
    {
        return false;
    }

    // 标准数学约定：clip = projection * view * v_world，所以 VP = proj * view。
    // （Matrix4x4::operator* 是 concatenate，就是通常的矩阵积。）
    return FrustumCull(bounds, transform, camera->GetProjectionMatrix() * camera->GetViewMatrix());
}

bool NaniteCullingSystem::FrustumCull(const AxisAlignedBox& bounds,
                                      const Matrix4x4& transform,
                                      const Matrix4x4& view_projection)
{
    // 包围盒无效（调用方没填）时**不做**剔除：不知道边界就把实例裁掉是比不裁更糟的
    // 错误。NaniteRenderer::RegisterInstance 会替你从网格资源填上，填不上说明注册
    // 路径有问题，那时应该看见"全都没被裁"，而不是"全都被裁没了"。
    if (!bounds.IsValid())
    {
        return false;
    }

    std::vector<Vector4> planes;
    ExtractFrustumPlanes(view_projection, planes);

    return AabbFrustumTest(bounds, transform, planes) < 0;
}

bool NaniteCullingSystem::OcclusionCull(const AxisAlignedBox& bounds, const Matrix4x4& transform)
{
    // TODO: 实现遮挡剔除
    return false;
}

bool NaniteCullingSystem::DistanceCull(const Vector3& position, float distance, float max_distance)
{
    return distance > max_distance;
}

uint8_t NaniteCullingSystem::SelectLOD(const NaniteCluster& cluster,
                                       const Vector3& camera_position,
                                       const Matrix4x4& instance_transform,
                                       const NaniteConfig& config)
{
    // 关掉 LOD 时保持集群自己的层级。以前这里返回 0，而 0 是**根（最粗）**，
    // 语义成了"尽量粗"而不是"别管我"。
    if (!config.enable_lod)
    {
        return cluster.lod_level;
    }

    // 计算距离
    Vector3 cluster_world_pos = instance_transform.transformCoord(cluster.center);
    float distance = cluster_world_pos.distance(camera_position);

    // 距离为 0（相机正好在集群中心）时下面的除法会得到 inf，判据彻底失效。
    // 这时集群显然近在眼前，直接用最细的一档。
    if (!(distance > 0.0f))
    {
        return cluster.lod_level;
    }

    // 基于距离和错误度量选择LOD。error_metric 是"丢掉这个集群会留下的洞"的世界尺度，
    // 除以距离就是它在屏幕上占多大 —— 越大越需要更细的一档。
    //
    // **以前这两个分支是反的**：误差小反而往更细（+1）、误差大反而往更粗（-1），
    // 于是越远的东西越要求细。另外它读的是成员 m_Config 而不是传进来的 config，
    // 调用方改了配置也不生效。
    const float screen_error = cluster.error_metric / distance;

    uint8_t lod_level = cluster.lod_level;
    if (screen_error < config.max_screen_error * 0.5f)
    {
        // 已经够细了，可以粗一档（往根走）
        lod_level = static_cast<uint8_t>(std::max(static_cast<int>(lod_level) - 1, 0));
    }
    else if (screen_error > config.max_screen_error * 2.0f)
    {
        // 太粗了，要更细一档（往叶子走）
        lod_level = std::min(static_cast<uint8_t>(lod_level + 1),
                             static_cast<uint8_t>(NaniteConstants::kMaxHierarchyDepth));
    }

    return lod_level;
}

bool NaniteCullingSystem::IsClusterVisible(const NaniteCluster& cluster,
                                           const Vector3& camera_position,
                                           const Matrix4x4& instance_transform,
                                           const NaniteConfig& config)
{
    // 只有叶子能提交渲染：内部节点的几何区间只是它子树的聚合，画它等于把整棵子树
    // 重画一遍（没有简化器，粗一档并不会少画三角形）。
    if (!cluster.is_leaf)
    {
        return false;
    }

    return SelectLOD(cluster, camera_position, instance_transform, config) >= cluster.lod_level;
}

void NaniteCullingSystem::ExtractFrustumPlanes(const Matrix4x4& view_projection, std::vector<Vector4>& out_planes)
{
    out_planes.resize(6);

    // Gribb-Hartmann 平面提取。
    //
    // **Matrix4x4 是行主序、`m_Mat[row][col]`、`clip = M * v`**（见 Matrix4.h 里
    // operator*(Vector4)：clip.x = 第 0 行点乘 v，clip.w = 第 3 行点乘 v）。所以
    // clip 的每个分量就是对应那一行与世界坐标的点积，六个平面直接由行组合出来：
    //
    //   -w <= x <=  w  ⇒  Left  = row3 + row0 , Right = row3 - row0
    //   -w <= y <=  w  ⇒  Bottom= row3 + row1 , Top   = row3 - row1
    //   -w <= z <=  w  ⇒  Near  = row3 + row2 , Far   = row3 - row2
    //
    // 以前这份代码用的是 Ogre / 列主序那套（把 m[i][j] 当成 m[col][row]、点号写在左边），
    // 对本仓库的矩阵会得到六个完全不相干的平面。它从没被调用过，所以没人发现。
    auto row_plus = [&view_projection](int row) {
        return Vector4(view_projection[3][0] + view_projection[row][0],
                       view_projection[3][1] + view_projection[row][1],
                       view_projection[3][2] + view_projection[row][2],
                       view_projection[3][3] + view_projection[row][3]);
    };
    auto row_minus = [&view_projection](int row) {
        return Vector4(view_projection[3][0] - view_projection[row][0],
                       view_projection[3][1] - view_projection[row][1],
                       view_projection[3][2] - view_projection[row][2],
                       view_projection[3][3] - view_projection[row][3]);
    };

    // Left plane
    out_planes[0] = row_plus(0);
    out_planes[0].normalise();

    // Right plane
    out_planes[1] = row_minus(0);
    out_planes[1].normalise();

    // Bottom plane
    out_planes[2] = row_plus(1);
    out_planes[2].normalise();

    // Top plane
    out_planes[3] = row_minus(1);
    out_planes[3].normalise();

    // Near plane
    out_planes[4] = row_plus(2);
    out_planes[4].normalise();

    // Far plane
    out_planes[5] = row_minus(2);
    out_planes[5].normalise();
}

int NaniteCullingSystem::PointFrustumTest(const Vector3& point, const std::vector<Vector4>& planes)
{
    for (const auto& plane : planes)
    {
        float distance = plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
        if (distance < 0.0f)
        {
            return -1;  // 在视锥外
        }
    }
    return 1;  // 在视锥内
}

int NaniteCullingSystem::AabbFrustumTest(const AxisAlignedBox& aabb,
                                         const Matrix4x4& transform,
                                         const std::vector<Vector4>& planes)
{
    // 变换AABB的8个角点到世界空间
    Vector3 center = aabb.getCenter();
    Vector3 half_extent = aabb.getHalfExtent();

    std::vector<Vector3> corners(8);
    corners[0] = transform.transformCoord(center + Vector3(-half_extent.x, -half_extent.y, -half_extent.z));
    corners[1] = transform.transformCoord(center + Vector3(half_extent.x, -half_extent.y, -half_extent.z));
    corners[2] = transform.transformCoord(center + Vector3(-half_extent.x, half_extent.y, -half_extent.z));
    corners[3] = transform.transformCoord(center + Vector3(half_extent.x, half_extent.y, -half_extent.z));
    corners[4] = transform.transformCoord(center + Vector3(-half_extent.x, -half_extent.y, half_extent.z));
    corners[5] = transform.transformCoord(center + Vector3(half_extent.x, -half_extent.y, half_extent.z));
    corners[6] = transform.transformCoord(center + Vector3(-half_extent.x, half_extent.y, half_extent.z));
    corners[7] = transform.transformCoord(center + Vector3(half_extent.x, half_extent.y, half_extent.z));

    // 测试所有角点
    int result = 1;  // 假设完全在视锥内
    for (const auto& plane : planes)
    {
        int inside_count = 0;
        for (const auto& corner : corners)
        {
            float distance = plane.x * corner.x + plane.y * corner.y + plane.z * corner.z + plane.w;
            if (distance >= 0.0f)
            {
                inside_count++;
            }
        }

        if (inside_count == 0)
        {
            return -1;  // 完全在视锥外
        }
        else if (inside_count < 8)
        {
            result = 0;  // 部分在视锥内
        }
    }

    return result;
}

// ==================== NaniteHierarchicalZBuffer ====================

NaniteHierarchicalZBuffer::NaniteHierarchicalZBuffer() = default;

NaniteHierarchicalZBuffer::~NaniteHierarchicalZBuffer()
{
    clear();
}

bool NaniteHierarchicalZBuffer::Initialize(RHI* rhi, uint32_t width, uint32_t height)
{
    m_Rhi = rhi;
    m_Width = width;
    m_Height = height;

    // 计算Mip级别数
    m_MipLevels = static_cast<uint32_t>(std::log2(std::max(width, height))) + 1;
    m_MipBuffers.resize(m_MipLevels, nullptr);

    return true;
}

void NaniteHierarchicalZBuffer::clear()
{
    m_MipBuffers.clear();
    m_Width = 0;
    m_Height = 0;
    m_MipLevels = 0;
}

void NaniteHierarchicalZBuffer::UpdateDepthBuffer(const void* depth_data, uint32_t width, uint32_t height)
{
    // TODO: 更新深度缓冲并构建层次结构
    BuildHierarchy();
}

void NaniteHierarchicalZBuffer::BuildHierarchy()
{
    // TODO: 构建层次Z缓冲
    for (uint32_t i = 1; i < m_MipLevels; ++i)
    {
        BuildMipLevel(i);
    }
}

void NaniteHierarchicalZBuffer::BuildMipLevel(uint32_t mip_level)
{
    // TODO: 构建单个Mip级别
}

bool NaniteHierarchicalZBuffer::IsOccluded(const AxisAlignedBox& bounds, const Matrix4x4& transform, float min_z)
{
    // TODO: 实现遮挡测试
    return false;
}