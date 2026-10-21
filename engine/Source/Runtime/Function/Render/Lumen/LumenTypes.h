#pragma once

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

// Lumen常量定义
namespace LumenConstants
{
    constexpr uint32_t kMaxTraceDistance = 10000;
    constexpr uint32_t kMaxReflectionBounces = 4;
    constexpr uint32_t kDefaultSurfaceCacheResolution = 512;
    constexpr uint32_t kDefaultDistanceFieldResolution = 256;
    // 体素化目前是 CPU 上的暴力实现（O(体素数 × 实例数))，256³ 的量级要跑几分钟。
    // 这里把分辨率压到 CPU 扛得住的上限；等距离场搬上 GPU 再放开。
    constexpr uint32_t kMaxCpuDistanceFieldResolution = 64;
    constexpr float kMinTraceStep = 0.1f;
    constexpr float kMaxTraceStep = 100.0f;
    // 软件光追（距离场 sphere tracing）的上限。球追踪在开阔空间里是几何收敛的，
    // 但贴着表面走时会退化成最小步长爬行，必须有个步数上限兜住。
    constexpr uint32_t kMaxSoftwareTraceSteps = 256;
    // 命中后用来把"跨过阈值的那一段"收敛到表面的二分次数。8 次把区间压到 1/256，
    // 已经远细于体素本身，再多没有意义。
    constexpr uint32_t kTraceBinaryRefinementSteps = 8;
    constexpr uint32_t kMaxSurfaceCachePages = 1024;
    constexpr uint32_t kSurfaceCachePageSize = 64;  // 64x64 texels per page
    // 一个页面空转多少帧之后允许被驱逐（60fps 下约 5 秒）。必须大于一帧，否则刚建好
    // 的页面会在被任何光线查到之前就被清掉。
    constexpr uint64_t kMaxSurfaceCachePageIdleFrames = 300;
    // GI probe 网格每轴的分辨率上限（32³ = 32768 个 probe）。CPU 侧一次 probe 就是
    // gi_sample_count 条光线，没有上限的话大场景里一次更新就能吃掉整帧。
    constexpr uint32_t kMaxGiProbeResolution = 32;
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
//
// 一个页面目前对应**一个场景实例**（= 一个 RenderEntity）。真正的 Lumen 是把网格拆成
// 若干张 axis-aligned 的 card，每张 card 光栅化出一张带材质的图集页；这一步必须在
// GPU 上做，CPU 路径没有光栅化器，所以这里退化成"一个实例一组常量材质"。
struct LumenSurfaceCachePage
{
    uint32_t page_id;
    // 每页 texel 边长。CPU 路径下是 0 —— 下面那些 per-texel 数组留空，等卡片光栅化
    // 真正存在了再填。以前这里无条件按 64×64 预分配 7 个数组，1024 页就是几百 MB，
    // 而每个 texel 存的还是同一个常量。
    uint32_t resolution;

    // 世界空间位置和方向
    Vector3 world_position;
    Vector3 normal;

    // 这个页面覆盖的世界空间区域（实例的世界 AABB）。查询页靠的是它，不是位置哈希。
    AxisAlignedBox world_bounds;

    // 常量材质（来自 RenderEntity 的 factor）。贴图在 CPU 上没法采样，所以这就是
    // CPU Lumen 今天能拿到的全部材质信息。
    Vector3 albedo {0.5f, 0.5f, 0.5f};
    float roughness {0.5f};
    float metallic {0.0f};
    Vector3 emissive {0.0f, 0.0f, 0.0f};

    // 来源实例，用于识别"命中了谁"
    uint64_t instance_id {0};

    // 缓存数据（per-texel，卡片光栅化就位后才有内容）
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

    float GetBoundsVolume() const
    {
        if (!world_bounds.IsValid())
        {
            return std::numeric_limits<float>::max();
        }
        const Vector3 size = world_bounds.getMaxCorner() - world_bounds.getMinCorner();
        return size.x * size.y * size.z;
    }
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

// GI 的产物：世界空间辐照度 probe 网格
//
// 网格锚定在距离场的世界盒上（**不跟着相机走**）—— 相机一动网格就重算的话，probe
// 的历史全丢，时间累积也就无从谈起。相机只用来决定"本帧优先更新哪些 probe"。
//
// probe 位于格心。每个 probe 存一个**无方向**的平均入射辐射（相当于 ambient 项）；
// 要做方向性的间接光需要 SH 或者六面辐照度，那是 GPU 路径的事。
struct LumenProbeGrid
{
    AxisAlignedBox world_bounds;
    Vector3 probe_spacing {1.0f, 1.0f, 1.0f};
    uint32_t resolution_x {0};
    uint32_t resolution_y {0};
    uint32_t resolution_z {0};

    // 每个 probe 累积的间接光
    std::vector<Vector3> irradiance;
    // 命中率：这个 probe 打出去的光线有多大比例撞到东西（0 = 四面全空，1 = 全封闭）
    std::vector<float> visibility;
    // 上次被更新的帧号；0 表示从没更新过
    std::vector<uint64_t> last_updated_frame;

    uint32_t GetProbeCount() const { return resolution_x * resolution_y * resolution_z; }

    bool IsValid() const
    {
        return GetProbeCount() > 0 && irradiance.size() == GetProbeCount() &&
               visibility.size() == GetProbeCount() && last_updated_frame.size() == GetProbeCount();
    }

    uint32_t GetProbeIndex(uint32_t x, uint32_t y, uint32_t z) const
    {
        return z * resolution_x * resolution_y + y * resolution_x + x;
    }

    Vector3 GetProbeWorldPosition(uint32_t index) const
    {
        const uint32_t x = resolution_x > 0 ? index % resolution_x : 0;
        const uint32_t y = resolution_x > 0 ? (index / resolution_x) % resolution_y : 0;
        const uint32_t z = resolution_x > 0 ? index / (resolution_x * resolution_y) : 0;

        return world_bounds.getMinCorner() + Vector3((static_cast<float>(x) + 0.5f) * probe_spacing.x,
                                                     (static_cast<float>(y) + 0.5f) * probe_spacing.y,
                                                     (static_cast<float>(z) + 0.5f) * probe_spacing.z);
    }

    void Reset()
    {
        world_bounds = AxisAlignedBox();
        probe_spacing = Vector3(1.0f, 1.0f, 1.0f);
        resolution_x = 0;
        resolution_y = 0;
        resolution_z = 0;
        irradiance.clear();
        visibility.clear();
        last_updated_frame.clear();
    }
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

    // GI probe 网格设置
    float gi_probe_spacing = 4.0f;         // 期望的 probe 间距（世界单位），会按分辨率上限被拉大
    uint32_t gi_probe_update_budget = 128;  // 每帧最多更新多少个 probe
    float gi_probe_max_distance = 200.0f;   // 离相机超过这个距离的 probe 本帧不更新
    float gi_temporal_blend_rate = 4.0f;    // 时间累积的收敛速率（每秒），0 = 完全不更新
    float gi_probe_max_surface_distance_factor = 2.0f;  // 离表面超过 (该值 × probe 间距) 的 probe 跳过

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
