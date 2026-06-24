#include "Runtime/UI/Render/UIGpuResources.h"

#include "Runtime/UI/Core/Font.h"
#include "Runtime/UI/Render/ZFontAtlas.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Resource/Config/ConfigManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
    #include <stdlib.h>
#endif

UIGpuResources* UIGpuResources::s_Instance = nullptr;

std::vector<std::pair<uint32_t, std::function<void()>>> UIGpuResources::s_InvalidationCallbacks;
uint32_t UIGpuResources::s_NextCallbackId = 1;

uint32_t UIGpuResources::RegisterInvalidationCallback(std::function<void()> callback)
{
    if (!callback)
        return 0;
    const uint32_t id = s_NextCallbackId++;
    s_InvalidationCallbacks.emplace_back(id, std::move(callback));
    return id;
}

void UIGpuResources::UnregisterInvalidationCallback(uint32_t id)
{
    if (id == 0)
        return;
    auto it = std::find_if(s_InvalidationCallbacks.begin(), s_InvalidationCallbacks.end(),
                           [id](const auto& pair) { return pair.first == id; });
    if (it != s_InvalidationCallbacks.end())
        s_InvalidationCallbacks.erase(it);
}

namespace
{
    // Try to add common CJK fallback fonts to `atlas` so that Chinese, Japanese,
    // and Korean codepoints render correctly when the primary font is Latin-only.
    // Mirrors UE's FCompositeFont fallback behaviour for the CJK range.
    void TryAddCjkFallbacks(ZFontAtlas* atlas)
    {
        if (atlas == nullptr)
        {
            return;
        }

#ifdef _WIN32
        // Windows CJK font candidates, in preference order. Only fonts that actually
        // exist on the current machine are added (avoid failing loads for every
        // nonexistent path).
        const char* cjk_candidates[] = {
            "C:/Windows/Fonts/msyh.ttc",    // Microsoft YaHei (Simplified Chinese)
            "C:/Windows/Fonts/simsun.ttc",   // SimSun (Simplified Chinese)
            "C:/Windows/Fonts/simhei.ttf",  // SimHei (Simplified Chinese, bold)
            "C:/Windows/Fonts/Deng.ttf",     // DengXian (Simplified Chinese)
            "C:/Windows/Fonts/meiryo.ttc",  // Meiryo (Japanese)
            "C:/Windows/Fonts/malgun.ttf",  // Malgun Gothic (Korean)
        };
        for (const char* path : cjk_candidates)
        {
            if (!std::filesystem::exists(path))
            {
                continue;
            }
            // Skip if already added (ResolveNativeFontPath caches failures too).
            if (atlas->AddFallbackFont(path))
            {
                LOG_INFO(ZRender, "UIGpuResources: added CJK fallback font '{}'", path);
            }
        }
#endif
    }
}  // namespace

UIGpuResources::~UIGpuResources()
{
    Shutdown();
}

UIGpuResources* UIGpuResources::Get()
{
    return s_Instance;
}

eastl::string UIGpuResources::ResolveDefaultFontPath() const
{
    if (const char* env_path = std::getenv("ZENGINE_UI_FONT"))
    {
        if (env_path[0] != '\0' && std::filesystem::exists(env_path))
        {
            return eastl::string(env_path);
        }
    }

    if (auto config = GET_SYSTEM(ConfigManager))
    {
        const std::filesystem::path& editor_font = config->GetEditorFontPath();
        if (!editor_font.empty() && std::filesystem::exists(editor_font))
        {
            return editor_font.generic_string().c_str();
        }
    }

#ifdef _WIN32
    const char* win_candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/msyh.ttc",
    };
    for (const char* candidate : win_candidates)
    {
        if (std::filesystem::exists(candidate))
        {
            return eastl::string(candidate);
        }
    }
#endif

    return eastl::string();
}

