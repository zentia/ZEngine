#pragma once

#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/UI/Render/ZFontAtlas.h"

#include <EASTL/string.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Font;
class Texture2D;

// GPU-side texture cache for the runtime UI (font atlas + Texture2D uploads).
// Initialized by UIPass once RHI is available; read during UISystem::PreRender().
class UIGpuResources
{
public:
    static UIGpuResources* Get();

    void Initialize(RHI* rhi);
    void Shutdown();
    bool IsReady() const { return m_Ready; }

    void* GetWhiteTextureId() const;

    // Native font path used by BatchedUIRenderer. Each source path maps to one
    // ZFontAtlas (its own CPU bitmap + GPU texture). Returns the default native
    // atlas when the font has no source or fails to load (null only if even the
    // default failed).
    ZFontAtlas* ResolveNativeFont(Font* font);
    ZFontAtlas* ResolveNativeFontPath(const eastl::string& path);
    ZFontAtlas* GetDefaultNativeFont() const { return m_DefaultNativeFont; }

    // Ensures a GPU texture exists for the given native atlas and returns its
    // stable handle_id (usable as a draw texture_id). Bakes from the atlas' current
    // bitmap on first call; later glyphs land via RefreshNativeFontAtlasesIfDirty.
    void* GetNativeFontTextureId(ZFontAtlas* atlas);

    // Re-uploads every native atlas whose bitmap grew this frame. Call once per
    // frame AFTER all UI has recorded (so newly requested glyphs are baked) and
    // BEFORE the batch is drawn. handle_ids stay stable across the re-upload.
    void RefreshNativeFontAtlasesIfDirty();

    // Force-invalidate ALL native font atlas GPU textures so they are re-created
    // from CPU bitmap data on the next GetNativeFontTextureId() call.
    // Must be called after a DX12 swapchain recreate / device-loss recovery:
    // the old SRV descriptor handles may be stale after DXGI surface
    // invalidation (Alt-Tab, minimize/maximize, driver TDR), and re-uploading
    // from the still-valid CPU pixels is the only safe recovery path.
    // Mirrors UE's FSlateRHIRenderer::Invalidated() behaviour for font resources.
    void InvalidateAllNativeFontTextures();

    // UE-parity: release ALL UI GPU resources (font atlases, white texture,
    // Texture2D cache, dynamic textures, external image views) so they are
    // re-created on the next draw call.  This is the ZEngine equivalent of
    // UE's FSlateRHIResourceManager::ReleaseResources().
    //
    // Implementation note: we intentionally do NOT free the RHI objects
    // (RHIImage / RHIImageView / RHIDescriptorSet) immediately — they may
    // still be referenced by in-flight GPU command buffers.  DX12's COM
    // reference counting keeps them alive until the GPU finishes; thereafter
    // they are reclaimed automatically.  The CPU-side ZFontAtlas pixel
    // buffers are never freed (they live in m_NativeFontsByPath), so the
    // next GetNativeFontTextureId() / EnsureTexture2D() call will upload
    // fresh GPU resources from the still-valid bitmap data.
    void InvalidateAllGpuResources();

    // Register a callback that fires AFTER all GPU resource caches are invalidated.
    // Use this to clear Editor-side thumbnail/preview caches that hold void*
    // handle references to now-destroyed GpuTexture wrappers. Thread-safe for
    // registration but NOT for concurrent firing (call on the main thread only).
    // Returns a registration id for UnregisterInvalidationCallback.
    static uint32_t RegisterInvalidationCallback(std::function<void()> callback);
    static void UnregisterInvalidationCallback(uint32_t id);

    // Opaque version that bumps every time m_Texture2DCache / m_DynamicTextures /
    // m_ExternalTextures are invalidated. Editor caches can store this alongside
    // their cached handles and discard on mismatch.
    uint64_t GetInvalidateCount() const { return m_InvalidateCount; }

    void* EnsureTexture2D(Texture2D* texture);

    // Dynamic CPU-bitmap texture for editor previews (software-rasterized mesh /
    // material previews). Pass `handle == nullptr` to create a new texture from
    // `rgba` (RGBA8, tightly packed, width*height*4 bytes); pass a handle returned
    // by an earlier call to re-upload in place (the handle stays stable, so an
    // SImage bound to it shows the new pixels without a rebuild). The image is
    // recreated when the size changes. Returns the stable handle (or nullptr on
    // failure / when not ready). Resources are freed on Shutdown.
    void* UpdateDynamicTexture(void* handle, const uint8_t* rgba, uint32_t width, uint32_t height);

