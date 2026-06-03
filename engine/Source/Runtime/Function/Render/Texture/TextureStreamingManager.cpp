#include "TextureStreamingManager.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResourceBase.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

TextureStreamingManager::TextureStreamingManager()
    : m_NextHandle(1), m_ActiveLoadCount(0), m_ShouldStop(false), m_Fov(60.0f), m_AspectRatio(16.0f / 9.0f),
      m_ScreenWidth(1920), m_ScreenHeight(1080)
{
    m_Config = TextureStreamingConfig();
}

TextureStreamingManager::~TextureStreamingManager()
{
    Shutdown();
}

void TextureStreamingManager::Initialize(RHI* rhi, RenderResourceBase* render_resource)
{
    m_Rhi = rhi;
    m_RenderResource = render_resource;
    m_ShouldStop = false;

    // Start loading thread if async loading is enabled
    if (m_Config.enable_async_loading)
    {
        m_LoadingThread = std::thread([this]() {
            while (!m_ShouldStop)
            {
                ProcessLoadingQueue();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps
            }
        });
    }
}

void TextureStreamingManager::Shutdown()
{
    m_ShouldStop = true;

    if (m_LoadingThread.joinable())
    {
        m_LoadingCv.notify_all();
        m_LoadingThread.join();
    }

    // Release all textures
    std::lock_guard<std::mutex> lock(m_TextureMutex);
    for (auto& pair : m_Textures)
    {
        if (pair.second.texture)
        {
            ReleaseGPUResources(pair.second.texture);
        }
    }
    m_Textures.clear();
}

uint64_t TextureStreamingManager::RegisterTexture(const eastl::string& texture_path, bool is_srgb)
{
    std::lock_guard<std::mutex> lock(m_TextureMutex);

    TextureHandle handle = m_NextHandle++;
    TextureEntry entry;
    entry.handle = handle;
    entry.texture = std::make_shared<StreamableTexture>();
    entry.texture->Initialize(texture_path, is_srgb);
    entry.is_registered = true;

    m_Textures[handle] = entry;

    return handle;
}

void TextureStreamingManager::UnregisterTexture(uint64_t texture_handle)
{
    std::lock_guard<std::mutex> lock(m_TextureMutex);

    auto it = m_Textures.find(texture_handle);
    if (it != m_Textures.end())
    {
        if (it->second.texture)
        {
            ReleaseGPUResources(it->second.texture);
        }
        m_Textures.erase(it);
    }
}

void TextureStreamingManager::RequestTextureStreaming(uint64_t texture_handle,
                                                      uint32_t requested_mip_level,
                                                      TextureStreamingPriority priority,
                                                      float distance,
                                                      float screen_size,
                                                      bool is_visible)
{
    std::lock_guard<std::mutex> lock(m_TextureMutex);

    auto it = m_Textures.find(texture_handle);
    if (it == m_Textures.end())
        return;

    TextureEntry& entry = it->second;
    if (!entry.is_registered || !entry.texture)
        return;

    // Update request
    entry.current_request.texture_path = entry.texture->getPath();
    entry.current_request.requested_mip_level = requested_mip_level;
    entry.current_request.priority = priority;
    entry.current_request.distance = distance;
    entry.current_request.screen_size = screen_size;
    entry.current_request.is_visible = is_visible;
    entry.current_request.request_id = texture_handle;

    // Check if we need to load/unload
    auto current_state = entry.texture->getState();
    auto current_mip = entry.texture->getCurrentMipLevel();

    if (current_state == TextureStreamingState::Unloaded || current_mip != requested_mip_level)
    {
        // Need to load or reload
        entry.current_request.state = TextureStreamingState::Loading;

        // Add to loading queue
        LoadingTask task;
        task.texture_handle = texture_handle;
        task.mip_level = requested_mip_level;
        task.priority = priority;

        std::lock_guard<std::mutex> loading_lock(m_LoadingMutex);
        m_LoadingQueue.push(task);
        m_LoadingCv.notify_one();
    }
}

void TextureStreamingManager::UpdateStreaming(const Vector3& camera_position,
                                              const Vector3& camera_forward,
                                              float fov,
                                              float aspect_ratio,
                                              uint32_t screen_width,
                                              uint32_t screen_height)
{
    m_CameraPosition = camera_position;
    m_CameraForward = camera_forward;
    m_Fov = fov;
    m_AspectRatio = aspect_ratio;
    m_ScreenWidth = screen_width;
    m_ScreenHeight = screen_height;

    std::lock_guard<std::mutex> lock(m_TextureMutex);

    // Update priorities and requests for all textures
    for (auto& pair : m_Textures)
    {
        TextureEntry& entry = pair.second;
        if (!entry.is_registered || !entry.texture)
            continue;

        // Calculate screen size and distance (simplified)
        // In a real implementation, you would calculate this based on object position
        float screen_size = 1.0f;  // Placeholder
        float distance = 0.0f;     // Placeholder

        UpdateTexturePriority(entry, camera_position, fov, screen_size);

        // Calculate required mip level
        uint32_t required_mip = entry.texture->CalculateRequiredMipLevel(distance, screen_size, fov);

        // Request streaming if needed
        if (entry.texture->getState() == TextureStreamingState::Unloaded ||
            entry.texture->getCurrentMipLevel() != required_mip)
        {
            RequestTextureStreaming(
                pair.first, required_mip, entry.current_request.priority, distance, screen_size, true);
        }
    }
}

