#include "ScriptQuality.h"

#include <EASTL/string.h>

#include <algorithm>
#include <vector>

namespace
{
    // ---------------------------------------------------------------------------
    // 进程内状态
    //
    // 与 `Gizmos.color` / `TimeState` 同一类：进程级单例。放在函数内 static
    // 而不是文件级全局，是为了避开静态初始化顺序 —— 这些开关会被
    // TypeContainer 注册路径上的代码读，而那条路径跑在 main() 之前。
    // ---------------------------------------------------------------------------
    struct QualityState
    {
        int                level {2};   // Medium
        int                vsync_count {1};
        int                anti_aliasing {2};
        float              shadow_distance {40.0f};
        int                shadow_cascades {2};
        ShadowResolution   shadow_resolution {ShadowResolution::High};
        ShadowProjection   shadow_projection {ShadowProjection::StableFit};
        float              shadow_near_plane_offset {3.0f};
        int                pixel_light_count {2};
        int                master_texture_limit {0};
        AnisotropicFiltering anisotropic_filtering {AnisotropicFiltering::Enable};
        bool               soft_particles {true};
        bool               billboards_face_camera_position {true};
        SkinWeights        skin_weights {SkinWeights::FourBones};
        float              lod_bias {1.0f};
        int                maximum_lod_level {0};
        int                max_queued_frames {2};
        int                async_upload_time_slice {2};
        int                async_upload_buffer_size {16};
        int                particle_raycast_budget {4096};
    };

    QualityState& State()
    {
        static QualityState s_State;
        return s_State;
    }