void UIGpuResources::ReuploadTextureInPlace(GpuTexture* target,
                                            const uint8_t* pixels,
                                            uint32_t width,
                                            uint32_t height)
{
    ReuploadTextureInPlace(target, pixels, width, height, RHI_FORMAT_R8G8B8A8_UNORM, 1);
}

void UIGpuResources::ReuploadTextureInPlace(GpuTexture* target,
                                            const uint8_t* pixels,
                                            uint32_t width,
                                            uint32_t height,
                                            RHIFormat format,
                                            uint32_t miplevels)
{
    if (target == nullptr || pixels == nullptr || width == 0 || height == 0)
    {
        return;
    }

    void* fresh = CreateFromPixels(pixels, width, height, format, std::max<uint32_t>(miplevels, 1u));
    if (fresh == nullptr)
    {
        LOG_WARNING(ZRender, "UIGpuResources: texture re-upload failed ({}x{})", width, height);
        return;
    }

    // Transplant the fresh RHI resources into the stable wrapper. handle_id stays
    // bound to the target, so recorded UI commands keep resolving to it via
    // GetDescriptorSet(); any GetXxxTextureId() is likewise unchanged.
    auto* fresh_tex = static_cast<GpuTexture*>(fresh);
    target->image = fresh_tex->image;
    target->view = fresh_tex->view;
    target->sampler = fresh_tex->sampler;
    target->descriptor_set = fresh_tex->descriptor_set;
    if (m_Rhi != nullptr)
    {
        m_Rhi->EnsureShaderVisibleImageView(target->view);
        if (target->descriptor_set != nullptr && target->view != nullptr)
        {
            RHIDescriptorImageInfo image_info {};
            image_info.sampler = target->sampler;
            image_info.imageView = target->view;
            image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            RHIWriteDescriptorSet write {};
            write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = target->descriptor_set;
            write.dstBinding = 0;
            write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &image_info;
            m_Rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
        }
    }
    // Free only the wrapper struct; its RHI handles were transplanted above. The
    // previous image/view/descriptor are intentionally leaked (see header).
    delete fresh_tex;
}

void UIGpuResources::Initialize(RHI* rhi)
{
    if (m_Ready || rhi == nullptr)
    {
        return;
    }

    s_Instance = this;
    m_Rhi = rhi;

    RHIDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = RHI_SHADER_STAGE_FRAGMENT_BIT;

    RHIDescriptorSetLayoutCreateInfo layout_info {};
    layout_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;

    if (RHI_SUCCESS != m_Rhi->CreateDescriptorSetLayout(&layout_info, m_TextureLayout))
    {
        throw std::runtime_error("UIGpuResources: create descriptor set layout failed");
    }

    const uint8_t white_rgba[4] = {255, 255, 255, 255};
    void* white_handle = CreateFromPixels(white_rgba, 1, 1, RHI_FORMAT_R8G8B8A8_UNORM);
    if (white_handle == nullptr)
    {
        throw std::runtime_error("UIGpuResources: create white texture failed");
    }

    m_WhiteTexture.reset(static_cast<GpuTexture*>(white_handle));

    m_Ready = true;

    // Native default font for ZSlate / runtime UI text. Created after m_Ready so
    // ResolveNativeFontPath proceeds past its readiness guard.
    CreateDefaultNativeFont();

    LOG_INFO(ZRender, "UIGpuResources initialized (native font atlas + white texture ready)");
}

void UIGpuResources::Shutdown()
{
    if (!m_Ready)
    {
        return;
    }

    m_Texture2DCache.clear();
    m_DynamicTextures.clear();
    m_ExternalTextures.clear();
    m_NativeFontTextures.clear();
    m_NativeFontsByPath.clear();
    m_DefaultNativeFont = nullptr;
    m_WhiteTexture.reset();

    m_TextureLayout = nullptr;
    m_Rhi = nullptr;
    m_Ready = false;

    if (s_Instance == this)
    {
        s_Instance = nullptr;
    }
}

