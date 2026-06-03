#pragma once

#include "Runtime/Function/Render/RenderResourceBase.h"
#include "TextureStreamingTypes.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

class RHI;
class RenderResourceBase;

/// <summary>
/// Texture Streaming Manager
/// Manages texture streaming based on distance, screen size, and priority
/// Similar to UE's TextureStreaming system
/// </summary>
class TextureStreamingManager
{
public:
    TextureStreamingManager();
    ~TextureStreamingManager();

    // Initialize the streaming manager
    void Initialize(RHI* rhi, RenderResourceBase* render_resource);

    // Shutdown the streaming manager
    void Shutdown();

    // Register a texture for streaming
    // Returns a handle to the streamable texture
    uint64_t RegisterTexture(const eastl::string& texture_path, bool is_srgb = false);

    // Unregister a texture
    void UnregisterTexture(uint64_t texture_handle);

    // Request texture streaming at a specific mip level
    void RequestTextureStreaming(uint64_t texture_handle,
                                 uint32_t requested_mip_level,
                                 TextureStreamingPriority priority = TextureStreamingPriority::Normal,
                                 float distance = 0.0f,
                                 float screen_size = 0.0f,
                                 bool is_visible = true);

    // Update streaming requests based on camera position and view
    void UpdateStreaming(const Vector3& camera_position,
                         const Vector3& camera_forward,
                         float fov,
                         float aspect_ratio,
                         uint32_t screen_width,
                         uint32_t screen_height);

    // Process streaming requests (call each frame)
    void Tick(float delta_time);

    // Get streamable texture by handle
    std::shared_ptr<StreamableTexture> GetStreamableTexture(uint64_t texture_handle) const;

    // Get texture GPU resources
    bool GetTextureResources(uint64_t texture_handle,
                             RHIImage*& image,
                             RHIImageView*& image_view,
                             VmaAllocation& allocation) const;

    // Get streaming statistics
    TextureStreamingStats GetStats() const;

    // Set streaming configuration
    void SetConfig(const TextureStreamingConfig& config);

    // Get streaming configuration
    const TextureStreamingConfig& getConfig() const { return m_Config; }

    // Force load a texture at full resolution (blocking)
    bool ForceLoadTexture(uint64_t texture_handle, uint32_t mip_level = 0);

    // Unload a texture
    void UnloadTexture(uint64_t texture_handle);

    // Check if texture is loaded
    bool IsTextureLoaded(uint64_t texture_handle) const;

private:
    // Internal texture handle type
    using TextureHandle = uint64_t;

    // Texture entry
    struct TextureEntry
    {
        TextureHandle handle;
        std::shared_ptr<StreamableTexture> texture;
        TextureStreamingRequest current_request;
        bool is_registered;

        TextureEntry()
            : handle(0), is_registered(false) {}
    };

    // Loading task
    struct LoadingTask
    {
        TextureHandle texture_handle;
        uint32_t mip_level;
        TextureStreamingPriority priority;
        std::shared_ptr<TextureData> texture_data;

        bool operator<(const LoadingTask& other) const
        {
            return static_cast<uint8_t>(priority) < static_cast<uint8_t>(other.priority);
        }
    };

    // Process loading queue
    void ProcessLoadingQueue();

    // Load texture data from file
    std::shared_ptr<TextureData> LoadTextureData(const eastl::string& path, bool is_srgb, uint32_t mip_level);

    // Create GPU resources for texture
    bool CreateGPUResources(std::shared_ptr<StreamableTexture> texture,
                            std::shared_ptr<TextureData> texture_data,
                            uint32_t mip_level);

    // Release GPU resources for texture
    void ReleaseGPUResources(std::shared_ptr<StreamableTexture> texture);

    // Calculate required mip level for a texture
    uint32_t CalculateRequiredMipLevel(std::shared_ptr<StreamableTexture> texture,
                                       const Vector3& camera_position,
                                       const Vector3& object_position,
                                       float fov,
                                       float screen_size) const;

    // Update texture streaming priority
    void UpdateTexturePriority(TextureEntry& entry, const Vector3& camera_position, float fov, float screen_size);

    // Check memory budget
    bool CheckMemoryBudget(size_t additional_memory) const;

    // Evict textures if needed
    void EvictTextures(size_t required_memory);

    // Get memory usage
    size_t GetCurrentMemoryUsage() const;

    // Thread-safe texture map access
    std::shared_ptr<StreamableTexture> GetTextureInternal(TextureHandle handle) const;

    RHI* m_Rhi;
    RenderResourceBase* m_RenderResource;  // Non-owning pointer

    // Texture registry
    mutable std::mutex m_TextureMutex;
    std::unordered_map<TextureHandle, TextureEntry> m_Textures;
    TextureHandle m_NextHandle;

    // Loading queue
    std::mutex m_LoadingMutex;
    std::priority_queue<LoadingTask> m_LoadingQueue;
    std::vector<LoadingTask> m_ActiveLoads;
    std::atomic<uint32_t> m_ActiveLoadCount;

    // Configuration
    TextureStreamingConfig m_Config;

    // Statistics
    mutable std::mutex m_StatsMutex;
    TextureStreamingStats m_Stats;

    // Threading
    std::thread m_LoadingThread;
    std::atomic<bool> m_ShouldStop;
    std::condition_variable m_LoadingCv;

    // Camera state (for updateStreaming)
    Vector3 m_CameraPosition;
    Vector3 m_CameraForward;
    float m_Fov;
    float m_AspectRatio;
    uint32_t m_ScreenWidth;
    uint32_t m_ScreenHeight;
};