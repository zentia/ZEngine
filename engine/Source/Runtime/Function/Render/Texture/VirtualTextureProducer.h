#pragma once

#include "VirtualTextureTypes.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
/// THREADING -- two threads, one writer per field.
///
/// On the Vulkan parallel-rendering path (the editor's default) the render thread and the
/// RHI thread are different threads, and GPU work may ONLY run on the RHI thread: Vulkan's
/// "current command buffer" and the graphics queue belong to it, and every VT upload is a
/// single-time submit that does vkQueueSubmit + vkQueueWaitIdle (RenderSystemFrameCommands.cpp
/// documents the crash and the double-recording that putting GPU work on the render thread
/// causes). So the work is split by what it touches:
///
///   TickCpu  (RENDER thread): read CVars/camera, load the source image (disk IO + decode),
///            RegisterVirtualTexture, RequestPages. CPU only. Owns m_Handle / m_ActivePath /
///            m_FailedPath. Never calls anything that creates or destroys a GPU object.
///   TickGpu  (RHI thread):    bring the manager (and pool) up, release retired textures,
///            then PumpLoads + Update (page upload, page-table flush). Owns the pool.
///
/// The only things that cross are (a) m_Retired: handles the render thread no longer wants,
/// destroyed on the RHI thread (a texture's page table is a GPU image the RHI thread may be
/// using, so the render thread must not free it), and (b) m_WantPool. Both are guarded. Tick()
/// runs TickCpu then TickGpu back to back for single-threaded callers (and the tests).
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
        // Sampling contract v2: a 1-texel gutter around every page (bilinear across page edges) and a resident
        // mip chain (the shader picks the mip from screen-space derivatives). Set page_border = 0 and
        // resident_mip_chain = false for the original mip-0-only point-sampled layout.
        uint32_t page_border = 1;
        bool resident_mip_chain = true;
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

    /// RENDER thread half of Tick: CPU only (see the threading note above).
    bool TickCpu(bool enabled,
                 const std::string& source_path,
                 const Vector3& camera_position,
                 const Vector3& camera_forward);

    /// RHI thread half of Tick: every GPU operation. `camera_*`/`fov`/`aspect`/screen size
    /// feed VirtualTextureManager::Update's priority pass; they are plain values the render
    /// thread captured, so this never reads render-thread-owned objects.
    void TickGpu(RHI* rhi,
                 RenderResourceBase* render_resource,
                 const Vector3& camera_position,
                 const Vector3& camera_forward,
                 float fov_radians,
                 float aspect,
                 uint32_t screen_width,
                 uint32_t screen_height,
                 float delta_time);

    /// Release the texture and, if this producer brought the manager up, the manager.
    /// Must be called while the RHI is still alive. Safe to call repeatedly.
    void Shutdown();

    // ---- observability (tests, and the periodic log line) ----
    uint64_t getHandle() const { return m_Handle; }
    /// RHI thread: handle of the texture a frame-time consumer (the DX12 overlay) may sample right now,
    /// or 0. Unlike getHandle() this is safe to call from the RHI thread -- m_Handle belongs to the
    /// render thread. It is cleared (under the same lock that queues the handle for destruction) BEFORE
    /// the handle is retired, and the retired handle is only ever unregistered on the RHI thread, so a
    /// consumer that reads a non-zero value on the RHI thread can use it for the rest of that frame.
    uint64_t getGpuVisibleHandle() const;
    bool isStreaming() const { return m_Handle != 0; }
    const std::string& getActivePath() const { return m_ActivePath; }
    uint32_t getFailureCount() const { return m_FailureCount; }
    /// Pages of the active texture that are resident / total (mip 0).
    void GetResidency(uint32_t& resident, uint32_t& total) const;

private:
    void ReleaseTexture();
    // RENDER thread, once, BEFORE the first RegisterVirtualTexture: push this producer's pool / gutter / mip-chain
    // settings into the manager. They must already be in place when the resource is registered (the resource bakes
    // the gutter and builds the mip chain at registration), which happens on the render thread long before the RHI
    // thread brings the pool up -- applying them in EnsureManager (RHI thread) was too late.
    void ApplyManagerConfig();
    bool m_ManagerConfigApplied {false};
    // RHI thread. Builds the pool on first need; reports failure back through m_PoolFailed.
    bool EnsureManager(RHI* rhi, RenderResourceBase* render_resource);
    // RENDER thread: hand a handle to the RHI thread instead of unregistering it here.
    void RetireHandle(uint64_t handle);

    // ---- the only state that crosses threads ----
    mutable std::mutex m_HandoffMutex;
    std::vector<uint64_t> m_Retired;    // handles for the RHI thread to unregister (guarded)
    bool m_WantPool {false};            // the render thread has a texture that needs the pool (guarded)
    bool m_PoolFailed {false};          // the RHI thread could not build the pool (guarded)
    uint64_t m_GpuVisibleHandle {0};    // handle consumers on the RHI thread may sample (guarded)

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