void UIGpuResources::CreateDefaultNativeFont()
{
    const eastl::string default_path = ResolveDefaultFontPath();
    if (default_path.empty())
    {
        LOG_WARNING(ZRender, "UIGpuResources: no UI font found; text will not render");
        return;
    }
    m_DefaultNativeFont = ResolveNativeFontPath(default_path);

    // Attach CJK fallback fonts so Chinese/Japanese/Korean text renders even
    // when the primary UI font is Latin-only (Segoe UI, Arial, etc.).
    // Matches UE's FCompositeFont behaviour for the CJK Unicode range.
    TryAddCjkFallbacks(m_DefaultNativeFont);
}

ZFontAtlas* UIGpuResources::ResolveNativeFontPath(const eastl::string& path)
{
    if (!m_Ready || path.empty())
    {
        return m_DefaultNativeFont;
    }

    const std::string key(path.c_str(), path.size());
    const auto found = m_NativeFontsByPath.find(key);
    if (found != m_NativeFontsByPath.end())
    {
        return found->second.get();
    }

    auto atlas = std::make_unique<ZFontAtlas>();
    if (!atlas->LoadFromFile(key))
    {
        // Cache the failure as a null entry so we do not retry the load every draw.
        m_NativeFontsByPath.emplace(key, nullptr);
        return m_DefaultNativeFont;
    }

    ZFontAtlas* raw = atlas.get();
    m_NativeFontsByPath.emplace(key, std::move(atlas));
    return raw;
}

ZFontAtlas* UIGpuResources::ResolveNativeFont(Font* font)
{
    if (!m_Ready)
    {
        return nullptr;
    }
    if (font == nullptr || !font->HasSource())
    {
        return m_DefaultNativeFont;
    }
    return ResolveNativeFontPath(font->GetSourcePath());
}

void* UIGpuResources::GetNativeFontTextureId(ZFontAtlas* atlas)
{
    if (!m_Ready || atlas == nullptr || !atlas->IsLoaded())
    {
        return nullptr;
    }

    const auto found = m_NativeFontTextures.find(atlas);
    if (found != m_NativeFontTextures.end())
    {
        return found->second->handle_id;
    }

    void* handle = CreateFromPixels(atlas->GetPixels(),
                                    atlas->GetWidth(),
                                    atlas->GetHeight(),
                                    RHI_FORMAT_R8G8B8A8_UNORM);
    if (handle == nullptr)
    {
        LOG_WARNING(ZRender, "UIGpuResources: native font atlas upload failed");
        return nullptr;
    }

    // Freshly uploaded the current bitmap; clear dirty so the first refresh pass
    // does not redundantly re-upload it.
    atlas->ClearDirty();

    auto gpu_tex = std::unique_ptr<GpuTexture>(static_cast<GpuTexture*>(handle));
    void* id = gpu_tex->handle_id;
    m_NativeFontTextures.emplace(atlas, std::move(gpu_tex));
    return id;
}

void UIGpuResources::RefreshNativeFontAtlasesIfDirty()
{
    if (!m_Ready)
    {
        return;
    }
    for (auto& entry : m_NativeFontTextures)
    {
        ZFontAtlas* atlas = entry.first;
        if (atlas == nullptr || !atlas->IsDirty())
        {
            continue;
        }
        ReuploadTextureInPlace(entry.second.get(), atlas->GetPixels(), atlas->GetWidth(), atlas->GetHeight());
        atlas->ClearDirty();
    }
}

void UIGpuResources::InvalidateAllNativeFontTextures()
{
    if (m_NativeFontTextures.empty())
    {
        return;
    }

    LOG_INFO(ZRender, "UIGpuResources: invalidating {} native font texture(s) for swapchain recovery",
            m_NativeFontTextures.size());

    m_NativeFontTextures.clear();
}