    int ClampInt(int v, int lo, int hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    float ClampFloat(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // 夹到"不高于 v 的最近合法值"。MSAA 采样数与级联数都是离散合法集，
    // 向上取会让"我要 3x MSAA"变成"我要 4x" —— 那是**升级**，会让画质设置
    // 悄悄超出用户选的预算；向下取才是保守方向。
    int SnapDownTo(int v, const int* legal, std::size_t count)
    {
        int result = legal[0];
        for (std::size_t i = 0; i < count; ++i)
        {
            if (legal[i] <= v)
            {
                result = legal[i];
            }
        }
        return result;
    }

    int SnapAntiAliasing(int v)
    {
        static const int kLegal[] = {0, 2, 4, 8};
        return SnapDownTo(v, kLegal, 4);
    }

    int SnapCascades(int v)
    {
        static const int kLegal[] = {0, 2, 4};
        return SnapDownTo(v, kLegal, 3);
    }

    // ---------------------------------------------------------------------------
    // 预设表
    //
    // 只列"档位之间确实不同的"那几项（vsync 各档都是 1，所以不在表里 ——
    // 放在表里反而让人以为低档会关垂直同步）。
    //
    // ⚠️ 这些数字目前**没有**渲染代码消费（见头文件"接线状态"）。接一项进来
    // 就应该回来核一遍这个量级是否还合适 —— 照抄 Unity 的默认值但渲染器实现
    // 不同，是很常见的错。
    // ---------------------------------------------------------------------------
    struct Preset
    {
        int                anti_aliasing;
        float              shadow_distance;
        int                shadow_cascades;
        ShadowResolution   shadow_resolution;
        ShadowProjection   shadow_projection;
        int                pixel_light_count;
        int                master_texture_limit;
        AnisotropicFiltering anisotropic_filtering;
        bool               soft_particles;
        SkinWeights        skin_weights;
        float              lod_bias;
    };

    const Preset kPresets[] = {
        // 0 Very Low
        {0, 15.0f, 0, ShadowResolution::Low, ShadowProjection::CloseFit, 0, 2,
         AnisotropicFiltering::Disable, false, SkinWeights::OneBone, 0.3f},
        // 1 Low
        {0, 20.0f, 0, ShadowResolution::Low, ShadowProjection::CloseFit, 0, 1,
         AnisotropicFiltering::Disable, false, SkinWeights::TwoBones, 0.5f},
        // 2 Medium
        {2, 40.0f, 2, ShadowResolution::High, ShadowProjection::StableFit, 2, 0,
         AnisotropicFiltering::Enable, true, SkinWeights::FourBones, 1.0f},
        // 3 High
        {4, 70.0f, 4, ShadowResolution::High, ShadowProjection::StableFit, 3, 0,
         AnisotropicFiltering::Enable, true, SkinWeights::FourBones, 1.5f},
        // 4 Very High
        {8, 150.0f, 4, ShadowResolution::VeryHigh, ShadowProjection::StableFit, 4, 0,
         AnisotropicFiltering::ForceEnable, true, SkinWeights::Unlimited, 2.0f},
        // 5 Ultra
        {8, 300.0f, 4, ShadowResolution::VeryHigh, ShadowProjection::StableFit, 4, 0,
         AnisotropicFiltering::ForceEnable, true, SkinWeights::Unlimited, 2.0f},
    };

    const char* const kLevelNames[] = {
        "Very Low", "Low", "Medium", "High", "Very High", "Ultra",
    };

    constexpr int kLevelCount = static_cast<int>(sizeof(kLevelNames) / sizeof(kLevelNames[0]));

    void ApplyPreset(int level)
    {
        const Preset& preset     = kPresets[level];
        QualityState& state      = State();
        state.level              = level;
        state.anti_aliasing      = preset.anti_aliasing;
        state.shadow_distance    = preset.shadow_distance;
        state.shadow_cascades    = preset.shadow_cascades;
        state.shadow_resolution  = preset.shadow_resolution;
        state.shadow_projection  = preset.shadow_projection;
        state.pixel_light_count  = preset.pixel_light_count;
        state.master_texture_limit = preset.master_texture_limit;
        state.anisotropic_filtering = preset.anisotropic_filtering;
        state.soft_particles     = preset.soft_particles;
        state.skin_weights       = preset.skin_weights;
        state.lod_bias           = preset.lod_bias;
    }
}  // namespace

// =============================================================================
// 档位
// =============================================================================

int QualitySettings::GetQualityLevel()
{
    return State().level;
}

void QualitySettings::SetQualityLevel(int level)
{
    // 越界不动，与 Unity 一致（理由见头文件）。
    if (level < 0 || level >= kLevelCount)
    {
        return;
    }
    ApplyPreset(level);
}

void QualitySettings::IncreaseLevel()
{
    if (State().level + 1 < kLevelCount)
    {
        ApplyPreset(State().level + 1);
    }
}

void QualitySettings::DecreaseLevel()
{
    if (State().level - 1 >= 0)
    {
        ApplyPreset(State().level - 1);
    }
}

int QualitySettings::GetQualityLevelCount()
{
    return kLevelCount;
}

std::vector<eastl::string> QualitySettings::GetNames()
{
    std::vector<eastl::string> names;
    names.reserve(kLevelCount);
    for (int i = 0; i < kLevelCount; ++i)
    {
        names.emplace_back(kLevelNames[i]);
    }
    return names;
}

eastl::string QualitySettings::GetNameForLevel(int level)
{
    if (level < 0 || level >= kLevelCount)
    {
        return eastl::string();
    }
    return eastl::string(kLevelNames[level]);
}

eastl::string QualitySettings::GetName()
{
    return GetNameForLevel(State().level);
}

// =============================================================================
// 垂直同步 / 抗锯齿
// =============================================================================

int QualitySettings::GetVSyncCount()
{
    return State().vsync_count;
}

void QualitySettings::SetVSyncCount(int count)
{
    State().vsync_count = ClampInt(count, 0, 4);
}

int QualitySettings::GetAntiAliasing()
{
    return State().anti_aliasing;
}

void QualitySettings::SetAntiAliasing(int samples)
{
    State().anti_aliasing = SnapAntiAliasing(samples);
}

// =============================================================================
// 阴影
// =============================================================================

float QualitySettings::GetShadowDistance()
{
    return State().shadow_distance;
}

void QualitySettings::SetShadowDistance(float distance)
{
    State().shadow_distance = ClampFloat(distance, 0.0f, 100000.0f);
}

int QualitySettings::GetShadowCascades()
{
    return State().shadow_cascades;
}

void QualitySettings::SetShadowCascades(int cascades)
{
    State().shadow_cascades = SnapCascades(cascades);
}

ShadowResolution QualitySettings::GetShadowResolution()
{
    return State().shadow_resolution;
}

void QualitySettings::SetShadowResolution(ShadowResolution resolution)
{
    // 枚举是从脚本来的 int 强转，可能是任意值。落到合法集之外就当 Medium
    // —— 一个超出范围的分辨率档位会在建 shadow map 时才炸。
    switch (resolution)
    {
        case ShadowResolution::Low:
        case ShadowResolution::Medium:
        case ShadowResolution::High:
        case ShadowResolution::VeryHigh:
            State().shadow_resolution = resolution;
            break;
        default:
            State().shadow_resolution = ShadowResolution::Medium;
            break;
    }
}

ShadowProjection QualitySettings::GetShadowProjection()
{
    return State().shadow_projection;
}

void QualitySettings::SetShadowProjection(ShadowProjection projection)
{
    switch (projection)
    {
        case ShadowProjection::CloseFit:
        case ShadowProjection::StableFit:
            State().shadow_projection = projection;
            break;
        default:
            State().shadow_projection = ShadowProjection::StableFit;
            break;
    }
}

float QualitySettings::GetShadowNearPlaneOffset()
{
    return State().shadow_near_plane_offset;
}

void QualitySettings::SetShadowNearPlaneOffset(float offset)
{
    State().shadow_near_plane_offset = ClampFloat(offset, 0.0f, 100.0f);
}

// =============================================================================
// 光照 / 纹理
// =============================================================================

int QualitySettings::GetPixelLightCount()
{
    return State().pixel_light_count;
}

void QualitySettings::SetPixelLightCount(int count)
{
    State().pixel_light_count = ClampInt(count, 0, 64);
}

int QualitySettings::GetMasterTextureLimit()
{
    return State().master_texture_limit;
}

void QualitySettings::SetMasterTextureLimit(int limit)
{
    State().master_texture_limit = ClampInt(limit, 0, 3);
}

AnisotropicFiltering QualitySettings::GetAnisotropicFiltering()
{
    return State().anisotropic_filtering;
}

void QualitySettings::SetAnisotropicFiltering(AnisotropicFiltering filtering)
{
    switch (filtering)
    {
        case AnisotropicFiltering::Disable:
        case AnisotropicFiltering::Enable:
        case AnisotropicFiltering::ForceEnable:
            State().anisotropic_filtering = filtering;
            break;
        default:
            State().anisotropic_filtering = AnisotropicFiltering::Disable;
            break;
    }
}

// =============================================================================
// 粒子 / 蒙皮 / LOD
// =============================================================================

bool QualitySettings::GetSoftParticles()
{
    return State().soft_particles;
}

void QualitySettings::SetSoftParticles(bool enabled)
{
    State().soft_particles = enabled;
}

bool QualitySettings::GetBillboardsFaceCameraPosition()
{
    return State().billboards_face_camera_position;
}

void QualitySettings::SetBillboardsFaceCameraPosition(bool enabled)
{
    State().billboards_face_camera_position = enabled;
}

SkinWeights QualitySettings::GetSkinWeights()
{
    return State().skin_weights;
}

void QualitySettings::SetSkinWeights(SkinWeights weights)
{
    switch (weights)
    {
        case SkinWeights::OneBone:
        case SkinWeights::TwoBones:
        case SkinWeights::FourBones:
        case SkinWeights::Unlimited:
            State().skin_weights = weights;
            break;
        default:
            State().skin_weights = SkinWeights::FourBones;
            break;
    }
}

float QualitySettings::GetLodBias()
{
    return State().lod_bias;
}

void QualitySettings::SetLodBias(float bias)
{
    State().lod_bias = ClampFloat(bias, 0.0f, 10.0f);
}

int QualitySettings::GetMaximumLODLevel()
{
    return State().maximum_lod_level;
}

void QualitySettings::SetMaximumLODLevel(int level)
{
    State().maximum_lod_level = ClampInt(level, 0, 16);
}

// =============================================================================
// 上传 / 排队
// =============================================================================

int QualitySettings::GetMaxQueuedFrames()
{
    return State().max_queued_frames;
}

void QualitySettings::SetMaxQueuedFrames(int frames)
{
    State().max_queued_frames = ClampInt(frames, 1, 8);
}

int QualitySettings::GetAsyncUploadTimeSlice()
{
    return State().async_upload_time_slice;
}

void QualitySettings::SetAsyncUploadTimeSlice(int milliseconds)
{
    State().async_upload_time_slice = ClampInt(milliseconds, 1, 32);
}

int QualitySettings::GetAsyncUploadBufferSize()
{
    return State().async_upload_buffer_size;
}

void QualitySettings::SetAsyncUploadBufferSize(int megabytes)
{
    State().async_upload_buffer_size = ClampInt(megabytes, 1, 1024);
}

int QualitySettings::GetParticleRaycastBudget()
{
    return State().particle_raycast_budget;
}

void QualitySettings::SetParticleRaycastBudget(int budget)
{
    State().particle_raycast_budget = ClampInt(budget, 1, 4096);
}

// =============================================================================
// 复位
// =============================================================================

void QualitySettings::ResetToDefaults()
{
    State() = QualityState {};
    ApplyPreset(State().level);
}
