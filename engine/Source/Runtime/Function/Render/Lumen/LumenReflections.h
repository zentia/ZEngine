#pragma once

#include "LumenRayTracing.h"
#include "LumenTypes.h"
#include "Runtime/Core/Math/Vector3.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

class RHI;
class RenderResourceBase;
class RenderScene;
class RenderCamera;

// 反射系统
class LumenReflections
{
public:
    LumenReflections();
    ~LumenReflections();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 运行时改配置（CVar）。
    void setConfig(const LumenConfig& config) { m_Config = config; }

    const LumenConfig& getConfig() const { return m_Config; }

    // 清理
    void clear();

    // 计算反射
    LumenReflectionData ComputeReflection(const Vector3& world_position,
                                          const Vector3& normal,
                                          const Vector3& view_direction,
                                          float roughness,
                                          LumenRayTracing* ray_tracing) const;

    // 每帧更新：拿表面缓存的页面当采样点，预算轮转 + 时间累积。
    //
    // 采样点用页面而不是另建一套网格：页面本身就有世界位置和法线，天然是一个着色
    // 点，而且它已经按实例去重过了。
    void UpdateReflections(std::shared_ptr<RenderScene> render_scene,
                           std::shared_ptr<RenderCamera> camera,
                           LumenRayTracing* ray_tracing,
                           LumenSurfaceCacheManager* surface_cache,
                           float delta_time);

    // 把本帧的反射结果汇总出来，供渲染器写进 per-frame uniform。
    // 返回 false 表示还没有任何页面被更新过（关掉了 / 没有距离场 / 没有页面）。
    bool GetAverageReflection(Vector3& out_color, uint32_t& out_sample_count) const;

    // 设置光线追踪引用
    void setRayTracing(LumenRayTracing* ray_tracing) { m_RayTracing = ray_tracing; }

    // 设置距离场引用（软件光追的输入，同时决定自相交偏移）
    void setDistanceField(LumenDistanceField* distance_field) { m_DistanceField = distance_field; }

private:
    // 着色点本身在表面上，距离场在那里几乎为 0，不抬出去的话反射光线第一脚就命中自己
    Vector3 OffsetOriginFromSurface(const Vector3& world_position, const Vector3& normal) const;

private:
    // 屏幕空间反射
    LumenReflectionData ComputeScreenSpaceReflection(const Vector3& world_position,
                                                     const Vector3& reflection_direction,
                                                     float roughness) const;

    // 距离场反射
    LumenReflectionData ComputeDistanceFieldReflection(const Vector3& world_position,
                                                       const Vector3& normal,
                                                       const Vector3& reflection_direction,
                                                       float roughness,
                                                       LumenRayTracing* ray_tracing) const;

    // 硬件光线追踪反射
    LumenReflectionData ComputeHardwareRayTracingReflection(const Vector3& world_position,
                                                            const Vector3& reflection_direction,
                                                            float roughness,
                                                            LumenRayTracing* ray_tracing) const;

    // 混合反射结果
    //
    // 权重只看"哪一份结果真的命中了"，**不看**配置里的 max_trace_distance —— 那是个
    // 常量，用它来分支等于让配置决定"距离场算出来的东西要不要丢掉"。
    LumenReflectionData BlendReflections(const LumenReflectionData& ssr,
                                         const LumenReflectionData& dfr,
                                         const LumenReflectionData& hwrt,
                                         float roughness) const;

    // 在 direction 周围的一个圆锥里抽一个方向（粗糙度的模糊反射用）
    Vector3 SampleConeDirection(const Vector3& direction, float cos_max_angle) const;

    // 时间累积的混合系数（与帧率无关）。rate = 0 表示冻结。
    float ComputeTemporalAlpha(float delta_time) const;

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 子系统引用
    LumenRayTracing* m_RayTracing = nullptr;
    LumenDistanceField* m_DistanceField = nullptr;

    // 每个实例（= 每个表面缓存页面）累积的反射辐射。
    //
    // 用 instance_id 而不是页面下标做键：页面会因为重建 / 驱逐换槽位，而实例 id 是稳
    // 定的，换槽位不该丢掉已经累积下来的历史。
    std::unordered_map<uint64_t, Vector3> m_ReflectionByInstance;
    // 每个实例上次被更新的帧号；0 = 从没更新过
    std::unordered_map<uint64_t, uint64_t> m_ReflectionFrame;

    // 预算轮转的光标
    uint32_t m_Cursor {0};
    // 帧号（时间累积的时基）
    uint64_t m_FrameIndex {0};
};