    // Adopts an externally-owned RHI image view (e.g. a render-to-texture color
    // target produced by a backend-specific preview renderer) as a UI-samplable
    // texture. ONLY a descriptor set in the UI texture layout is allocated here;
    // the RHIImage/RHIImageView/RHISampler lifetime stays with the caller (they
    // are NOT freed on Shutdown). Pass `handle == nullptr` to create a fresh
    // binding; pass a handle returned by an earlier call to re-point the existing
    // descriptor set at `view` (used when the caller recreated the underlying
    // view, e.g. the RTT was resized). When `sampler` is null a default linear
    // sampler is used. Returns the stable handle (or nullptr on failure / when
    // not ready). The view must already be (or will be made) shader-visible.
    void* AdoptExternalImageView(void* handle, RHIImageView* view, RHISampler* sampler = nullptr);

    RHIDescriptorSet* GetDescriptorSet(void* texture_id) const;
    RHIDescriptorSetLayout* GetTextureLayout() const { return m_TextureLayout; }

    ~UIGpuResources();

private:
    struct GpuTexture
    {
        void* handle_id {nullptr};
        RHIImage* image {nullptr};
        RHIImageView* view {nullptr};
        RHISampler* sampler {nullptr};
        RHIDescriptorSet* descriptor_set {nullptr};
    };

    eastl::string ResolveDefaultFontPath() const;
    // Transplants freshly-created RHI image/view/descriptor into an existing
    // GpuTexture wrapper, keeping its handle_id stable. Old GPU resources are
    // intentionally leaked (they may still be referenced by in-flight command
    // buffers, so freeing them here would be a use-after-free without a frame-
    // fence wait). Used by the native atlas refresh path.
    void ReuploadTextureInPlace(GpuTexture* target, const uint8_t* pixels, uint32_t width, uint32_t height);
    // Overload supporting arbitrary RHI formats and mip chains (for Texture2D assets
    // with compressed / mipped data). Used by InvalidateAllGpuResources to re-upload
    // Texture2D cache entries in place.
    void ReuploadTextureInPlace(GpuTexture* target, const uint8_t* pixels, uint32_t width, uint32_t height,
                                RHIFormat format, uint32_t miplevels);
    void CreateDefaultNativeFont();
    void* CreateFromPixels(const uint8_t* pixels, uint32_t width, uint32_t height, RHIFormat format);
    // Mip-aware overload: `pixels` is a tightly-packed mip chain (mip0 first),
    // `miplevels` its count. Used by EnsureTexture2D for cooked compressed+mipped
    // Texture2D assets. The 4-arg overload above forwards here with miplevels=1.
    void* CreateFromPixels(const uint8_t* pixels, uint32_t width, uint32_t height, RHIFormat format, uint32_t miplevels);

    static UIGpuResources* s_Instance;

    // Invalidation callback registry (see RegisterInvalidationCallback).
    static std::vector<std::pair<uint32_t, std::function<void()>>> s_InvalidationCallbacks;
    static uint32_t s_NextCallbackId;

    RHI* m_Rhi;
    RHIDescriptorSetLayout* m_TextureLayout {nullptr};

    std::unique_ptr<GpuTexture> m_WhiteTexture;

    std::unordered_map<const Texture2D*, std::unique_ptr<GpuTexture>> m_Texture2DCache;

    // Editor preview dynamic textures, keyed by their stable handle_id.
    std::unordered_map<void*, std::unique_ptr<GpuTexture>> m_DynamicTextures;

    // Externally-owned image views adopted as UI textures (RTT shader/material
    // previews). The GpuTexture wrapper owns ONLY its descriptor set; image/view/
    // sampler are borrowed and must not be destroyed here.
    std::unordered_map<void*, std::unique_ptr<GpuTexture>> m_ExternalTextures;

    std::unordered_map<std::string, std::unique_ptr<ZFontAtlas>> m_NativeFontsByPath;
    std::unordered_map<ZFontAtlas*, std::unique_ptr<GpuTexture>> m_NativeFontTextures;
    ZFontAtlas* m_DefaultNativeFont {nullptr};

    uint64_t m_InvalidateCount {0};
    bool m_Ready {false};
};
