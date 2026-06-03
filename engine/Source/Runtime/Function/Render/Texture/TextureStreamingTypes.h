#pragma once

#include "Runtime/Core/Base/Hash.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#if defined(__APPLE__) || defined(__EMSCRIPTEN__)
using VmaAllocation = void*;
#else
    #include <vma/vk_mem_alloc.h>
#endif

// Forward declarations
class RHI;
class TextureStreamingManager;

/// <summary>
/// Texture streaming state
/// </summary>
enum class TextureStreamingState : uint8_t
{
    Unloaded = 0,  // Texture is not loaded
    Loading,       // Texture is being loaded
    Loaded,        // Texture is fully loaded
    Unloading,     // Texture is being unloaded
    Error          // Error occurred during loading
};

/// <summary>
/// Texture streaming priority
/// </summary>
enum class TextureStreamingPriority : uint8_t
{
    Low = 0,
    Normal,
    High,
    Critical
};

/// <summary>
/// Texture streaming request
/// </summary>
struct TextureStreamingRequest
{
    eastl::string texture_path;         // Path to texture file
    uint32_t requested_mip_level;       // Requested mip level (0 = full resolution)
    TextureStreamingPriority priority;  // Streaming priority
    float distance;                     // Distance from camera
    float screen_size;                  // Screen space size
    bool is_visible;                    // Is texture currently visible
    uint64_t request_id;                // Unique request ID
    TextureStreamingState state;        // Current state

    TextureStreamingRequest()
        : requested_mip_level(0), priority(TextureStreamingPriority::Normal), distance(0.0f), screen_size(0.0f),
          is_visible(false), request_id(0), state(TextureStreamingState::Unloaded)
    {
    }
};

/// <summary>
/// Streamable texture resource
/// Represents a texture that can be streamed with different mip levels
/// </summary>
class StreamableTexture
{
public:
    StreamableTexture();
    ~StreamableTexture();

    // Initialize texture with file path
    void Initialize(const eastl::string& path, bool is_srgb = false);

    // Get texture path
    const eastl::string& getPath() const { return m_Path; }

    // Get current loaded mip level
    uint32_t getCurrentMipLevel() const { return m_CurrentMipLevel; }

    // Get maximum mip levels
    uint32_t getMaxMipLevels() const { return m_MaxMipLevels; }

    // Get texture dimensions at a specific mip level
    void GetDimensionsAtMip(uint32_t mip_level, uint32_t& width, uint32_t& height) const;

    // Get GPU texture resources
    RHIImage* getImage() const { return m_Image; }
    RHIImageView* GetImageView() const { return m_ImageView; }
    VmaAllocation getAllocation() const { return m_Allocation; }

    // Get texture format
    RHIFormat getFormat() const { return m_Format; }

    // Check if texture is loaded
    bool isLoaded() const { return m_State == TextureStreamingState::Loaded && m_Image != nullptr; }

    // Get streaming state
    TextureStreamingState getState() const { return m_State; }

    // Set streaming state (internal use)
    void setState(TextureStreamingState state) { m_State = state; }

    // Set GPU resources (internal use)
    void setGPUResources(RHIImage* image, RHIImageView* image_view, VmaAllocation allocation)
    {
        m_Image = image;
        m_ImageView = image_view;
        m_Allocation = allocation;
    }

    // Set texture data (internal use)
    void setTextureData(std::shared_ptr<TextureData> data, uint32_t mip_level)
    {
        m_TextureData = data;
        m_CurrentMipLevel = mip_level;

        // Initialize dimensions if not set
        if (data && m_BaseWidth == 0 && m_BaseHeight == 0)
        {
            m_BaseWidth = data->m_Width;
            m_BaseHeight = data->m_Height;
            m_Format = data->m_Format;
            m_MaxMipLevels = CalculateMipLevels(m_BaseWidth, m_BaseHeight);
        }
    }

    // Get texture data
    std::shared_ptr<TextureData> getTextureData() const { return m_TextureData; }

    // Calculate required mip level based on distance and screen size
    uint32_t CalculateRequiredMipLevel(float distance, float screen_size, float fov) const;

    // Get memory size at a specific mip level
    size_t GetMemorySizeAtMip(uint32_t mip_level) const;

    // Get total memory size
    size_t GetTotalMemorySize() const;

private:
    eastl::string m_Path;           // Texture file path
    bool m_IsSrgb;                  // Is sRGB texture
    uint32_t m_BaseWidth;           // Base width (mip 0)
    uint32_t m_BaseHeight;          // Base height (mip 0)
    uint32_t m_MaxMipLevels;        // Maximum mip levels
    uint32_t m_CurrentMipLevel;     // Currently loaded mip level
    RHIFormat m_Format;             // Texture format
    TextureStreamingState m_State;  // Current state

    // GPU resources
    RHIImage* m_Image;
    RHIImageView* m_ImageView;
    VmaAllocation m_Allocation;

    // CPU texture data (cached)
    std::shared_ptr<TextureData> m_TextureData;

    // Calculate mip levels from dimensions
    uint32_t CalculateMipLevels(uint32_t width, uint32_t height) const;
};

/// <summary>
/// Texture streaming statistics
/// </summary>
struct TextureStreamingStats
{
    uint32_t total_textures;     // Total number of streamable textures
    uint32_t loaded_textures;    // Number of loaded textures
    uint32_t loading_textures;   // Number of textures being loaded
    uint32_t unloaded_textures;  // Number of unloaded textures
    size_t total_memory_used;    // Total GPU memory used (bytes)
    size_t memory_budget;        // Memory budget (bytes)
    float memory_usage_ratio;    // Memory usage ratio (0.0 - 1.0)
};

/// <summary>
/// Texture streaming configuration
/// </summary>
struct TextureStreamingConfig
{
    size_t memory_budget_mb;              // Memory budget in MB
    uint32_t max_concurrent_loads;        // Maximum concurrent texture loads
    float min_screen_size;                // Minimum screen size to load texture
    float max_distance;                   // Maximum distance to load texture
    float streaming_distance_multiplier;  // Distance multiplier for streaming
    bool enable_async_loading;            // Enable asynchronous loading
    bool enable_mip_streaming;            // Enable mip level streaming

    TextureStreamingConfig()
        : memory_budget_mb(512), max_concurrent_loads(4), min_screen_size(0.01f), max_distance(1000.0f),
          streaming_distance_multiplier(1.0f), enable_async_loading(true), enable_mip_streaming(true)
    {
    }
};