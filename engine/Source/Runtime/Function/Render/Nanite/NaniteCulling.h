#pragma once

#include "NaniteTypes.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <memory>
#include <vector>

class RHI;
class RenderScene;

// 裁剪系统 - 负责视锥裁剪、遮挡剔除等
class NaniteCullingSystem
{
public:
    NaniteCullingSystem();
    ~NaniteCullingSystem();

    // 初始化
    bool Initialize(RHI* rhi, const NaniteConfig& config);

    // 清理
    void clear();

    // 执行裁剪（CPU版本）
    NaniteVisibilityResult CullInstancesCPU(const std::vector<NaniteInstance>& instances,
                                            const std::shared_ptr<RenderCamera>& camera,
                                            const NaniteConfig& config);

    // 执行裁剪（GPU版本）
    NaniteVisibilityResult CullInstancesGPU(const std::vector<NaniteInstance>& instances,
                                            const std::shared_ptr<RenderCamera>& camera,
                                            const NaniteConfig& config);

    // 视锥裁剪
    bool
    FrustumCull(const AxisAlignedBox& bounds, const Matrix4x4& transform, const std::shared_ptr<RenderCamera>& camera);

    // 遮挡剔除（基于层次Z缓冲）
    bool OcclusionCull(const AxisAlignedBox& bounds, const Matrix4x4& transform);

    // 距离裁剪
    bool DistanceCull(const Vector3& position, float distance, float max_distance);

    // LOD选择
    uint8_t SelectLOD(const NaniteCluster& cluster,
                      const Vector3& camera_position,
                      const Matrix4x4& instance_transform,
                      const NaniteConfig& config);

private:
    RHI* m_Rhi;
    NaniteConfig m_Config;

    // GPU裁剪资源
    void* m_CullingBuffer = nullptr;
    void* m_VisibilityBuffer = nullptr;
    void* m_HierarchicalZBuffer = nullptr;

    // 计算视锥平面
    void ExtractFrustumPlanes(const Matrix4x4& view_projection, std::vector<Vector4>& out_planes);

    // 点与视锥的关系
    int PointFrustumTest(const Vector3& point, const std::vector<Vector4>& planes);

    // AABB与视锥的关系
    int AabbFrustumTest(const AxisAlignedBox& aabb, const Matrix4x4& transform, const std::vector<Vector4>& planes);
};

// 层次Z缓冲（用于遮挡剔除）
class NaniteHierarchicalZBuffer
{
public:
    NaniteHierarchicalZBuffer();
    ~NaniteHierarchicalZBuffer();

    // 初始化
    bool Initialize(RHI* rhi, uint32_t width, uint32_t height);

    // 清理
    void clear();

    // 更新深度缓冲
    void UpdateDepthBuffer(const void* depth_data, uint32_t width, uint32_t height);

    // 构建层次结构
    void BuildHierarchy();

    // 遮挡测试
    bool IsOccluded(const AxisAlignedBox& bounds, const Matrix4x4& transform, float min_z);

private:
    RHI* m_Rhi;

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_MipLevels = 0;

    // 层次深度缓冲
    std::vector<void*> m_MipBuffers;

    // 构建单个Mip级别
    void BuildMipLevel(uint32_t mip_level);
};