#pragma once

#include "Editor/EditorFileService/EditorFileService.h"
#include "function/framework/Object/ObjectIdAllocator.h"

#include <EASTL/string.h>

#include <filesystem>
#include <string>
#include <vector>

class GameObject;
struct EditorFileNode;

namespace ContentBrowserHelpers
{
    bool IsFolderNode(const EditorFileNode* node);
    eastl::string GetContentBrowserDisplayName(const EditorFileNode* node);

    std::filesystem::path GetEditorSourceAssetFolder();
    std::filesystem::path NormalizeContentBrowserPath(const std::filesystem::path& path);

    EditorFileNode* FindContentBrowserNodeByPath(EditorFileNode* node, const std::filesystem::path& path);
    EditorFileNode* FindContentBrowserNodeAcrossRoots(EditorFileService& service, const std::filesystem::path& path);
    EditorFileNode* FindParentFolderNode(EditorFileService& service, const EditorFileNode* node);

    // Root-to-leaf folder chain for breadcrumb navigation (includes `folder`).
    std::vector<EditorFileNode*> CollectFolderBreadcrumbChain(EditorFileService& service, EditorFileNode* folder);

    bool IsShaderAssetNode(const EditorFileNode* node);
    bool IsZassetProductNode(const EditorFileNode* node);

    void TrimRenameBufferInPlace(char* buffer, size_t capacity);
    bool IsValidRenameName(const std::string& name);
    bool IsProtectedRenamePath(const std::filesystem::path& path);
    void UnloadAssetStreamsUnderPath(const std::filesystem::path& target_path);

    std::filesystem::path ResolveDropTargetFolder(const EditorFileNode* node);
    std::filesystem::path BuildUniquePrefabAssetPath(const std::filesystem::path& folder, const eastl::string& base_name);
    GameObject* ResolveLiveGameObject(GObjectID id);

    bool WriteTextFileIfMissing(const std::filesystem::path& path, const std::string& content);
}
