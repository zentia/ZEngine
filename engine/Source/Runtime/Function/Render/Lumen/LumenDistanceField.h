#pragma once

#include "LumenTypes.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <cstdint>
#include <memory>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderScene;

// 距离场生成器和管理器
class LumenDistanceField
{
public:
    // 场景几何缓存。距离场体素化（以及之后的增量更新）唯一的输入。
    //
    // positions/indices 存的是**局部空间**几何，并且按网格资产去重 —— 同一个资产被
    // 100 个实例引用也只存一份。因此"哪个实例用哪段几何"不能靠下标推出来，必须由
    // ranges 显式记录：ranges / transforms / bounds 三个数组一一对应，长度就是实例数。
    struct SceneGeometryCache
    {
        // 一个实例引用的几何在 positions / indices 里的区间
        struct MeshRange
        {
            uint32_t vertex_offset {0};
            uint32_t vertex_count {0};
            uint32_t index_offset {0};
            uint32_t index_count {0};
        };

        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;

        std::vector<MeshRange> ranges;         // 每个实例一段（多个实例可指向同一段）
        std::vector<Matrix4x4> transforms;     // 每个实例：局部 -> 世界
        std::vector<AxisAlignedBox> bounds;    // 每个实例：世界空间 AABB

        // 场景"长什么样"的指纹（实例 id + 网格资产 id + 模型矩阵），用于在场景没变时
        // 跳过整次体素化
        uint64_t geometry_hash {0};

        size_t GetInstanceCount() const { return ranges.size(); }
        uint32_t GetTriangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }
        bool IsEmpty() const { return ranges.empty(); }
    };

    LumenDistanceField();
    ~LumenDistanceField();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 从场景生成距离场。render_resource 提供 CPU 侧网格几何（CpuMeshGeometry 缓存）。
    bool GenerateFromScene(std::shared_ptr<RenderScene> render_scene,
                           RenderResourceBase* render_resource,
                           const AxisAlignedBox& world_bounds);

    // 更新距离场（增量更新）
    void UpdateDistanceField(std::shared_ptr<RenderScene> render_scene,
                             RenderResourceBase* render_resource,
                             const Vector3& camera_position);

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

    // 获取场景几何缓存（调试与测试用）
    const SceneGeometryCache& getSceneGeometryCache() const { return m_SceneGeometryCache; }

private:
    // 场景"长什么样"的指纹：实例 id + 网格资产 id + 模型矩阵
    static uint64_t ComputeSceneGeometryHash(const std::vector<RenderEntity>& entities);

    // 把场景里的实例收集进 m_SceneGeometryCache（几何按网格资产去重）
    bool CollectSceneGeometry(const std::vector<RenderEntity>& entities, RenderResourceBase* render_resource);

    // 点到 AABB 的距离（点在盒内为 0），用于体素化时快速剔除整个实例
    static float DistancePointToAxisAlignedBox(const Vector3& point, const AxisAlignedBox& box);

    // 计算点到三角形的距离
    float DistanceToTriangle(const Vector3& point, const Vector3& v0, const Vector3& v1, const Vector3& v2) const;

    // 计算世界空间点到某个实例的距离。world_to_local 由调用方预先算好 —— 体素化是
    // O(体素数 × 实例数)，不可能每个体素再求一次逆矩阵。
    float DistanceToMesh(const Vector3& world_point,
                         const SceneGeometryCache::MeshRange& range,
                         const Matrix4x4& world_to_local) const;

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
    SceneGeometryCache m_SceneGeometryCache;
};