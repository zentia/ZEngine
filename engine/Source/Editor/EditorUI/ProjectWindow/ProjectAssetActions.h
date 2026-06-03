#pragma once

#include "Editor/EditorUI/ProjectWindow/ProjectWindowContext.h"

#include <filesystem>
#include <string>

struct EditorFileNode;

namespace ProjectAssetActions
{
    void RequestImportDialog(ProjectWindowContext& ctx);
    void ExecutePendingImportDialog(ProjectWindowContext& ctx);

    void OnMenuItemDelete(ProjectWindowContext& ctx, EditorFileNode* node);
    void ExecutePendingDelete(ProjectWindowContext& ctx);

    void RequestPrefabCreate(ProjectWindowContext& ctx, GObjectID source_id, std::filesystem::path target_folder);
    void ExecutePendingPrefabCreate(ProjectWindowContext& ctx);

    void RefreshProjectSelection(ProjectWindowContext& ctx, const std::filesystem::path& asset_path);

    void RequestMaterialFromShader(ProjectWindowContext& ctx, EditorFileNode* node);
    void ExecutePendingMaterialFromShader(ProjectWindowContext& ctx);

    void CreateNewAsset(ProjectWindowContext& ctx, const std::string& asset_type);

    bool CanRenameNode(const ProjectWindowContext& ctx, const EditorFileNode* node);
    bool IsNodeRenaming(const ProjectWindowContext& ctx, const EditorFileNode* node);
    void OnMenuItemRename(ProjectWindowContext& ctx, EditorFileNode* node);
    void CancelRename(ProjectWindowContext& ctx);
    void CommitRename(ProjectWindowContext& ctx);

    void OnMenuItemShowInExplorer(EditorFileNode* node);
    void OnMenuItemCopyPath(EditorFileNode* node);
    void OnMenuItemReimport(ProjectWindowContext& ctx, EditorFileNode* node);
}
