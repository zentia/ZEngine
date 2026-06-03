#pragma once

#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"

#include <filesystem>
#include <string>

// Instantiate Project-window assets into the active Level (Unity-style).
namespace EditorScenePlacement
{
    // Normalize runtime class name ("MeshData", "Prefab", ...) to inspector-style type.
    std::string NormalizeDroppedAssetType(std::string asset_type);

    // True when `absolute_zasset_path` is a MeshData .zasset under the project Assets root.
    bool IsMeshDataZAsset(const std::filesystem::path& absolute_zasset_path);

    // Project-relative path stored on SubMeshRes::m_MeshAsset (e.g. "Models/Foo.zasset").
    std::string MakeContentRelativeAssetPath(const std::filesystem::path& absolute_path);

    // Create a GameObject + MeshRenderer referencing the mesh .zasset. Selects and frames it.
    bool InstantiateMeshAssetInCurrentLevel(const std::filesystem::path& absolute_zasset_path,
                                            GObjectID parent_gobject_id = k_invalid_gobject_id);

    // Dispatches by .zasset header type (MeshData vs Prefab only). Returns true when spawned.
    bool InstantiateDroppedContentBrowserAsset(const std::filesystem::path& absolute_zasset_path,
                                       GObjectID parent_gobject_id = k_invalid_gobject_id);

    // Deferred drop (safe outside UI tree walks). Last request wins per frame.
    // When `parent_gobject_id` is valid, the spawned root is parented under that object.
    void RequestDrop(std::filesystem::path absolute_zasset_path,
                     GObjectID parent_gobject_id = k_invalid_gobject_id);
    void ExecutePendingDrop();
}  // namespace EditorScenePlacement