void UIGpuResources::InvalidateAllGpuResources()
{
    if (!m_Ready)
    {
        return;
    }

    size_t total = m_NativeFontTextures.size()
                  + m_Texture2DCache.size()
                  + m_DynamicTextures.size()
                  + m_ExternalTextures.size()
                  + (m_WhiteTexture ? 1 : 0);

    LOG_INFO(ZRender,
            "UIGpuResources: InvalidateAllGpuResources — releasing {} GPU texture "
            "cache entrie(s) (fonts={}, texture2d={}, dynamic={}, external={}, white={})",
            total,
            m_NativeFontTextures.size(),
            m_Texture2DCache.size(),
            m_DynamicTextures.size(),
            m_ExternalTextures.size(),
            (m_WhiteTexture ? 1 : 0));

    // UE parity: FSlateRHIResourceManager::ReleaseResources() releases ALL
    // UI GPU resources (font atlases, brush textures, dynamic textures, etc.)
    // and lets them be lazily re-created on the next draw call.
    //
    // CRITICAL: unlike UE where ReleaseResources is followed by a full
    // UpdateTextureAtlases before the next draw, ZEngine's editor parallel
    // path records the batch on the game thread BEFORE the render thread
    // detects a swapchain resize and calls InvalidateAllGpuResources().
    // If we clear the cache maps, the GpuTexture objects are destroyed and
    // the batch commands' texture_id (GpuTexture* handles) become dangling
    // pointers — fonts disappear for the entire resize frame.
    //
    // Therefore we re-upload every cache entry IN PLACE via
    // ReuploadTextureInPlace: fresh RHI images/views/descriptors are
    // transplanted into the EXISTING GpuTexture wrappers, keeping their
    // handle_id (void* address) stable.  Batch commands recorded before
    // this invalidation continue to resolve to valid descriptor sets.
    //
    // Old RHI objects are intentionally leaked: they may still be
    // referenced by in-flight GPU command buffers; DX12 COM / Vulkan
    // deferred destroy keeps them alive until the GPU finishes.

    // Bump the invalidation version so Editor-side caches (ContentBrowserThumbnailCache,
    // MeshDataPreview, InspectorMaterialPreview) can detect stale handles.
    ++m_InvalidateCount;

    // 1. Native font atlases: re-upload in place from still-valid CPU bitmaps.
    for (auto& entry : m_NativeFontTextures)
    {
        ZFontAtlas* atlas = entry.first;
        if (atlas != nullptr && atlas->IsLoaded())
        {
            ReuploadTextureInPlace(entry.second.get(),
                                   atlas->GetPixels(),
                                   atlas->GetWidth(),
                                   atlas->GetHeight());
        }
    }

    // 2. White texture: re-upload in place.
    if (m_WhiteTexture)
    {
        const uint8_t white_rgba[4] = {255, 255, 255, 255};
        ReuploadTextureInPlace(m_WhiteTexture.get(), white_rgba, 1, 1);
    }

    // 3. Texture2D cache: re-upload in place from the source Texture2D::m_Pixels.
    //    ContentBrowserThumbnailCache and other Editor-side consumers hold void*
    //    handles to these entries; clearing the cache would leave them with
    //    dangling pointers. Re-uploading keeps handle_ids stable.
    for (auto& entry : m_Texture2DCache)
    {
        const Texture2D* tex = entry.first;
        GpuTexture* gpu = entry.second.get();
        if (tex != nullptr && tex->IsValid() && gpu != nullptr && !tex->m_Pixels.empty())
        {
            const RHIFormat format = static_cast<RHIFormat>(tex->m_Format);
            ReuploadTextureInPlace(gpu, tex->m_Pixels.data(), tex->m_Width, tex->m_Height,
                                   format, tex->GetMipCount());
        }
    }

    // 4. Dynamic / external textures: clear (no CPU-pixel backup stored, so
    //    in-place re-upload is not possible). Editor-side caches that hold
    //    these handles must re-create via the invalidation callback below.
    m_DynamicTextures.clear();
    m_ExternalTextures.clear();

    // 5. Fire registered invalidation callbacks so Editor-side thumbnail/preview
    //    caches (ContentBrowserThumbnailCache, MeshDataPreview, InspectorMaterialPreview)
    //    can discard their now-stale void* handles.
    for (const auto& cb : s_InvalidationCallbacks)
    {
        if (cb.second)
            cb.second();
    }
}

