#pragma once

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <cstdint>
#include <memory>
#include <vector>

// Lumen常量定义
namespace LumenConstants
{
    constexpr uint32_t kMaxTraceDistance = 10000;
    constexpr uint32_t kMaxReflectionBounces = 4;
    constexpr uint32_t kDefaultSurfaceCacheResolution = 512;
    constexpr uint32_t kDefaultDistanceFieldResolution = 256;
    constexpr float kMinTraceStep = 0.1f;
    constexpr float kMaxTraceStep = 100.0f;
    constexpr uint32_t kMaxSurfaceCachePages = 1024;
    constexpr uint32_t kSurfaceCachePageSize = 64;  // 64x64 texels per page
}  // namespace LumenConstants

// Lumen质量等级
enum class LumenQuality : uint8_t
{
    Low,
    Medium,
    High,
    Epic
};

// 距离场类型
enum class DistanceFieldType : uint8_t
{
    Global,       // 全局距离场
    Hierarchical  // 层次距离场
};

// 光线追踪类型
enum class RayTracingType : uint8_t
{
    Hardware,  // 硬件光线追踪
    Software,  // 软件光线追踪（基于距离场）
    Hybrid     // 混合模式
};

// 距离场数据结构
struct LumenDistanceFieldData
{
    // 唯一标识
    uint64_t distance_field_id;

    // 分辨率
    uint32_t resolution_x;
    uint32_t resolution_y;
    uint32_t resolution_z;

    // 世界空间边界
    AxisAlignedBox world_bounds;

    // 距离场数据（有符号距离值）
    std::vector<float> distance_data;

    // 层次结构（用于层次距离场）
    std::vector<LumenDistanceFieldData> hierarchy_levels;

    // 类型
    DistanceFieldType type;

    // 更新标志
    bool needs_update;
    bool is_loaded;
};

// 表面缓存页面
struct LumenSurfaceCachePage
{
    uint32_t page_id;
    uint32_t resolution;

    // 世界空间位置和方向
    Vector3 world_position;
    Vector3 normal;

    // 缓存数据
    std::vector<Vector4> albedo_data;     // RGB + Alpha
    std::vector<Vector4> normal_data;     // XYZ + unused
    std::vector<Vector4> roughness_data;  // R + unused
    std::vector<Vector4> metallic_data;   // R + unused
    std::vector<Vector4> emissive_data;   // RGB + Intensity

    // 光照数据
    std::vector<Vector3> indirect_lighting;
    std::vector<float> visibility;

    // LOD级别
    uint8_t lod_level;

    // 时间戳（用于缓存管理）
    uint64_t last_access_time;
    bool is_valid;
};

// 表面缓存
struct LumenSurfaceCache
{
    // 唯一标识
    uint64_t cache_id;

    // 页面列表
    std::vector<LumenSurfaceCachePage> pages;

    // 分辨率
    uint32_t base_resolution;

    // 世界空间边界
    AxisAlignedBox world_bounds;

    // 统计信息
    uint32_t active_page_count;
    uint32_t total_page_count;

    // 更新标志
    bool needs_update;
};

// 光线追踪结果
struct LumenRayTracingResult
{
    // 是否命中
    bool hit;

    // 命中位置
    Vector3 hit_position;

    // 命中距离
    float hit_distance;

    // 命中法线
    Vector3 hit_normal;

    // 材质信息
    Vector3 albedo;
    float roughness;
    float metallic;
    Vector3 emissive;

    // 实例ID（用于识别命中的对象）
    uint64_t instance_id;

    // 追踪类型
    RayTracingType tracing_type;
};

// 全局光照数据
struct LumenGlobalIlluminationData
{
    // 间接光照颜色
    Vector3 indirect_lighting;

    // 光照方向（主要方向）
    Vector3 lighting_direction;

    // 可见性
    float visibility;

    // 反射次数
    uint8_t bounce_count;

    // 时间戳（用于时间累积）
    uint64_t timestamp;
};

// 反射数据
struct LumenReflectionData
{
    // 反射颜色
    Vector3 reflection_color;

    // 反射方向
    Vector3 reflection_direction;

    // 反射距离
    float reflection_distance;

    // 反射类型
    RayTracingType reflection_type;

    // 粗糙度影响
    float roughness_factor;
};

// Lumen配置
struct LumenConfig
{
    // 基本设置
    bool enabled = true;
    bool enable_global_illumination = true;
    bool enable_reflections = true;

    // 光线追踪设置
    bool use_hardware_ray_tracing = false;  // 默认使用软件追踪
    bool use_software_ray_tracing = true;
    RayTracingType ray_tracing_type = RayTracingType::Software;

    // 追踪参数
    float max_trace_distance = static_cast<float>(LumenConstants::kMaxTraceDistance);
    uint32_t max_reflection_bounces = LumenConstants::kMaxReflectionBounces;
    float min_trace_step = LumenConstants::kMinTraceStep;
    float max_trace_step = LumenConstants::kMaxTraceStep;

    // 表面缓存设置
    uint32_t surface_cache_resolution = LumenConstants::kDefaultSurfaceCacheResolution;
    uint32_t surface_cache_page_size = LumenConstants::kSurfaceCachePageSize;
    uint32_t max_surface_cache_pages = LumenConstants::kMaxSurfaceCachePages;

    // 距离场设置
    uint32_t distance_field_resolution = LumenConstants::kDefaultDistanceFieldResolution;
    bool use_hierarchical_distance_field = true;
    bool enable_distance_field_streaming = true;

    // 质量设置
    LumenQuality quality = LumenQuality::High;

    // 性能设置
    bool enable_temporal_accumulation = true;
    bool enable_spatial_reuse = true;
    uint32_t gi_sample_count = 1;
    uint32_t reflection_sample_count = 1;

    // 调试设置
    bool debug_visualize_distance_field = false;
    bool debug_visualize_surface_cache = false;
    bool debug_visualize_ray_tracing = false;
};

// Lumen统计信息
struct LumenStats
{
    // 距离场统计
    uint32_t distance_field_update_count;
    float distance_field_update_time_ms;

    // 表面缓存统计
    uint32_t surface_cache_update_count;
    float surface_cache_update_time_ms;

    // 光线追踪统计
    uint32_t ray_tracing_count;
    float ray_tracing_time_ms;
    uint32_t hardware_ray_tracing_count;
    uint32_t software_ray_tracing_count;

    // 全局光照统计
    uint32_t gi_compute_count;
    float gi_compute_time_ms;

    // 反射统计
    uint32_t reflection_compute_count;
    float reflection_compute_time_ms;

    // 内存使用
    uint64_t distance_field_memory_bytes;
    uint64_t surface_cache_memory_bytes;
    uint64_t total_memory_bytes;
};
