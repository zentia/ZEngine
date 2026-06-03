#pragma once

#include "Runtime/ExportRuntime.h"

#include <memory>
#include <string>
#include <vector>

struct EditorFileNode;
using EditorFileNodeArray = std::vector<std::shared_ptr<EditorFileNode>>;

struct EditorFileNode
{
    eastl::string m_FileName;

    // Lower-case file extension WITHOUT the leading dot (e.g. "zasset", "ts",
    // "hlsl", "json", "shader"). For folder nodes the special token "folder"
    // is used. This is the routing key (analogous to UE's
    // FPaths::GetExtension() / Unity's Path.GetExtension()) used to decide
    // "is this a script source vs a binary asset vs a folder", regardless of
    // the asset that's actually inside the file.
    std::string m_FileExtension;

    // For binary .zasset files, the lower-case reflection class name with the
    // trailing "Res" stripped (e.g. "prefab", "material", "mesh", "texture").
    // Empty for folders, scripts, configs and any non-.zasset file. This is
    // the equivalent of UE's FAssetData::AssetClassPath / Unity's
    // AssetDatabase.GetMainAssetTypeAtPath(). For multi-extension shader
    // sources (.vert.hlsl etc.) and .json variants (.transform.component
    // etc.) the secondary extension is ALSO stored here so consumers can
    // disambiguate without re-parsing the path.
    std::string m_AssetType;

    eastl::string m_FilePath;
    int m_NodeDepth;
    EditorFileNodeArray m_ChildNodes;

    EditorFileNode() = default;
    EditorFileNode(const eastl::string& name,
                   const std::string& extension,
                   const std::string& asset_type,
                   const eastl::string& path,
                   int depth)
        : m_FileName(name), m_FileExtension(extension), m_AssetType(asset_type), m_FilePath(path), m_NodeDepth(depth)
    {
    }

    // Convenience: returns the most specific type label for UI display
    // (m_AssetType if non-empty, otherwise m_FileExtension). This is a
    // pure presentation helper -- selection / dispatch logic should always
    // branch on the underlying field that matches its semantics, not on
    // this string.
    const std::string& displayTypeLabel() const
    {
        return m_AssetType.empty() ? m_FileExtension : m_AssetType;
    }

    bool isFolder() const { return m_FileExtension == "folder"; }
};

class EditorFileService
{
    EditorFileNodeArray m_FileNodeArray;
    EditorFileNode m_RootNode {"asset", "folder", "", "asset", -1};

    // Ordered list of root indices into m_FileNodeArray. Index 0 is always
    // the Assets root (built first, kept for legacy callers that use
    // getEditorRootNode()). Additional roots (e.g. the Scripts/ source root)
    // are appended in the order they were registered.
    std::vector<EditorFileNode*> m_RootNodes;

private:
    EditorFileNode* GetParentNodePtr(EditorFileNode* file_node);
    bool CheckFileArray(EditorFileNode* file_node);

    // Walk one root path on disk and append nodes to m_FileNodeArray.
    // `root_label` is the synthetic top-level name shown in the Project
    // window (e.g. "Assets", "Scripts"). `display_name` overrides the
    // root_label for the on-screen folder; pass empty to use root_label.
    // Returns the newly-added root node, or nullptr if root_path is empty
    // / does not exist.
    EditorFileNode* BuildRoot(const std::filesystem::path& root_path,
                              const eastl::string& root_label,
                              const eastl::string& display_name);

public:
    /// Legacy single-root accessor: returns the FIRST root (Assets). Existing
    /// callers (selection sync, refresh-by-path, etc.) operate on Assets and
    /// keep working unchanged.
    EditorFileNode* getEditorRootNode() { return m_RootNodes.empty() ? nullptr : m_RootNodes.front(); }

    /// All roots in registration order. The Project window iterates this to
    /// draw multiple top-level trees (Assets, Scripts, ...).
    const std::vector<EditorFileNode*>& getEditorRootNodes() const { return m_RootNodes; }

    void BuildEngineFileTree();
};