void* UIGpuResources::GetWhiteTextureId() const
{
    return m_WhiteTexture != nullptr ? m_WhiteTexture->handle_id : nullptr;
}

void* UIGpuResources::EnsureTexture2D(Texture2D* texture)
{
    if (texture == nullptr || !texture->IsValid() || !m_Ready)
    {
        return GetWhiteTextureId();
    }

    const auto found = m_Texture2DCache.find(texture);
    if (found != m_Texture2DCache.end())
    {
        return found->second->handle_id;
    }

    const RHIFormat format = static_cast<RHIFormat>(texture->m_Format);
    void* handle = CreateFromPixels(texture->m_Pixels.data(),
                                    texture->m_Width,
                                    texture->m_Height,
                                    format,
                                    texture->GetMipCount());
    if (handle == nullptr)
    {
        LOG_WARNING(ZRender, "UIGpuResources: failed to upload Texture2D");
        return GetWhiteTextureId();
    }

    auto gpu_tex = std::unique_ptr<GpuTexture>(static_cast<GpuTexture*>(handle));
    void* id = gpu_tex->handle_id;
    m_Texture2DCache.emplace(texture, std::move(gpu_tex));
    return id;
}

void* UIGpuResources::UpdateDynamicTexture(void* handle, const uint8_t* rgba, uint32_t width, uint32_t height)
{
    if (!m_Ready || rgba == nullptr || width == 0 || height == 0)
    {
        return nullptr;
    }

    if (handle != nullptr)
    {
        const auto found = m_DynamicTextures.find(handle);
        if (found != m_DynamicTextures.end())
        {
            // Re-upload in place; handle_id stays bound to the wrapper so the
            // SImage referencing it picks up the new pixels next frame.
            ReuploadTextureInPlace(found->second.get(), rgba, width, height);
            return handle;
        }
        // Unknown handle -- fall through and create a fresh one.
    }

    void* fresh = CreateFromPixels(rgba, width, height, RHI_FORMAT_R8G8B8A8_UNORM);
    if (fresh == nullptr)
    {
        LOG_WARNING(ZRender, "UIGpuResources: dynamic texture create failed ({}x{})", width, height);
        return nullptr;
    }

    auto gpu_tex = std::unique_ptr<GpuTexture>(static_cast<GpuTexture*>(fresh));
    void* id = gpu_tex->handle_id;
    m_DynamicTextures.emplace(id, std::move(gpu_tex));
    return id;
}

void* UIGpuResources::AdoptExternalImageView(void* handle, RHIImageView* view, RHISampler* sampler)
{
    if (!m_Ready || m_Rhi == nullptr || view == nullptr)
    {
        return nullptr;
    }

    // DX12 overlay: relocate the SRV into the shader-visible heap so it survives
    // the bindless heap bind (no-op on backends that don't need it).
    m_Rhi->EnsureShaderVisibleImageView(view);

    if (sampler == nullptr)
    {
        sampler = m_Rhi->GetOrCreateDefaultSampler(RHIDefaultSamplerType::Default_Sampler_Linear);
    }

    auto write_descriptor = [&](RHIDescriptorSet* set) {
        RHIDescriptorImageInfo image_info {};
        image_info.sampler = sampler;
        image_info.imageView = view;
        image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        RHIWriteDescriptorSet write {};
        write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;
        m_Rhi->UpdateDescriptorSets(1, &write, 0, nullptr);
    };

    if (handle != nullptr)
    {
        const auto found = m_ExternalTextures.find(handle);
        if (found != m_ExternalTextures.end())
        {
            // Re-point the existing descriptor set at the (possibly new) view.
            found->second->view = view;
            found->second->sampler = sampler;
            write_descriptor(found->second->descriptor_set);
            return handle;
        }
        // Unknown handle -- fall through and create a fresh binding.
    }

    auto entry = std::make_unique<GpuTexture>();
    entry->image = nullptr;  // borrowed; never freed here
    entry->view = view;      // borrowed
    entry->sampler = sampler;  // borrowed

    RHIDescriptorSetAllocateInfo alloc_info {};
    alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &m_TextureLayout;
    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&alloc_info, entry->descriptor_set))
    {
        return nullptr;
    }

    write_descriptor(entry->descriptor_set);

    entry->handle_id = entry.get();
    void* id = entry->handle_id;
    m_ExternalTextures.emplace(id, std::move(entry));
    return id;
}

