#pragma once

#include "LumenTypes.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class RHI;
class RenderResourceBase;
class RenderScene;

// 表面缓存管理器
//
// 页面 = 一个场景实例（一个 RenderEntity），覆盖它的世界 AABB，携带它的常量材质。
// 查询按世界位置走一个均匀网格索引，再从候选页里挑真正包含该点的那个。
class LumenSurfaceCacheManager
{
public:
    // 无效的页面 ID
    static constexpr uint32_t kInvalidPage = UINT32_MAX;

    LumenSurfaceCacheManager();
    ~LumenSurfaceCacheManager();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 运行时改配置（CVar）。页面已经建好的话会由场景哈希决定要不要重建。
    void setConfig(const LumenConfig& config) { m_Config = config; }

    const LumenConfig& getConfig() const { return m_Config; }

    // 清理
    void clear();

    // 从场景构建表面缓存。render_resource 用来在实例包围盒无效时退回网格的局部包围盒。
    bool BuildFromScene(std::shared_ptr<RenderScene> render_scene,
                        RenderResourceBase* render_resource,
                        const Vector3& camera_position);

    // 更新表面缓存（增量更新）。每帧调用一次。
    void UpdateSurfaceCache(std::shared_ptr<RenderScene> render_scene,
                            RenderResourceBase* render_resource,
                            const Vector3& camera_position);

    // 获取表面信息。
    //
    // 故意**不是 const**：命中一次就要刷新页面的 LRU 时间戳，否则驱逐逻辑没有任何
    // 依据可依。
    bool GetSurfaceInfo(const Vector3& world_position, const Vector3& normal, LumenSurfaceCachePage& out_page);

    // 获取表面缓存数据
    const LumenSurfaceCache& getSurfaceCache() const { return m_SurfaceCache; }
    LumenSurfaceCache& getSurfaceCache() { return m_SurfaceCache; }

    // 检查是否需要更新
    bool needsUpdate() const { return m_SurfaceCache.needs_update; }

    // 标记为需要更新
    void markForUpdate() { m_SurfaceCache.needs_update = true; }

    // 驱逐空转超过 kMaxSurfaceCachePageIdleFrames 的页面
    void CleanupInvalidPages();

    // 当前帧号（LRU 的时基）
    uint64_t getFrameIndex() const { return m_FrameIndex; }

    // 当前索引里登记的页面数（测试用）
    uint32_t getActivePageCount() const { return m_SurfaceCache.active_page_count; }

private:
    // 均匀网格的格子坐标
    struct CellCoord
    {
        int32_t x {0};
        int32_t y {0};
        int32_t z {0};

        bool operator==(const CellCoord& rhs) const { return x == rhs.x && y == rhs.y && z == rhs.z; }
    };

    struct CellCoordHash
    {
        size_t operator()(const CellCoord& cell) const noexcept
        {
            // 三个 21 位段塞进 64 位：覆盖 ±1M 格，超出会回绕但不影响正确性
            // （查询和写入用的是同一个函数）
            const auto pack = [](int32_t value) {
                return static_cast<uint64_t>(static_cast<int64_t>(value)) & 0x1FFFFFull;
            };
            return static_cast<size_t>((pack(cell.x) << 42) | (pack(cell.y) << 21) | pack(cell.z));
        }
    };

    // 分配新的缓存页面
    uint32_t AllocatePage();

    // 释放缓存页面
    void FreePage(uint32_t page_id);

    // 把所有页面归还并清空索引（重建前调用）
    void ResetPages();

    // 把页面登记进它 AABB 覆盖到的每一个格子
    void RegisterPageInGrid(uint32_t page_id);

    // 把页面从格子里摘掉（驱逐时用）
    void UnregisterPageFromGrid(uint32_t page_id);

    // 驱逐一个页面：摘索引 + 归还槽位
    void EvictPage(uint32_t page_id);

    // 世界位置 -> 格子坐标
    CellCoord CellOf(const Vector3& world_position) const;

    // 从格子里取出候选页面
    const std::vector<uint32_t>* FindGridCell(const Vector3& world_position) const;

    // 找出覆盖该位置的页面；没有精确命中时退回"最近的页面"
    uint32_t FindPageAt(const Vector3& world_position) const;

    // 驱逐最久未访问的一个有效页面（AllocatePage 走投无路时才用）
    bool EvictOldestPage();

    // 实例的世界空间 AABB；实体包围盒无效时退回网格的局部包围盒
    static AxisAlignedBox ComputeInstanceWorldBounds(const RenderEntity& entity, RenderResourceBase* render_resource);

    // 场景指纹：实例 id + 网格资产 id + 材质资产 id + 模型矩阵 + 材质 factor
    static uint64_t ComputeSceneHash(const std::vector<RenderEntity>& entities);

    // 计算页面LOD级别
    uint8_t CalculatePageLOD(const Vector3& world_position, const Vector3& camera_position) const;

    // 更新GPU资源
    void UpdateGPUResources();

    // 表面缓存数据
    LumenSurfaceCache m_SurfaceCache;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 页面索引：格子 -> 页面 ID 列表
    //
    // 页面会登记到它 AABB 覆盖的**每一个**格子，所以落在页面里的点所在的格子一定能
    // 查到它。格子边长取所有页面里最大的那个轴长，因此任意页面最多跨 2×2×2 = 8 个
    // 格子，登记量是有界的（1024 页 × 8 = 8192 条）。
    std::unordered_map<CellCoord, std::vector<uint32_t>, CellCoordHash> m_PageGrid;

    // 格子边长（世界单位）
    float m_CellSize {1.0f};

    // 实例 ID -> 页面 ID
    std::unordered_map<uint64_t, uint32_t> m_InstanceToPage;

    // 空闲页面列表
    std::vector<uint32_t> m_FreePages;

    // 场景指纹（用于"场景没变就不重建"）
    uint64_t m_SceneHash {0};

    // 帧号，LRU 的时基
    uint64_t m_FrameIndex {0};

    // GPU资源
    RHIImage* m_SurfaceCacheTexture = nullptr;
    RHISampler* m_SurfaceCacheSampler = nullptr;
};
