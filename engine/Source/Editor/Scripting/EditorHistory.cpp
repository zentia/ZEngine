#include "EditorHistory.h"

#include "Editor/Undo/EditorUndo.h"
#include "Runtime/Core/Base/SystemRegistry.h"

#include <string>
#include <vector>

namespace
{
    eastl::string ToEastl(const std::string& text)
    {
        return eastl::string(text.c_str(), text.size());
    }
}  // namespace

bool EditorHistory::CanUndo()
{
    return EditorUndo::CanUndo();
}

bool EditorHistory::CanRedo()
{
    return EditorUndo::CanRedo();
}

bool EditorHistory::Undo()
{
    return EditorUndo::Undo();
}

bool EditorHistory::Redo()
{
    return EditorUndo::Redo();
}

eastl::string EditorHistory::GetUndoTitle()
{
    return ToEastl(EditorUndo::GetUndoTitle());
}

eastl::string EditorHistory::GetRedoTitle()
{
    return ToEastl(EditorUndo::GetRedoTitle());
}

int EditorHistory::GetUndoCount()
{
    // Counts live on the engine-agnostic core (UndoSystem), not on
    // EditorUndoSystem itself, so they are reached through GetCore().
    // EditorUndo::GetSystem() is null before the editor systems are up and
    // during level load, which every entry point here treats as "empty
    // history" rather than faulting.
    EditorUndoSystem* undo = EditorUndo::GetSystem();
    return undo ? static_cast<int>(undo->GetCore().GetUndoCount()) : 0;
}

int EditorHistory::GetRedoCount()
{
    EditorUndoSystem* undo = EditorUndo::GetSystem();
    return undo ? static_cast<int>(undo->GetCore().GetRedoCount()) : 0;
}

void EditorHistory::BeginTransaction(const eastl::string& title)
{
    EditorUndo::BeginTransaction(std::string(title.c_str(), title.size()));
}

void EditorHistory::EndTransaction()
{
    EditorUndo::EndTransaction();
}

void EditorHistory::CancelTransaction()
{
    EditorUndo::CancelTransaction();
}

bool EditorHistory::IsRecording()
{
    return EditorUndo::IsRecording();
}

void EditorHistory::ClearHistory(const eastl::string& reason)
{
    EditorUndoSystem* undo = EditorUndo::GetSystem();
    if (undo == nullptr)
        return;
    undo->ClearHistory(reason.empty() ? "cleared from editor script" : reason.c_str());
}

// ---- recording (P3) --------------------------------------------------------

namespace
{
    /// Falls back to a generic title rather than passing "" down: an undo entry
    /// with an empty label shows up as "Undo " in the menu.
    std::string RecordTitle(const eastl::string& title, const char* fallback)
    {
        if (title.empty())
            return fallback;
        return std::string(title.c_str(), title.size());
    }
}  // namespace

void EditorHistory::RecordComponent(Component* component, const eastl::string& title)
{
    if (component == nullptr)
        return;
    // EditorUndo::RecordComponent already no-ops when recording is off and
    // routes a Transform to RecordTransform, so nothing is duplicated here.
    EditorUndo::RecordComponent(component, RecordTitle(title, "Modify Component"));
}

void EditorHistory::RecordTransform(Transform* transform, const eastl::string& title)
{
    if (transform == nullptr)
        return;
    EditorUndo::RecordTransform(transform, RecordTitle(title, "Modify Transform"));
}

void EditorHistory::RecordGameObject(GameObject* object, const eastl::string& title)
{
    if (object == nullptr)
        return;
    // The object's identity for undo purposes is its Transform: that record
    // carries local TRS, parent, sibling index and the active flag.
    Transform* transform = object->tryGetComponent<Transform>("Transform");
    if (transform == nullptr)
        return;
    EditorUndo::RecordTransform(transform, RecordTitle(title, "Modify GameObject"));
}

void EditorHistory::RegisterCreatedGameObject(GameObject* object, const eastl::string& title)
{
    if (object == nullptr)
        return;
    EditorUndo::RegisterCreatedGameObject(object->GetID(), RecordTitle(title, "Create GameObject"));
}

int EditorHistory::DestroyGameObject(GameObject* object, const eastl::string& title)
{
    if (object == nullptr)
        return 0;
    const std::vector<GObjectID> ids {object->GetID()};
    return static_cast<int>(EditorUndo::DestroyGameObjects(ids, RecordTitle(title, "Delete GameObject")));
}
