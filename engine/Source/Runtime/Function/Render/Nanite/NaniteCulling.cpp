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

    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto& instance = instances[i];

        // 视锥裁剪
        // TODO: 获取实例的边界框
        AxisAlignedBox instance_bounds;  // 需要从mesh资源获取
        if (!FrustumCull(instance_bounds, instance.transform, camera))
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
        if (config.enable_culling && OcclusionCull(instance_bounds, instance.transform))
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
    // 计算视锥平面
    std::vector<Vector4> planes;
    // TODO: 从camera获取view_projection矩阵
    // ExtractFrustumPlanes(camera->getViewProjectionMatrix(), planes);

    // 变换边界框到世界空间
    Vector3 center = bounds.getCenter();
    Vector3 half_extent = bounds.getHalfExtent();

    // 简单的AABB测试
    // TODO: 实现完整的视锥裁剪
    return true;
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
    if (!config.enable_lod)
    {
        return 0;
    }

    // 计算距离
    Vector3 cluster_world_pos = instance_transform.transformCoord(cluster.center);
    float distance = cluster_world_pos.distance(camera_position);

    // 基于距离和错误度量选择LOD
    float screen_error = cluster.error_metric / distance;

    uint8_t lod_level = cluster.lod_level;
    if (screen_error < m_Config.max_screen_error * 0.5f)
    {
        lod_level =
            std::min(static_cast<uint8_t>(lod_level + 1), static_cast<uint8_t>(NaniteConstants::kMaxHierarchyDepth));
    }
    else if (screen_error > m_Config.max_screen_error * 2.0f)
    {
        lod_level = std::max(static_cast<uint8_t>(lod_level - 1), static_cast<uint8_t>(0));
    }

    return lod_level;
}

void NaniteCullingSystem::ExtractFrustumPlanes(const Matrix4x4& view_projection, std::vector<Vector4>& out_planes)
{
    out_planes.resize(6);

    // 提取视锥平面
    // Left plane
    out_planes[0] = Vector4(view_projection[0][3] + view_projection[0][0],
                            view_projection[1][3] + view_projection[1][0],
                            view_projection[2][3] + view_projection[2][0],
                            view_projection[3][3] + view_projection[3][0]);
    out_planes[0].normalise();

    // Right plane
    out_planes[1] = Vector4(view_projection[0][3] - view_projection[0][0],
                            view_projection[1][3] - view_projection[1][0],
                            view_projection[2][3] - view_projection[2][0],
                            view_projection[3][3] - view_projection[3][0]);
    out_planes[1].normalise();

    // Bottom plane
    out_planes[2] = Vector4(view_projection[0][3] + view_projection[0][1],
                            view_projection[1][3] + view_projection[1][1],
                            view_projection[2][3] + view_projection[2][1],
                            view_projection[3][3] + view_projection[3][1]);
    out_planes[2].normalise();

    // Top plane
    out_planes[3] = Vector4(view_projection[0][3] - view_projection[0][1],
                            view_projection[1][3] - view_projection[1][1],
                            view_projection[2][3] - view_projection[2][1],
                            view_projection[3][3] - view_projection[3][1]);
    out_planes[3].normalise();

    // Near plane
    out_planes[4] = Vector4(view_projection[0][3] + view_projection[0][2],
                            view_projection[1][3] + view_projection[1][2],
                            view_projection[2][3] + view_projection[2][2],
                            view_projection[3][3] + view_projection[3][2]);
    out_planes[4].normalise();

    // Far plane
    out_planes[5] = Vector4(view_projection[0][3] - view_projection[0][2],
                            view_projection[1][3] - view_projection[1][2],
                            view_projection[2][3] - view_projection[2][2],
                            view_projection[3][3] - view_projection[3][2]);
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