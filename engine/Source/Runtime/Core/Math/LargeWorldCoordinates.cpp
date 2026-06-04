#include "Runtime/Core/Math/LargeWorldCoordinates.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace LargeWorldCoordinates
{
namespace
{
    std::atomic<bool> g_enabled {false};
    Vector3d g_render_tile {0.0, 0.0, 0.0};

    Vector3d SnapWorldPositionToRenderTile(Vector3d world_position)
    {
        const double tile = k_render_tile_size;
        return Vector3d(std::floor(world_position.x / tile) * tile,
                        std::floor(world_position.y / tile) * tile,
                        std::floor(world_position.z / tile) * tile);
    }

    bool EqualsIgnoreCase(const char* a, const char* b)
    {
        while (*a != '\0' && *b != '\0')
        {
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            {
                return false;
            }
            ++a;
            ++b;
        }
        return *a == *b;
    }

    void ApplyEnvOverrideOnce()
    {
        static bool s_applied = false;
        if (s_applied)
        {
            return;
        }
        s_applied = true;

        const char* env = std::getenv("ZENGINE_LWC_ENABLE");
        if (env == nullptr || env[0] == '\0')
        {
            return;
        }
        if (env[0] == '1' || EqualsIgnoreCase(env, "true") || EqualsIgnoreCase(env, "on"))
        {
            g_enabled.store(true, std::memory_order_relaxed);
        }
    }
}  // namespace

bool IsEnabled()
{
    ApplyEnvOverrideOnce();
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_relaxed);
}

Vector3d GetRenderTile()
{
    return g_render_tile;
}

void SetRenderTileFromWorldPosition(Vector3d world_position)
{
    g_render_tile = SnapWorldPositionToRenderTile(world_position);
}

Vector3 WorldToRender(Vector3d world_position)
{
    const Vector3d offset = world_position - g_render_tile;
    return offset.ToVector3();
}

Vector3d RenderToWorld(Vector3 render_position)
{
    const Vector3d render_offset(render_position);
    return g_render_tile + render_offset;
}

Vector3 GetPreViewTranslation(Vector3d view_origin_world)
{
    const Vector3d offset = view_origin_world - g_render_tile;
    return -offset.ToVector3();
}

Matrix4x4 ApplyRenderOriginToModelMatrix(const Matrix4x4& world_matrix)
{
    if (!IsEnabled())
    {
        return world_matrix;
    }

    Vector3 translation;
    Vector3 scale;
    Quaternion rotation;
    world_matrix.Decomposition(translation, scale, rotation);

    const Vector3d world_translation(translation);
    const Vector3 render_translation = WorldToRender(world_translation);

    return Matrix4x4::GetTrans(render_translation) * Matrix4x4(rotation) *
           Matrix4x4::BuildScaleMatrix(scale.x, scale.y, scale.z);
}

}  // namespace LargeWorldCoordinates
