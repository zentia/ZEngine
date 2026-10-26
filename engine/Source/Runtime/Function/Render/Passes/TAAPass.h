#pragma once

// =====================================================================================
// TAAPass -- depth-reprojection temporal anti-aliasing
// -------------------------------------------------------------------------------------
// The engine has no motion-vector / velocity attachment, so instead of reading a
// per-pixel velocity this pass reconstructs one:
//
//   depth + inverse(current view-projection)  ->  world position
//   world position + previous view-projection ->  previous clip position -> previous uv
//
// That is exact for opaque geometry (including moving objects) and only degrades where
// depth carries no information -- transparents and sky written at the far plane -- which
// is exactly the set of pixels that ghosts in every velocity-buffer-free TAA.
//
// It sits BETWEEN RP1 and bloom: it consumes RP1's jittered HDR colour and hands the
// resolved frame on as if it were the scene colour, so neither bloom nor tone mapping
// knows anything changed. Resolving after tone mapping would make the variance clip
// fight the tone curve and produce crawling.
//
// Why it owns its history instead of using the RenderGraph: same reason as BloomPass --
// the graph is opt-in (ZENGINE_RG_RP2=1) and Vulkan/DX12 only, and a resolve history is
// a persistent resource the graph would have to be told about every frame.
//
// Two constraints shaped the implementation and are worth knowing before editing it:
//
//   1. RHI has NO MapMemory and NO barrier API. So the per-frame uniform block cannot be
//      written in place; it is re-created each frame into a small ring (see kUboRingSize)
//      and the descriptor set is re-pointed at it. Likewise there is no way to transition
//      the scene depth image -- which is why SetupRenderPass1's depth attachment has to
//      end in DEPTH_STENCIL_READ_ONLY_OPTIMAL and the depth image has to be created with
//      SAMPLED_BIT. Both are done by MainCameraPass / VulkanRHI, not here.
//
//   2. History is ping-ponged between two targets rather than "resolve + copy": the
//      target written this frame IS next frame's history, so there is no copy pass and no
//      read-write hazard on a single image.
// =====================================================================================

#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>

struct TAASettings
{
    /// How much of the CURRENT frame wins. ~0.05-0.15; lower = smoother but ghosier.
    float blend {0.10f};
    /// Width of the variance-clip box in standard deviations. Higher clips less.
    float variance_gamma {1.0f};
    /// 0 = standard [0,1] depth, 1 = reversed-Z (1 at the near plane).
    bool reversed_z {false};
    bool enabled {true};
};

struct TAAPassInitInfo : RenderPassInitInfo
{
    /// Format of the scene's HDR colour buffer; the history chain is allocated in it.
    RHIFormat hdr_format;
};

class TAAPass : public RenderPass
{
public:
    TAAPass() = default;
    /// RenderPassBase declares no virtual destructor and the pipeline drops these
    /// through std::shared_ptr<RenderPassBase>, so the history chain is released here.
    /// Shutdown() is null-safe and idempotent.
    ~TAAPass() { Shutdown(); }

    void Initialize(const RenderPassInitInfo* init_info) override final;
    void Draw() override final;

    void Shutdown();

    /// Reallocate the history chain for a new render size and re-bind the scene colour
    /// and depth this frame's resolve reads.
    void UpdateAfterFramebufferRecreate(uint32_t width,
                                        uint32_t height,
                                        RHIImageView* scene_hdr_input,
                                        RHIImageView* depth_input);

    /// Push this frame's view-projection. The pass keeps the previous one itself, so no
    /// engine-side "previous matrix" plumbing is needed. `reset` clears the history
    /// (used on resize / teleport / first frame) so a stale image cannot be blended in.
    void UpdatePerFrame(const Matrix4x4& current_view_projection, bool reset);

    void SetSettings(const TAASettings& settings) { m_Settings = settings; }
    const TAASettings& GetSettings() const { return m_Settings; }
    void SetEnabled(bool enabled) { m_Settings.enabled = enabled; }

    /// The resolved frame. Feed this to bloom / tone mapping instead of the raw scene.
    RHIImageView* GetOutputImageView() const;

    bool IsEnabled() const { return m_Settings.enabled; }
    bool IsValid() const { return m_Ready; }

private:
    struct HistoryTarget
    {
        RHIImage* image {nullptr};
        RHIDeviceMemory* memory {nullptr};
        RHIImageView* view {nullptr};
        RHIFramebuffer* framebuffer {nullptr};
    };

    struct FrameUbo
    {
        RHIBuffer* buffer {nullptr};
        RHIDeviceMemory* memory {nullptr};
    };

    void SetupRenderPass();
    void SetupDescriptorSetLayout();
    void SetupPipeline();

    void RebuildTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    void RebuildDescriptor(RHIImageView* scene_hdr_input, RHIImageView* depth_input);
    void DestroyDescriptor();

    /// Advance the UBO ring and fill it with this frame's matrices.
    void UploadUniforms();

    TAASettings m_Settings;
    RHIFormat m_HdrFormat {RHI_FORMAT_R16G16B16A16_SFLOAT};
    RHIImageView* m_SceneInput {nullptr};
    RHIImageView* m_DepthInput {nullptr};

    uint32_t m_Width {0};
    uint32_t m_Height {0};
    bool m_Ready {false};

    /// Stable resolve target. Bloom / tone mapping bind this once and never re-bind.
    HistoryTarget m_Output;
    /// History ping-pongs because a pass may not sample and write the same image. Only
    /// this pass reads it, so the alternation is invisible to every consumer.
    HistoryTarget m_History[2];
    uint32_t m_HistoryWriteIndex {0};
    /// One framebuffer per history slot: the render pass writes the resolve AND the
    /// history, so the second attachment changes with the ping-pong.
    RHIFramebuffer* m_ResolveFramebuffers[2] {nullptr, nullptr};

    RHIDescriptorSet* m_DescriptorSet {nullptr};
    /// The RHI cannot map uniform memory, so the block is re-created per frame. The ring
    /// is slightly larger than the number of frames in flight so the buffer being
    /// replaced is never one the GPU is still reading.
    static constexpr uint32_t kUboRingSize {3};
    FrameUbo m_UboRing[kUboRingSize];
    uint32_t m_UboCursor {0};
    bool m_UboRingReady {false};

    Matrix4x4 m_CurrentViewProjection {Matrix4x4::IDENTITY};
    Matrix4x4 m_PreviousViewProjection {Matrix4x4::IDENTITY};
    bool m_HasPrevious {false};
    bool m_ResetPending {true};
};