RHIDescriptorSet* UIGpuResources::GetDescriptorSet(void* texture_id) const
{
    if (texture_id == nullptr)
    {
        return m_WhiteTexture != nullptr ? m_WhiteTexture->descriptor_set : nullptr;
    }

    if (m_WhiteTexture != nullptr && texture_id == m_WhiteTexture->handle_id)
    {
        return m_WhiteTexture->descriptor_set;
    }

    for (const auto& entry : m_Texture2DCache)
    {
        if (entry.second->handle_id == texture_id)
        {
            return entry.second->descriptor_set;
        }
    }

    for (const auto& entry : m_NativeFontTextures)
    {
        if (entry.second->handle_id == texture_id)
        {
            return entry.second->descriptor_set;
        }
    }

    const auto dynamic_it = m_DynamicTextures.find(texture_id);
    if (dynamic_it != m_DynamicTextures.end())
    {
        return dynamic_it->second->descriptor_set;
    }

    const auto external_it = m_ExternalTextures.find(texture_id);
    if (external_it != m_ExternalTextures.end())
    {
        return external_it->second->descriptor_set;
    }

    return m_WhiteTexture != nullptr ? m_WhiteTexture->descriptor_set : nullptr;
}

void* UIGpuResources::CreateFromPixels(const uint8_t* pixels,
                                       uint32_t width,
                                       uint32_t height,
                                       RHIFormat format)
{
    return CreateFromPixels(pixels, width, height, format, 1);
}

void* UIGpuResources::CreateFromPixels(const uint8_t* pixels,
                                       uint32_t width,
                                       uint32_t height,
                                       RHIFormat format,
                                       uint32_t miplevels)
{
    if (m_Rhi == nullptr || pixels == nullptr || width == 0 || height == 0)
    {
        return nullptr;
    }

    auto entry = std::make_unique<GpuTexture>();
    entry->sampler = m_Rhi->GetOrCreateDefaultSampler(RHIDefaultSamplerType::Default_Sampler_Linear);
    m_Rhi->CreateGlobalImage(entry->image,
                             entry->view,
                             nullptr,
                             width,
                             height,
                             const_cast<uint8_t*>(pixels),
                             format,
                             std::max<uint32_t>(miplevels, 1u));

    if (entry->image == nullptr || entry->view == nullptr)
    {
        return nullptr;
    }

    m_Rhi->EnsureShaderVisibleImageView(entry->view);

    RHIDescriptorSetAllocateInfo alloc_info {};
    alloc_info.sType = RHI_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = m_Rhi->GetDescriptorPoor();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &m_TextureLayout;

    if (RHI_SUCCESS != m_Rhi->AllocateDescriptorSets(&alloc_info, entry->descriptor_set))
    {
        return nullptr;
    }

    RHIDescriptorImageInfo image_info {};
    image_info.sampler = entry->sampler;
    image_info.imageView = entry->view;
    image_info.imageLayout = RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    RHIWriteDescriptorSet write {};
    write.sType = RHI_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = entry->descriptor_set;
    write.dstBinding = 0;
    write.descriptorType = RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    m_Rhi->UpdateDescriptorSets(1, &write, 0, nullptr);

    entry->handle_id = entry.get();
    return entry.release();
}
