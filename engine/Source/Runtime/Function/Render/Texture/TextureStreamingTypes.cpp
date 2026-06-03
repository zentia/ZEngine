#include "TextureStreamingTypes.h"

#include "Runtime/Core/Base/Macro.h"

#include <algorithm>
#include <cmath>

StreamableTexture::StreamableTexture()
    : m_IsSrgb(false), m_BaseWidth(0), m_BaseHeight(0), m_MaxMipLevels(0), m_CurrentMipLevel(0),
      m_Format(RHI_FORMAT_MAX_ENUM), m_State(TextureStreamingState::Unloaded), m_Image(nullptr), m_ImageView(nullptr),
      m_Allocation(nullptr)
{
}

StreamableTexture::~StreamableTexture()
{
    // GPU resources should be released by TextureStreamingManager
    m_Image = nullptr;
    m_ImageView = nullptr;
    m_Allocation = nullptr;
    m_TextureData.reset();
}

void StreamableTexture::Initialize(const eastl::string& path, bool is_srgb)
{
    m_Path = path;
    m_IsSrgb = is_srgb;
    m_State = TextureStreamingState::Unloaded;
}

void StreamableTexture::GetDimensionsAtMip(uint32_t mip_level, uint32_t& width, uint32_t& height) const
{
    if (mip_level >= m_MaxMipLevels)
    {
        width = 1;
        height = 1;
        return;
    }

    width = std::max(1u, m_BaseWidth >> mip_level);
    height = std::max(1u, m_BaseHeight >> mip_level);
}

uint32_t StreamableTexture::CalculateRequiredMipLevel(float distance, float screen_size, float fov) const
{
    if (!m_TextureData || m_MaxMipLevels == 0)
        return 0;

    // Calculate required mip level based on screen size
    // Larger screen size = lower mip level (higher resolution)
    // Smaller screen size = higher mip level (lower resolution)

    // Simple heuristic: use screen size to determine mip level
    // This is a simplified version - UE uses more complex calculations
    float mip_bias = 0.0f;

    // Adjust based on distance
    if (distance > 0.0f)
    {
        // Further away = higher mip level
        float distance_factor = std::min(1.0f, distance / 100.0f);
        mip_bias += distance_factor * 2.0f;
    }

    // Adjust based on screen size
    if (screen_size > 0.0f)
    {
        // Smaller screen size = higher mip level
        float size_factor = 1.0f - std::min(1.0f, screen_size);
        mip_bias += size_factor * 2.0f;
    }

    uint32_t required_mip = static_cast<uint32_t>(std::floor(mip_bias));
    return std::min(required_mip, m_MaxMipLevels - 1);
}

size_t StreamableTexture::GetMemorySizeAtMip(uint32_t mip_level) const
{
    if (mip_level >= m_MaxMipLevels)
        return 0;

    uint32_t width, height;
    GetDimensionsAtMip(mip_level, width, height);

    // Calculate size based on format
    size_t bytes_per_pixel = 4;  // Default to RGBA8
    switch (m_Format)
    {
        case RHI_FORMAT_R8G8B8A8_UNORM:
        case RHI_FORMAT_R8G8B8A8_SRGB:
            bytes_per_pixel = 4;
            break;
        case RHI_FORMAT_R32G32B32A32_SFLOAT:
            bytes_per_pixel = 16;
            break;
        case RHI_FORMAT_R32G32_SFLOAT:
            bytes_per_pixel = 8;
            break;
        default:
            bytes_per_pixel = 4;
            break;
    }

    return width * height * bytes_per_pixel;
}

size_t StreamableTexture::GetTotalMemorySize() const
{
    size_t total_size = 0;
    for (uint32_t i = 0; i < m_MaxMipLevels; ++i)
    {
        total_size += GetMemorySizeAtMip(i);
    }
    return total_size;
}

uint32_t StreamableTexture::CalculateMipLevels(uint32_t width, uint32_t height) const
{
    uint32_t max_dimension = std::max(width, height);
    return static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(max_dimension)))) + 1;
}