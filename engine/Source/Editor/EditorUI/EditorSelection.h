#pragma once

#include "Editor/EditorSceneManager/EditorSceneManager.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class GameObject;

// Unity Selection-style facade over EditorSceneManager (single entry for panels/tools).
class EditorSelection
{
public:
    static std::shared_ptr<EditorSceneManager> GetSceneManager();

    static std::weak_ptr<GameObject> GetActiveGameObject();
    static GObjectID GetActiveGameObjectId();
    static const std::vector<GObjectID>& GetSelectedGameObjectIds();
    static size_t GetSelectedGameObjectCount();
    static bool IsGameObjectSelected(GObjectID object_id);

    static void SelectGameObject(GObjectID object_id,
                                 GObjectSelectionOp op = GObjectSelectionOp::Replace);
    static void SelectGameObjectRange(GObjectID end_object_id,
                                      const std::vector<GObjectID>& visible_order);
    static void ClearGameObjectSelection();

    static const std::filesystem::path& GetActiveAssetPath();
    static const std::string& GetActiveAssetType();
    static void SelectAsset(const std::filesystem::path& asset_path, const std::string& asset_type);
};