void TextureStreamingManager::Tick(float delta_time)
{
    // Process completed loads
    std::lock_guard<std::mutex> loading_lock(m_LoadingMutex);
    auto it = m_ActiveLoads.begin();
    while (it != m_ActiveLoads.end())
    {
        if (it->texture_data != nullptr)
        {
            // Load completed, create GPU resources
            std::lock_guard<std::mutex> texture_lock(m_TextureMutex);
            auto texture_it = m_Textures.find(it->texture_handle);
            if (texture_it != m_Textures.end())
            {
                auto& entry = texture_it->second;
                if (entry.texture)
                {
                    CreateGPUResources(entry.texture, it->texture_data, it->mip_level);
                    entry.texture->setState(TextureStreamingState::Loaded);
                }
            }

            it = m_ActiveLoads.erase(it);
            m_ActiveLoadCount--;
        }
        else
        {
            ++it;
        }
    }
}

std::shared_ptr<StreamableTexture> TextureStreamingManager::GetStreamableTexture(uint64_t texture_handle) const
{
    return GetTextureInternal(texture_handle);
}

bool TextureStreamingManager::GetTextureResources(uint64_t texture_handle,
                                                  RHIImage*& image,
                                                  RHIImageView*& image_view,
                                                  VmaAllocation& allocation) const
{
    auto texture = GetTextureInternal(texture_handle);
    if (!texture || !texture->isLoaded())
        return false;

    image = texture->getImage();
    image_view = texture->GetImageView();
    allocation = texture->getAllocation();
    return true;
}

TextureStreamingStats TextureStreamingManager::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    return m_Stats;
}

void TextureStreamingManager::SetConfig(const TextureStreamingConfig& config)
{
    m_Config = config;
}

bool TextureStreamingManager::ForceLoadTexture(uint64_t texture_handle, uint32_t mip_level)
{
    std::lock_guard<std::mutex> lock(m_TextureMutex);

    auto it = m_Textures.find(texture_handle);
    if (it == m_Textures.end())
        return false;

    TextureEntry& entry = it->second;
    if (!entry.texture)
        return false;

    // Load texture data
    auto texture_data = LoadTextureData(entry.texture->getPath(), false, mip_level);
    if (!texture_data)
        return false;

    // Create GPU resources
    if (!CreateGPUResources(entry.texture, texture_data, mip_level))
        return false;

    entry.texture->setState(TextureStreamingState::Loaded);
    return true;
}

void TextureStreamingManager::UnloadTexture(uint64_t texture_handle)
{
    std::lock_guard<std::mutex> lock(m_TextureMutex);

    auto it = m_Textures.find(texture_handle);
    if (it == m_Textures.end())
        return;

    TextureEntry& entry = it->second;
    if (entry.texture)
    {
        ReleaseGPUResources(entry.texture);
        entry.texture->setState(TextureStreamingState::Unloaded);
    }
}

bool TextureStreamingManager::IsTextureLoaded(uint64_t texture_handle) const
{
    auto texture = GetTextureInternal(texture_handle);
    return texture && texture->isLoaded();
}

void TextureStreamingManager::ProcessLoadingQueue()
{
    std::unique_lock<std::mutex> loading_lock(m_LoadingMutex);

    // Wait for work or stop signal
    m_LoadingCv.wait(loading_lock, [this] { return !m_LoadingQueue.empty() || m_ShouldStop; });

    if (m_ShouldStop)
        return;

    // Process up to max_concurrent_loads
    while (!m_LoadingQueue.empty() && m_ActiveLoadCount < m_Config.max_concurrent_loads)
    {
        LoadingTask task = m_LoadingQueue.top();
        m_LoadingQueue.pop();

        loading_lock.unlock();

        // Load texture data
        std::lock_guard<std::mutex> texture_lock(m_TextureMutex);
        auto it = m_Textures.find(task.texture_handle);
        if (it != m_Textures.end() && it->second.texture)
        {
            task.texture_data = LoadTextureData(it->second.texture->getPath(), false, task.mip_level);
        }

        loading_lock.lock();
        m_ActiveLoads.push_back(task);
        m_ActiveLoadCount++;
    }
}

std::shared_ptr<TextureData>
TextureStreamingManager::LoadTextureData(const eastl::string& path, bool is_srgb, uint32_t mip_level)
{
    if (!m_RenderResource)
        return nullptr;

    // Load base texture
    auto texture_data = m_RenderResource->LoadTexture(path, is_srgb);
    if (!texture_data)
        return nullptr;

    // TODO: Generate mip levels if needed
    // For now, we just return the base texture
    // In a full implementation, you would generate mip levels here

    return texture_data;
}

