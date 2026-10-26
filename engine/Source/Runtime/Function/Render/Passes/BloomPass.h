#pragma once

// =====================================================================================
// BloomPass -- self-contained, backend-local bloom
// -------------------------------------------------------------------------------------
// Unity-URP-shaped bloom: threshold -> downsample chain -> tent upsample chain ->
// composite. It is deliberately SELF-CONTAINED: the pass allocates its own half-res mip
// chain, its own single-attachment render pass and its own framebuffers, so it can be
// dropped into either RP2 path -- the hand-written chain or the RenderGraph one -- and
// it does not care that Metal / WebGL2 fall back to the hand-written path (the graph
// refuses to run there because their swapchain exposes no RHIImage).
//
// Why not RenderGraph's CreateTexture: that is the tidier home for a mip chain, but it
// only exists behind ZENGINE_RG_RP2=1 and only on Vulkan / DX12, which would leave
// Apple and Web without bloom. Owning the resources costs boilerplate and buys coverage.
//
// Chain layout (N = settings.mip_count, default 6):
//
//   scene (full res, HDR) --prefilter--> D[0] (w/2)
//   D[0] --down--> D[1] --down--> ... --down--> D[N-1]
//   U[N-2] = tent(D[N-1]) + D[N-2]
//   U[N-3] = tent(U[N-2]) + D[N-3]
//   ...
//   U[0]   = tent(U[1])   + D[0]        <-- the finished bloom, at half resolution
//   scene + U[0] * intensity --combine--> m_Output (full res, HDR)
//
// The up chain reads the coarse accumulation AND the same-resolution mip from the down
// chain and writes their sum, instead of blending additively into the larger mip. That
// costs one extra texture set (U) and saves every backend from having to own an additive
// blend state -- and it makes each mip's write a plain "overwrite" with no read-write
// hazard on the same image.
//
// Every mip is fully overwritten each frame, so the render pass can use
// loadOp = DONT_CARE / initialLayout = UNDEFINED and skip any clear. Its finalLayout is
// SHADER_READ_ONLY_OPTIMAL, which is exactly what the next mip's sampler wants -- so the
// whole chain needs ZERO manual barriers.
// =====================================================================================

#include "Runtime/Function/Render/RenderPass.h"

#include <cstdint>
#include <vector>

struct BloomSettings
{
    /// Luminance above which a pixel starts to bloom.
    float threshold {1.0f};
    /// Soft-knee width below `threshold` (Unity's knee). 0 = hard cutoff.
    float knee {0.5f};
    /// How much of the finished bloom is added back. 0 disables the effect but keeps
    /// the pass on the GPU -- use `enabled` to skip the work entirely.
    float intensity {0.25f};
    /// 0 = tight glow, 1 = wide, soft glow (blends the bloom against a 4x-radius gather).
    float scatter {0.6f};
    /// Hard ceiling on the accumulated bloom, so one pathological HDR pixel cannot blow
    /// the whole frame out permanently.
    float clamp_max {16.0f};
    /// Number of half-res steps. 6 is Unity's default; each extra step widens the glow
    /// and costs ~2 more full-screen draws.
    uint32_t mip_count {6};
    bool enabled {true};
};

struct BloomPassInitInfo : RenderPassInitInfo
{
    /// Format of the scene's HDR colour buffer. The mip chain is allocated in the same
    /// format so no conversion happens anywhere in the chain.
    RHIFormat hdr_format;
};

class BloomPass : public RenderPass
{
public:
    BloomPass() = default;
    /// RenderPassBase declares no virtual destructor, and the pipeline drops these
    /// through std::shared_ptr<RenderPassBase>, so the mip chain's images / device
    /// memory / pipelines are released here instead of relying on any base hook.
    /// Shutdown() is null-safe and idempotent, so an explicit call stays harmless.
    ~BloomPass() { Shutdown(); }

    void Initialize(const RenderPassInitInfo* init_info) override final;
    void Draw() override final;

    /// Free everything the pass owns. Call before the RHI goes away.
    void Shutdown();

    /// Reallocate the mip chain for a new render size and re-bind `scene_hdr_input`
    /// (the full-resolution HDR colour the prefilter and composite read). Cheap no-op
    /// when neither the size nor the settings changed.
    void UpdateAfterFramebufferRecreate(uint32_t width, uint32_t height, RHIImageView* scene_hdr_input);

    void SetSettings(const BloomSettings& settings);
    const BloomSettings& GetSettings() const { return m_Settings; }

    /// Flip the pass on/off without reallocating anything. `enabled` is the only field
    /// that is NOT baked into a uniform buffer, so unlike SetSettings this is safe to
    /// call at any point outside a frame -- it only decides whether Draw() records
    /// anything and whether the caller should sample GetOutputImageView().
    void SetEnabled(bool enabled) { m_Settings.enabled = enabled; }

    /// The composited HDR scene (scene + bloom). Feed this to colour grading / tone
    /// mapping instead of the raw scene colour.
    RHIImageView* GetOutputImageView() const { return m_Output.view; }

    bool IsEnabled() const { return m_Settings.enabled; }
    bool IsValid() const { return m_Ready; }

private:
    /// One render target. `framebuffer` is created against the shared single-attachment
    /// render pass, whose size is carried by the framebuffer -- that is what lets one
    /// render pass serve every mip.
    struct MipTarget
    {
        RHIImage* image {nullptr};
        RHIDeviceMemory* memory {nullptr};
        RHIImageView* view {nullptr};
        RHIFramebuffer* framebuffer {nullptr};
        uint32_t width {0};
        uint32_t height {0};
    };

    /// A descriptor set plus the small uniform buffer bound into it. The UBO has to be
    /// per-set because each mip needs its own texel size.
    struct PassBinding
    {
        RHIDescriptorSet* set {nullptr};
        RHIBuffer* ubo {nullptr};
        RHIDeviceMemory* ubo_memory {nullptr};
    };

    void SetupRenderPass();
    void SetupDescriptorSetLayouts();
    void SetupPipelines();

    void RebuildTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    void RebuildBindings(RHIImageView* scene_hdr_input);
    void DestroyBindings();

    PassBinding AllocateBinding(uint32_t layout_index,
                                RHIImageView* texture0,
                                RHIImageView* texture2,
                                float src_width,
                                float src_height,
                                float dst_width,
                                float dst_height);
    void DestroyBinding(PassBinding& binding);

    void DrawStage(int pipeline_index, const PassBinding& binding, const MipTarget& target);

    BloomSettings m_Settings;
    RHIFormat m_HdrFormat;
    RHIImageView* m_SceneInput {nullptr};
    uint32_t m_Width {0};
    uint32_t m_Height {0};
    bool m_Ready {false};

    std::vector<MipTarget> m_Down;
    std::vector<MipTarget> m_Up;
    MipTarget m_Output;

    PassBinding m_PrefilterBinding;
    std::vector<PassBinding> m_DownBindings;
    std::vector<PassBinding> m_UpBindings;
    PassBinding m_CombineBinding;
};
