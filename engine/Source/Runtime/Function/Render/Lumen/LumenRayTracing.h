#pragma once

#include "LumenDistanceField.h"
#include "LumenSurfaceCache.h"
#include "LumenTypes.h"
#include "Runtime/Core/Math/Vector3.h"

#include <memory>

class RHI;
class RenderResourceBase;
class RenderScene;

// 光线追踪系统
class LumenRayTracing
{
public:
    LumenRayTracing();
    ~LumenRayTracing();

    // 初始化
    bool Initialize(RHI* rhi, const LumenConfig& config);

    // 清理
    void clear();

    // 追踪光线（软件追踪，基于距离场）
    //
    // distance_field 允许为 nullptr，此时退化为 setDistanceField() 设进来的那个。
    // 但调用方最好显式传：nullptr 兜底只是为了让老调用点不至于静默变成"永不命中"。
    LumenRayTracingResult TraceRaySoftware(const Vector3& ray_origin,
                                           const Vector3& ray_direction,
                                           float max_distance,
                                           LumenDistanceField* distance_field) const;

    // 追踪光线（硬件追踪）
    LumenRayTracingResult
    TraceRayHardware(const Vector3& ray_origin, const Vector3& ray_direction, float max_distance) const;

    // 追踪光线（自动选择硬件或软件）
    LumenRayTracingResult TraceRay(const Vector3& ray_origin,
                                   const Vector3& ray_direction,
                                   float max_distance,
                                   LumenDistanceField* distance_field) const;

    // 检查硬件光线追踪支持
    bool isHardwareRayTracingSupported() const { return m_HardwareRayTracingSupported; }

    // 设置距离场引用
    void setDistanceField(LumenDistanceField* distance_field) { m_DistanceField = distance_field; }

    LumenDistanceField* getDistanceField() const { return m_DistanceField; }

    // 设置表面缓存引用。命中点要在这里查材质 —— 没有它的话命中结果只能带一组硬编码
    // 的常量材质，GI 就退化成"任何表面都弹同样的光"。
    void setSurfaceCache(LumenSurfaceCacheManager* surface_cache) { m_SurfaceCache = surface_cache; }

private:
    // 用表面缓存填命中点的材质；查不到时填一组中性的缺省值。
    //
    // 它是 const，但会通过指针去改表面缓存的 LRU 时间戳 —— 指针本身是 const，指向的
    // 对象不是，这和 DistanceFieldMarch 通过指针读距离场是同一回事。
    void FillSurfaceMaterial(const Vector3& hit_position,
                             const Vector3& hit_normal,
                             LumenRayTracingResult& out_result) const;
    // 距离场步进追踪（sphere tracing）。distance_field 由调用方显式给出，不再偷偷
    // 去读成员 —— 成员是 nullptr 时旧代码会直接返回"未命中"，看起来就跟场景是空的一样。
    bool DistanceFieldMarch(const Vector3& ray_origin,
                            const Vector3& ray_direction,
                            float max_distance,
                            LumenDistanceField* distance_field,
                            Vector3& out_hit_position,
                            Vector3& out_hit_normal,
                            float& out_hit_distance) const;

    // 初始化硬件光线追踪
    bool InitializeHardwareRayTracing();

    // 配置
    LumenConfig m_Config;

    // RHI
    RHI* m_Rhi;

    // 距离场引用
    LumenDistanceField* m_DistanceField = nullptr;

    // 表面缓存引用（命中点材质的来源）
    LumenSurfaceCacheManager* m_SurfaceCache = nullptr;

    // 硬件光线追踪支持
    bool m_HardwareRayTracingSupported = false;

    // 硬件光线追踪资源
    // TODO: 添加硬件光线追踪相关资源（加速结构等）
};