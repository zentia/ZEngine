#include "Editor/EditorUI/ContentBrowser/ContentBrowserThumbnailCache.h"

#include "Editor/EditorUI/ContentBrowser/ContentBrowserHelpers.h"
#include "Editor/EditorWindow/InspectorWindow/InspectorAssetCommon.h"
#include "Editor/EditorWindow/InspectorWindow/InspectorMaterialPreview.h"
#include "Editor/EditorWindow/PreviewWindow/MeshDataPreview.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/UI/Render/UiGpuResources.h"

#include <filesystem>
#include <string>
#include <unordered_map>

namespace
{
    struct CacheEntry
    {
        void* handle {nullptr};
        std::filesystem::file_time_type write_time {};
    };

    std::unordered_map<std::string, CacheEntry>& cacheMap()
    {
        static std::unordered_map<std::string, CacheEntry> s_cache;
        return s_cache;
    }

    std::string cacheKey(const std::filesystem::path& path)
    {
        return ContentBrowserHelpers::NormalizeContentBrowserPath(path).generic_string();
    }

    std::filesystem::file_time_type fileWriteTime(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::last_write_time(path, ec);
    }
}  // namespace

namespace ContentBrowserThumbnailCache
{
    void InvalidateAll()
    {
        cacheMap().clear();
        MeshDataPreview::InvalidateAll();
        InvalidateAllMaterialPreviews();
    }

    void InvalidatePath(const std::filesystem::path& path)
    {
        cacheMap().erase(cacheKey(path));
        MeshDataPreview::InvalidatePreview(path);
        InvalidateMaterialPreview(path);
    }

    bool Tick(int max_mesh_thumbnails_per_frame)
    {
        return MeshDataPreview::TickPendingThumbnails(max_mesh_thumbnails_per_frame);
    }

    void* ResolveForNode(const EditorFileNode* node)
    {
        if (node == nullptr || ContentBrowserHelpers::IsFolderNode(node) || node->m_FilePath.empty())
            return nullptr;

        if (!ZSlate::ZSlateEditorOverlay::Get().IsNativeBackendEnabled())
            return nullptr;

        UiGpuResources* gpu = UiGpuResources::Get();
        if (gpu == nullptr || !gpu->IsReady())
            return nullptr;

        const std::filesystem::path asset_path(node->m_FilePath.c_str());
        const std::string key = cacheKey(asset_path);
        const std::filesystem::file_time_type mtime = fileWriteTime(asset_path);

        CacheEntry& entry = cacheMap()[key];
        if (entry.handle != nullptr && entry.write_time == mtime)
            return entry.handle;

        const std::string asset_type =
            ResolveInspectorAssetType(asset_path, node->displayTypeLabel());

        if (IsTexture2DInspectorAssetType(asset_type))
        {
            std::filesystem::path read_path = asset_path;
            Texture2D* texture = GET_SYSTEM(AssetManager)->ReadObject<Texture2D>(read_path);
            if (texture == nullptr || !texture->IsValid())
            {
                entry = {};
                return nullptr;
            }

            void* handle = gpu->EnsureTexture2D(texture);
            entry.handle = handle;
            entry.write_time = mtime;
            return handle;
        }

        if (MeshDataPreview::IsSupportedAssetType(asset_type))
        {
            constexpr uint32_t k_mesh_thumb_pixels = 128;
            if (void* handle = MeshDataPreview::TryGetThumbnailHandle(asset_path, k_mesh_thumb_pixels))
            {
                entry.handle = handle;
                entry.write_time = mtime;
                return handle;
            }

            MeshDataPreview::RequestThumbnail(asset_path, k_mesh_thumb_pixels);
            return nullptr;
        }

        if (IsMaterialInspectorAssetType(asset_type))
        {
            constexpr uint32_t k_material_thumb_pixels = 128;
            const MaterialPreviewResult frame =
                RenderMaterialThumbnailFromPath(asset_path, k_material_thumb_pixels);
            if (!frame.ok || frame.texture_handle == nullptr)
            {
                entry = {};
                return nullptr;
            }

            entry.handle = frame.texture_handle;
            entry.write_time = mtime;
            return frame.texture_handle;
        }

        entry = {};
        return nullptr;
    }
}  // namespace ContentBrowserThumbnailCache
