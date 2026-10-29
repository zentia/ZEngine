#include "EditorSelection.h"

#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

#include <vector>

namespace
{
    /// Resolves a selected GObjectID into a raw GameObject*.
    ///
    /// The level owns the object through a shared_ptr and hands out a weak_ptr;
    /// scripts get a borrowed raw pointer that is only valid for the duration
    /// of the call. That is the same contract every other script-facing
    /// GameObject* has, and it is why nothing here is cached across frames: a
    /// stale ID (object deleted after it was selected) resolves to null instead
    /// of a dangling pointer.
    GameObject* ResolveObject(GObjectID id)
    {
        if (id == k_invalid_gobject_id)
            return nullptr;
        auto world = GET_SYSTEM(WorldManager);
        if (!world)
            return nullptr;
        Level* level = world->getCurrentActiveLevel();
        if (level == nullptr)
            return nullptr;
        return level->GetGObjectByID(id).lock().get();
    }

    /// Selected IDs, or an empty span when the editor scene manager is absent.
    const std::vector<GObjectID>* SelectedIds()
    {
        auto scene = GET_SYSTEM(EditorSceneManager);
        return scene ? &scene->GetSelectedObjectIDs() : nullptr;
    }
}  // namespace

int EditorSelection::GetObjectCount()
{
    const std::vector<GObjectID>* ids = SelectedIds();
    return ids ? static_cast<int>(ids->size()) : 0;
}

GameObject* EditorSelection::GetObjectAt(int index)
{
    const std::vector<GObjectID>* ids = SelectedIds();
    if (ids == nullptr || index < 0 || static_cast<size_t>(index) >= ids->size())
        return nullptr;
    return ResolveObject((*ids)[static_cast<size_t>(index)]);
}

GameObject* EditorSelection::GetActiveObject()
{
    auto scene = GET_SYSTEM(EditorSceneManager);
    if (!scene)
        return nullptr;
    // GetSelectedGObject() is the Inspector's own accessor, so "active" here
    // means exactly what the Inspector is showing.
    return scene->GetSelectedGObject().lock().get();
}

int EditorSelection::GetInstanceIdAt(int index)
{
    const std::vector<GObjectID>* ids = SelectedIds();
    if (ids == nullptr || index < 0 || static_cast<size_t>(index) >= ids->size())
        return 0;
    return static_cast<int>((*ids)[static_cast<size_t>(index)]);
}

void EditorSelection::SetActiveObject(GameObject* object)
{
    auto scene = GET_SYSTEM(EditorSceneManager);
    if (!scene)
        return;
    if (object == nullptr)
    {
        Clear();
        return;
    }
    // Goes through SetGObjectSelection rather than setSelectedObjectID: the
    // latter only writes the legacy single-ID field and would leave the
    // multi-select vector (what the Hierarchy highlights) untouched.
    const std::vector<GObjectID> ids {object->GetID()};
    scene->SetGObjectSelection(ids, object->GetID());
}

bool EditorSelection::SelectByName(const eastl::string& name)
{
    auto world = GET_SYSTEM(WorldManager);
    if (!world)
        return false;
    Level* level = world->getCurrentActiveLevel();
    if (level == nullptr)
        return false;
    GameObject* object = level->FindGObjectByName(name);
    if (object == nullptr)
        return false;
    SetActiveObject(object);
    return true;
}

void EditorSelection::Clear()
{
    auto scene = GET_SYSTEM(EditorSceneManager);
    if (!scene)
        return;
    scene->SetGObjectSelection({}, k_invalid_gobject_id);
}
