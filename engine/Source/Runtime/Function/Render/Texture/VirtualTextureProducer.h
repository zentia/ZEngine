#pragma once

#include "VirtualTextureTypes.h"

#include <cstdint>
#include <memory>
#include <string>

class RHI;
class RenderResourceBase;
class TextureData;
class VirtualTextureManager;
class Vector3;

/// <summary>
/// The minimal virtual-texture PRODUCER: the piece that turns "r.VirtualTexture.Enable 1"
/// from a no-op into actual streaming.
///
/// Before this, the engine had nothing that initialised the VirtualTextureManager
/// singleton, registered a virtual texture, or requested pages -- the Enable CVar
/// advanced a manager that had no pool and no textures. This class owns that missing
/// lifecycle for ONE texture:
///
///   * lazily brings the singleton up (sized to the device -- the default config asks
///     for a 256 GiB pool image), and tears it down while the RHI is still alive;
///   * loads the source image, registers it, and keeps its page requests alive;
///   * is driven once per frame by the render system, BEFORE VirtualTextureManager::Update
///     (so a page requested this frame is committed this frame).
///
/// It deliberately draws nothing: there is no material that samples a virtual texture
/// yet. "Streaming works" is observable through residency (GetStats / the periodic log
/// line / the test), not through pixels. Wiring a real material is a separate step.
///
/// Threading: every method runs on the render-system owner thread (the one that calls
/// Update), matching VirtualTextureManager's owner-thread contract.
/// </summary>
class VirtualTextureProducer
{
public:
    /// What the producer needs from its environment. A function pointer instead of a
    /// hard dependency on RenderResourceBase so the test can feed synthetic pixels
    /// without a project / AssetManager.
    using TextureLoader = std::shared_ptr<TextureData> (*)(void* user, const std::string& path);

    struct Config
    {
        // Pool geometry. The producer clamps it to what the device can allocate.
        uint32_t page_size = 128;
        uint32_t pool_pages_per_side = 16;  // 16*128 = 2048 px; 16 MiB RGBA8
        uint32_t max_concurrent_loads = 8;
        // Distance at which the texture is anchored in front of the camera.
        float anchor_distance = 10.0f;
        // World half-extent of the anchored texture's footprint (drives screen size).
        float anchor_half_extent = 2.0f;
        // Frames between "streaming settled" log lines; 0 = never log.
        uint32_t log_every_frames = 300;
    };

    VirtualTextureProducer() = default;
    ~VirtualTextureProducer();

    VirtualTextureProducer(const VirtualTextureProducer&) = delete;
    VirtualTextureProducer& operator=(const VirtualTextureProducer&) = delete;

    /// `manager` is normally VirtualTextureManager::Get(); the test passes its own so
    /// it never touches the process-wide singleton. `loader`/`loader_user` produce the
    /// source pixels for a path.
    void Bind(VirtualTextureManager* manager, TextureLoader loader, void* loader_user, const Config& config);

    /// One frame. `enabled` mirrors r.VirtualTexture.Enable, `source_path`
    /// r.VirtualTexture.Source. Returns true when streaming work happened.
    ///
    /// State machine (all transitions are idempotent and safe to repeat every frame):
    ///   disabled / empty path  -> nothing registered; if something was, it is released
    ///   path changed           -> old texture unregistered, new one registered
    ///   path unchanged         -> RequestPages (idempotent) to keep the pages wanted
    ///   load/initialise failed -> logged ONCE for that path, then not retried every frame
    bool Tick(RHI* rhi,
              RenderResourceBase* render_resource,
              bool enabled,
              const std::string& source_path,
              const Vector3& camera_position,
              const Vector3& camera_forward);

    /// Release the texture and, if this producer brought the manager up, the manager.
    /// Must be called while the RHI is still alive. Safe to call repeatedly.
    void Shutdown();

    // ---- observability (tests, and the periodic log line) ----
    uint64_t getHandle() const { return m_Handle; }
    bool isStreaming() const { return m_Handle != 0; }
    const std::string& getActivePath() const { return m_ActivePath; }
    uint32_t getFailureCount() const { return m_FailureCount; }
    /// Pages of the active texture that are resident / total (mip 0).
    void GetResidency(uint32_t& resident, uint32_t& total) const;

private:
    void ReleaseTexture();
    bool EnsureManager(RHI* rhi, RenderResourceBase* render_resource);

    VirtualTextureManager* m_Manager {nullptr};
    TextureLoader m_Loader {nullptr};
    void* m_LoaderUser {nullptr};
    Config m_Config {};

    bool m_BroughtUpManager {false};
    uint64_t m_Handle {0};
    std::string m_ActivePath;
    std::string m_FailedPath;  // 失败过的路径：同一路径不再每帧重试（路径一变就重新尝试）
    uint32_t m_FailureCount {0};
    uint32_t m_FrameCounter {0};
    bool m_Settled {false};
};
