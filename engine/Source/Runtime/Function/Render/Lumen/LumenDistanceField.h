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

    // 运行时改配置（CVar）。不释放任何资源；分辨率变了之后下一次
    // UpdateDistanceField 会因为几何哈希/分辨率不匹配而重建。
    void setConfig(const LumenConfig& config) { m_Config = config; }

    const LumenConfig& getConfig() const { return m_Config; }

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

    // 距离场是否已经有可用数据。软件光追必须先看这个：没生成过的话 QueryDistance
    // 一律返回 FLT_MAX，"没命中"和"没数据"会混成一回事。
    bool isLoaded() const { return m_DistanceField.is_loaded; }

    // 世界空间体素尺寸（三轴各自一份，通常不等）。距离场的精度上限就是它 —— 任何
    // 比体素还细的判定（命中阈值、法线梯度步长、表面自相交偏移）都要按它换算，
    // 写死常量的话换个分辨率就失效。
    Vector3 GetVoxelSize() const;

    // 体素盒的对角线长度，方便调用方直接当"一个体素的尺度"用
    float GetVoxelDiagonal() const { return GetVoxelSize().length(); }

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

    // 单个距离场体素：求它到所有实例的最近距离。只读 cache / world_to_local（体素化
    // 期间都不改动），返回该体素的距离值 —— 由调用方写进互不相交的下标，因此多个体素
    // 可以并行算而不需要任何锁。抽出来既给串行路径用，也给并行 worker 复用。
    static float ComputeVoxelDistance(const LumenDistanceField& df,
                                      const SceneGeometryCache& cache,
                                      const std::vector<Matrix4x4>& world_to_local,
                                      const AxisAlignedBox& bounds,
                                      const Vector3& voxel_size,
                                      uint32_t resolution_x,
                                      uint32_t resolution_y,
                                      uint32_t z,
                                      uint32_t y,
                                      uint32_t x);

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