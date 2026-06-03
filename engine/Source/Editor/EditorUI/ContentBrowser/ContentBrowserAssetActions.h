#pragma once

#include "Editor/EditorUI/ContentBrowser/ContentBrowserContext.h"

#include <filesystem>
#include <string>

struct EditorFileNode;

namespace ContentBrowserAssetActions
{
    void RequestImportDialog(ContentBrowserContext& ctx);
    void ExecutePendingImportDialog(ContentBrowserContext& ctx);

    void OnMenuItemDelete(ContentBrowserContext& ctx, EditorFileNode* node);
    void ExecutePendingDelete(ContentBrowserContext& ctx);

    void RequestPrefabCreate(ContentBrowserContext& ctx, GObjectID source_id, std::filesystem::path target_folder);
    void ExecutePendingPrefabCreate(ContentBrowserContext& ctx);

    void RefreshContentBrowserSelection(ContentBrowserContext& ctx, const std::filesystem::path& asset_path);

    void RequestMaterialFromShader(ContentBrowserContext& ctx, EditorFileNode* node);
    void ExecutePendingMaterialFromShader(ContentBrowserContext& ctx);

    void CreateNewAsset(ContentBrowserContext& ctx, const std::string& asset_type);

    bool CanRenameNode(const ContentBrowserContext& ctx, const EditorFileNode* node);
    bool IsNodeRenaming(const ContentBrowserContext& ctx, const EditorFileNode* node);
    void OnMenuItemRename(ContentBrowserContext& ctx, EditorFileNode* node);
    void CancelRename(ContentBrowserContext& ctx);
    void CommitRename(ContentBrowserContext& ctx);

    void OnMenuItemShowInExplorer(EditorFileNode* node);
    void OnMenuItemCopyPath(EditorFileNode* node);
    void OnMenuItemReimport(ContentBrowserContext& ctx, EditorFileNode* node);
}