bool TextureStreamingManager::CreateGPUResources(std::shared_ptr<StreamableTexture> texture,
                                                 std::shared_ptr<TextureData> texture_data,
                                                 uint32_t mip_level)
{
    if (!m_Rhi || !texture || !texture_data)
        return false;

    // Release old resources if any
    ReleaseGPUResources(texture);

    // Set texture data first to initialize dimensions
    texture->setTextureData(texture_data, mip_level);

    // Check memory budget
    size_t required_memory = texture->GetMemorySizeAtMip(mip_level);
    if (!CheckMemoryBudget(required_memory))
    {
        EvictTextures(required_memory);
    }

    // Create GPU image
    RHIImage* image = nullptr;
    RHIImageView* image_view = nullptr;
    VmaAllocation allocation = nullptr;

    // Use actual texture data dimensions (may be downscaled for mip level)
    uint32_t width = texture_data->m_Width;
    uint32_t height = texture_data->m_Height;

    m_Rhi->CreateGlobalImage(
        image, image_view, allocation, width, height, texture_data->m_Pixels, texture_data->m_Format, mip_level);

    if (image && image_view)
    {
        texture->setGPUResources(image, image_view, allocation);
        return true;
    }

    return false;
}

void TextureStreamingManager::ReleaseGPUResources(std::shared_ptr<StreamableTexture> texture)
{
    if (!texture || !m_Rhi)
        return;

    RHIImage* image = texture->getImage();
    RHIImageView* image_view = texture->GetImageView();
    VmaAllocation allocation = texture->getAllocation();

    if (image)
    {
        // Release resources through RHI
        // Note: This depends on RHI implementation
        // For now, we just clear the references
        texture->setGPUResources(nullptr, nullptr, nullptr);
    }
}

uint32_t TextureStreamingManager::CalculateRequiredMipLevel(std::shared_ptr<StreamableTexture> texture,
                                                            const Vector3& camera_position,
                                                            const Vector3& object_position,
                                                            float fov,
                                                            float screen_size) const
{
    if (!texture)
        return 0;

    // Calculate distance
    Vector3 diff = object_position - camera_position;
    float distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);

    // Use texture's calculation
    return texture->CalculateRequiredMipLevel(distance, screen_size, fov);
}

void TextureStreamingManager::UpdateTexturePriority(TextureEntry& entry,
                                                    const Vector3& camera_position,
                                                    float fov,
                                                    float screen_size)
{
    if (!entry.texture)
        return;

    // Update priority based on visibility and screen size
    if (screen_size > 0.5f)
    {
        entry.current_request.priority = TextureStreamingPriority::Critical;
    }
    else if (screen_size > 0.1f)
    {
        entry.current_request.priority = TextureStreamingPriority::High;
    }
    else if (screen_size > 0.01f)
    {
        entry.current_request.priority = TextureStreamingPriority::Normal;
    }
    else
    {
        entry.current_request.priority = TextureStreamingPriority::Low;
    }
}

bool TextureStreamingManager::CheckMemoryBudget(size_t additional_memory) const
{
    size_t current_usage = GetCurrentMemoryUsage();
    size_t budget_bytes = m_Config.memory_budget_mb * 1024 * 1024;
    return (current_usage + additional_memory) <= budget_bytes;
}

void TextureStreamingManager::EvictTextures(size_t required_memory)
{
    // Simple eviction: unload low priority textures
    std::lock_guard<std::mutex> lock(m_TextureMutex);

    std::vector<TextureHandle> to_evict;

    for (auto& pair : m_Textures)
    {
        if (pair.second.texture && pair.second.texture->isLoaded())
        {
            if (pair.second.current_request.priority == TextureStreamingPriority::Low)
            {
                to_evict.push_back(pair.first);
            }
        }
    }

    // Evict until we have enough memory
    size_t freed_memory = 0;
    for (auto handle : to_evict)
    {
        if (freed_memory >= required_memory)
            break;

        auto it = m_Textures.find(handle);
        if (it != m_Textures.end() && it->second.texture)
        {
            freed_memory += it->second.texture->GetMemorySizeAtMip(it->second.texture->getCurrentMipLevel());
            ReleaseGPUResources(it->second.texture);
            it->second.texture->setState(TextureStreamingState::Unloaded);
        }
    }
}

size_t TextureStreamingManager::GetCurrentMemoryUsage() const
{
    size_t total = 0;
    for (const auto& pair : m_Textures)
    {
        if (pair.second.texture && pair.second.texture->isLoaded())
        {
            total += pair.second.texture->GetMemorySizeAtMip(pair.second.texture->getCurrentMipLevel());
        }
    }
    return total;
}

std::shared_ptr<StreamableTexture> TextureStreamingManager::GetTextureInternal(TextureHandle handle) const
{
    std::lock_guard<std::mutex> lock(m_TextureMutex);
    auto it = m_Textures.find(handle);
    if (it != m_Textures.end() && it->second.is_registered)
    {
        return it->second.texture;
    }
    return nullptr;
}