#pragma once

#include "Runtime/Core/Math/Matrix4.h"

#include <cstdint>

namespace MegaLights
{

constexpr uint32_t kTileSize = 8;
constexpr uint32_t kMaxLocalLights = 1024;
constexpr uint32_t kMaxLightsPerTile = 64;
constexpr uint32_t kDefaultSamplesPerPixel = 4;
constexpr uint32_t kDefaultScreenSpaceShadowSteps = 12;
constexpr uint32_t kViewportHistoryCount = 2;
constexpr float kDefaultTemporalBlend = 0.9f;
constexpr float kDefaultDisocclusionThreshold = 0.02f;
constexpr uint32_t kDefaultSpatialRadius = 1;
constexpr float kDefaultSpatialDepthSigma = 0.01f;
constexpr float kDefaultSpatialNormalPower = 32.0f;

// std430 header before MegaLightGpu array in binding 8.
struct MegaLightsHeaderGpu
{
    uint32_t light_count {0};
    uint32_t tile_count_x {0};
    uint32_t tile_count_y {0};
    uint32_t num_samples {kDefaultSamplesPerPixel};
    uint32_t frame_index {0};
    uint32_t ss_steps {kDefaultScreenSpaceShadowSteps};
    uint32_t viewport_width {0};
    uint32_t viewport_height {0};
    uint32_t temporal_enable {0};
    uint32_t history_valid {0};
    float temporal_blend {kDefaultTemporalBlend};
    float disocclusion_threshold {kDefaultDisocclusionThreshold};
    float prev_proj_view[16] {};
    uint32_t spatial_enable {0};
    uint32_t spatial_radius {kDefaultSpatialRadius};
    float spatial_depth_sigma {kDefaultSpatialDepthSigma};
    float spatial_normal_power {kDefaultSpatialNormalPower};
};

struct MegaLightGpu
{
    float position_radius[4] {0.0f, 0.0f, 0.0f, 1.0f};
    float intensity[4] {0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(MegaLightsHeaderGpu) == 128, "MegaLightsHeaderGpu must match GLSL/HLSL std430 header");

inline void CopyMatrixToHeader(float* dst, const Matrix4x4& matrix)
{
    for (uint32_t row = 0; row < 4; ++row)
    {
        for (uint32_t col = 0; col < 4; ++col)
        {
            dst[row * 4 + col] = matrix.m_Mat[row][col];
        }
    }
}

}  // namespace MegaLights
