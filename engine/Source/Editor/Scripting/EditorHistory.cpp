#include "EditorHistory.h"

#include "Editor/Undo/EditorUndo.h"
#include "Runtime/Core/Base/SystemRegistry.h"

#include <string>

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
