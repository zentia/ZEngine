#pragma once

#include "LumenTypes.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <memory>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderScene;

// 距离场生成器和管理器
class LumenDistanceField
{
public:
    LumenDistanceField();
    ~LumenDistanceField();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 从场景生成距离场
    bool GenerateFromScene(std::shared_ptr<RenderScene> render_scene, const AxisAlignedBox& world_bounds);

    // 更新距离场（增量更新）
    void UpdateDistanceField(std::shared_ptr<RenderScene> render_scene, const Vector3& camera_position);

    // 查询距离场（获取指定位置的距离值）
    float QueryDistance(const Vector3& world_position) const;

    // 查询距离场（获取最近表面位置和法线）
    bool QueryNearestSurface(const Vector3& world_position,
                             Vector3& out_surface_position,
                             Vector3& out_surface_normal) const;

    // 构建层次距离场
    bool BuildHierarchicalDistanceField();

    // 获取距离场数据
    const LumenDistanceFieldData& getDistanceField() const { return m_DistanceField; }
    LumenDistanceFieldData& getDistanceField() { return m_DistanceField; }

    // 检查是否需要更新
    bool needsUpdate() const { return m_DistanceField.needs_update; }

    // 标记为需要更新
    void markForUpdate() { m_DistanceField.needs_update = true; }

    // 获取世界空间边界
    const AxisAlignedBox& getWorldBounds() const { return m_DistanceField.world_bounds; }

private:
    // 从网格数据生成距离场
    bool generateFromMesh(const std::vector<Vector3>& positions,
                          const std::vector<uint32_t>& indices,
                          const Matrix4x4& transform,
                          const AxisAlignedBox& mesh_bounds);

    // 计算点到三角形的距离
    float DistanceToTriangle(const Vector3& point, const Vector3& v0, const Vector3& v1, const Vector3& v2) const;

    // 计算点到网格的距离
    float DistanceToMesh(const Vector3& point,
                         const std::vector<Vector3>& positions,
                         const std::vector<uint32_t>& indices,
                         const Matrix4x4& transform) const;

    // 生成距离场体素
    void GenerateDistanceFieldVoxels(const AxisAlignedBox& bounds,
                                     uint32_t resolution_x,
                                     uint32_t resolution_y,
                                     uint32_t resolution_z,
                                     std::vector<float>& out_distance_data);

    // 构建层次级别
    void BuildHierarchyLevel(const LumenDistanceFieldData& parent_level,
                             uint32_t level_index,
                             LumenDistanceFieldData& out_level);

    // 更新GPU资源
    void UpdateGPUResources();

    // 距离场数据
    LumenDistanceFieldData m_DistanceField;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // GPU资源（纹理或缓冲区）
    RHIImage* m_DistanceFieldTexture = nullptr;
    RHIBuffer* m_DistanceFieldBuffer = nullptr;
    RHISampler* m_DistanceFieldSampler = nullptr;

    // 场景几何缓存（用于增量更新）
    struct SceneGeometryCache
    {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
        std::vector<Matrix4x4> transforms;
        std::vector<AxisAlignedBox> bounds;
        uint64_t geometry_hash;
    };
    SceneGeometryCache m_SceneGeometryCache;
};