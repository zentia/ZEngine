#include "Editor/EditorUI/ContentBrowser/ContentBrowserDragDrop.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserHelpers.h"
#include "Editor/Menu/AssetsMenu.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "core/Log/LogSystem.h"

#include <filesystem>
#include <mutex>

namespace ContentBrowserDragDrop
{
    void OnOsFilesDropped(ContentBrowserContext& ctx, const std::vector<std::string>& paths)
    {
        if (paths.empty())
        {
            return;
        }
        std::lock_guard<std::mutex> lock(ctx.os_drop_mutex);
        ctx.pending_os_drop_imports.insert(ctx.pending_os_drop_imports.end(), paths.begin(), paths.end());
    }

    void ExecutePendingOsDropImports(ContentBrowserContext& ctx)
    {
        std::vector<std::string> drops;
        {
            std::lock_guard<std::mutex> lock(ctx.os_drop_mutex);
            if (ctx.pending_os_drop_imports.empty())
            {
                return;
            }
            drops.swap(ctx.pending_os_drop_imports);
        }

        const std::filesystem::path target_folder = ContentBrowserHelpers::ResolveDropTargetFolder(ctx.selected_node);

        auto editor_asset_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
        AssetImportManager* import_manager =
            editor_asset_mgr != nullptr ? &editor_asset_mgr->getImportManager() : nullptr;

        int imported = 0;
        int skipped = 0;
        for (const std::string& src : drops)
        {
            std::filesystem::path src_path(src);

            std::error_code ec;
            if (!std::filesystem::exists(src_path, ec) || ec)
            {
                LOG_WARNING(ZAsset, "Drop-import: source does not exist: {}", src);
                ++skipped;
                continue;
            }

            if (std::filesystem::is_directory(src_path, ec))
            {
                LOG_INFO(ZAsset,
                         "Drop-import: directory drops are not yet supported, "
                         "skipped: {}",
                         src_path.string());
                ++skipped;
                continue;
            }

            if (import_manager == nullptr || import_manager->FindImporter(src_path) == nullptr)
            {
                LOG_WARNING(ZAsset,
                            "Drop-import: no importer registered for '{}' (extension '{}'), skipped",
                            src_path.string(),
                            src_path.extension().string());
                ++skipped;
                continue;
            }

            AssetsMenu::ConvertAsset(eastl::string(src_path.string().c_str()),
                                     eastl::string(target_folder.string().c_str()));
            ++imported;
        }

        LOG_INFO(ZAsset,
                 "Drop-import: {} imported, {} skipped (target: {})",
                 imported,
                 skipped,
                 target_folder.string());

        if (imported > 0)
        {
            ctx.asset_tree_dirty = true;
        }
    }
}
