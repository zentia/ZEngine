#pragma once

#include "LumenRayTracing.h"
#include "LumenSurfaceCache.h"
#include "LumenTypes.h"
#include "Runtime/Core/Math/Vector3.h"

#include <memory>

class RHI;
class RenderResourceBase;
class RenderScene;
class RenderCamera;

// 全局光照计算系统
class LumenGlobalIllumination
{
public:
    LumenGlobalIllumination();
    ~LumenGlobalIllumination();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 计算全局光照
    bool ComputeGlobalIllumination(std::shared_ptr<RenderScene> render_scene,
                                   std::shared_ptr<RenderCamera> camera,
                                   LumenRayTracing* ray_tracing,
                                   LumenSurfaceCacheManager* surface_cache,
                                   float delta_time);

    // 获取间接光照。
    //
    // 故意**不是 const**：查不到 probe 时会直接追踪，并把算出来的结果按时间累积写回
    // probe 网格 —— 这个"边查边填"的行为就是缓存本身。
    Vector3 GetIndirectLighting(const Vector3& world_position, const Vector3& normal);

    // 设置光线追踪引用
    void setRayTracing(LumenRayTracing* ray_tracing) { m_RayTracing = ray_tracing; }

    // 设置表面缓存引用
    void setSurfaceCache(LumenSurfaceCacheManager* surface_cache) { m_SurfaceCache = surface_cache; }

    // 设置距离场引用。软件光追的必需输入，同时决定"离表面多远才算离开自己"的偏移量，
    // 也是 probe 网格的世界盒来源。
    void setDistanceField(LumenDistanceField* distance_field) { m_DistanceField = distance_field; }

    // GI 的产物：世界空间辐照度 probe 网格。渲染 Pass / 将来的 GPU 上传读这里。
    const LumenProbeGrid& getProbeGrid() const { return m_ProbeGrid; }
    LumenProbeGrid& getProbeGrid() { return m_ProbeGrid; }

    // GI 自己维护的帧号（probe 时间累积的时基）
    uint64_t getFrameIndex() const { return m_FrameIndex; }

private:
    // 从表面点出发时的自相交偏移。着色点本身就在面上，距离场在那个体素里的值约等于
    // 0（小于命中阈值），不偏移的话第一脚就"命中自己"，GI 会退化成全黑或全自照明。
    // 偏移量取一个体素对角线：距离场能分辨的最小尺度就是这个。
    float GetSurfaceOffset() const;

    // 把着色点沿法线抬到表面之外
    Vector3 OffsetOriginFromSurface(const Vector3& world_position, const Vector3& normal) const;

private:
    // 计算单次间接光照
    Vector3
    ComputeSingleBounceGI(const Vector3& world_position, const Vector3& normal, const Vector3& view_direction) const;

    // 计算多次反射间接光照
    Vector3 ComputeMultiBounceGI(const Vector3& world_position,
                                 const Vector3& normal,
                                 const Vector3& view_direction,
                                 uint32_t max_bounces) const;

    // 采样间接光照方向
    Vector3 SampleIndirectDirection(const Vector3& normal, float roughness) const;

    // 时间累积：把 lighting 混进它所在 probe 的历史值，并把混合结果写回网格。
    // lighting 本身也被改成混合后的值，调用方拿到的就是降噪结果。
    //
    // delta_time <= 0 时 alpha = 0：空 probe 照样被填上，已填过的 probe 保持不变 ——
    // 也就是"边查边填"绝不跟 UpdateProbes 的正规累积打架。
    void TemporalAccumulation(const Vector3& world_position, Vector3& lighting, float delta_time);

    // 空间复用：在 probe 网格上做三线性插值。以前是一个"线性扫稀疏缓存 + 半径 10"的
    // 实现，而那个缓存从来没被写入过，所以它恒返回 0。
    Vector3 SpatialReuse(const Vector3& world_position, const Vector3& normal) const;

    // 三线性插值的取数部分。返回 false 表示"这个位置附近还没有任何 probe 被更新过"，
    // 注意它跟"插出来的值是 0"是两回事 —— 被封闭几何体包住的 probe 本来就应该是 0。
    bool SampleProbeGrid(const Vector3& world_position, Vector3& out_irradiance) const;

    // probe 网格：让网格覆盖距离场的世界盒；分辨率按 gi_probe_spacing 算，再压到上限
    void ResizeProbeGrid();

    // 本帧更新一批 probe（轮转预算 + 离相机距离筛选）
    void UpdateProbes(const Vector3& camera_position, float delta_time);

    // 在一个 probe 上打 gi_sample_count 条球面光线，得到入射辐射与命中率
    void TraceProbeIrradiance(const Vector3& probe_position, Vector3& out_irradiance, float& out_visibility) const;

    // 时间累积的一步：previous 朝 sample 走 alpha（与帧率无关）
    Vector3 BlendTemporally(const Vector3& previous, const Vector3& sample, float delta_time) const;

    // 由 delta_time 和收敛速率算出本帧的混合系数
    float ComputeTemporalAlpha(float delta_time) const;

    // 均匀球面采样（probe 是无方向的，采整个球而不是半球）
    Vector3 SampleSphereDirection() const;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 子系统引用
    LumenRayTracing* m_RayTracing = nullptr;
    LumenSurfaceCacheManager* m_SurfaceCache = nullptr;
    LumenDistanceField* m_DistanceField = nullptr;

    // GI 的产物：辐照度 probe 网格
    LumenProbeGrid m_ProbeGrid;

    // 本帧更新 probe 的起点（轮转游标）
    uint32_t m_ProbeCursor {0};

    // GI 自己的帧号；probe 的 last_updated_frame 以它为准
    uint64_t m_FrameIndex {0};
};
