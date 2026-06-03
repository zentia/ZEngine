#include "EditorSelection.h"

#include "Runtime/Core/Base/Macro.h"

std::shared_ptr<EditorSceneManager> EditorSelection::GetSceneManager()
{
    return GET_SYSTEM(EditorSceneManager);
}

std::weak_ptr<GameObject> EditorSelection::GetActiveGameObject()
{
    if (auto scene = GetSceneManager())
    {
        return scene->GetSelectedGObject();
    }
    return {};
}

GObjectID EditorSelection::GetActiveGameObjectId()
{
    if (auto scene = GetSceneManager())
    {
        return scene->getSelectedObjectID();
    }
    return k_invalid_gobject_id;
}

const std::vector<GObjectID>& EditorSelection::GetSelectedGameObjectIds()
{
    static const std::vector<GObjectID> kEmpty;
    if (auto scene = GetSceneManager())
    {
        return scene->GetSelectedObjectIDs();
    }
    return kEmpty;
}

size_t EditorSelection::GetSelectedGameObjectCount()
{
    if (auto scene = GetSceneManager())
    {
        return scene->GetSelectedObjectCount();
    }
    return 0;
}

bool EditorSelection::IsGameObjectSelected(GObjectID object_id)
{
    if (auto scene = GetSceneManager())
    {
        return scene->IsGObjectSelected(object_id);
    }
    return false;
}

void EditorSelection::SelectGameObject(GObjectID object_id, GObjectSelectionOp op)
{
    if (auto scene = GetSceneManager())
    {
        scene->OnGObjectSelected(object_id, op);
    }
}

void EditorSelection::SelectGameObjectRange(GObjectID end_object_id,
                                            const std::vector<GObjectID>& visible_order)
{
    if (auto scene = GetSceneManager())
    {
        scene->OnGObjectRangeSelected(end_object_id, visible_order);
    }
}

void EditorSelection::ClearGameObjectSelection()
{
    SelectGameObject(k_invalid_gobject_id, GObjectSelectionOp::Replace);
}

const std::filesystem::path& EditorSelection::GetActiveAssetPath()
{
    static const std::filesystem::path kEmpty;
    if (auto scene = GetSceneManager())
    {
        return scene->getSelectedAssetPath();
    }
    return kEmpty;
}

const std::string& EditorSelection::GetActiveAssetType()
{
    static const std::string kEmpty;
    if (auto scene = GetSceneManager())
    {
        return scene->getSelectedAssetType();
    }
    return kEmpty;
}

void EditorSelection::SelectAsset(const std::filesystem::path& asset_path,
                                  const std::string& asset_type)
{
    if (auto scene = GetSceneManager())
    {
        scene->OnAssetSelected(asset_path, asset_type);
    }
}
