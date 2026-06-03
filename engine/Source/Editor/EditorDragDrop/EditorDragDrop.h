#pragma once

#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Slate/Application/SlateDragDrop.h"

#include <string>

// =====================================================================================
// editor_drag_drop.h
// -------------------------------------------------------------------------------------
// Single source of truth for drag-and-drop payload type strings used between editor
// windows. ImGui's drag-drop API matches payloads by C-string, so source/target
// windows must agree on the exact same key — define the contract here once.
//
// Convention: payload keys are short (<= 32 chars, ImGui internal limit) UPPER_SNAKE
// names prefixed with "ZE_" so they don't collide with ImGui-builtin payload types
// such as "_COL3F" / "_COL4F".
// =====================================================================================

namespace EditorDragDrop
{
    /// Hierarchy-window → Project-window: dragging a live GameObject onto the project
    /// view creates a `.zasset` Prefab from that GameObject's subtree (mirrors Unity's
    /// "drag from Hierarchy onto Project").
    ///
    /// Payload contents: a single `GObjectID` (POD integer ID local to the active Level).
    /// Receivers should resolve it through `WorldManager::getCurrentActiveLevel()` →
    /// `Level::GetGObjectByID()` and treat a missing/expired weak_ptr as a benign abort
    /// (the GameObject may have been deleted between drag start and drop).
    constexpr const char* kPayloadHierarchyGameObject = "ZE_HIERARCHY_GO";

    /// Project-window → Hierarchy / Scene: dragging a droppable `.zasset` spawns
    /// content in the active Level (Unity-style). Supported: Prefab, MeshData.
    ///
    /// Payload contents: a NUL-terminated UTF-8 absolute filesystem path, including
    /// the trailing NUL. The payload is sized as `strlen(path) + 1` so the receiver
    /// can treat `payload->Data` as a C string.
    ///
    /// Receivers call `EditorScenePlacement::InstantiateDroppedContentBrowserAsset`. The
    /// Project window only starts a drag for known droppable types (prefab, meshdata).
    constexpr const char* kPayloadContentBrowserPrefab = "ZE_CONTENT_BROWSER_PREFAB";

    // ------------------------------------------------------------------------
    // Native ZSlate drag-drop payload discriminators (FDragDropOperation::
    // PayloadType). Distinct from the ImGui keys above: the ZSlate windows
    // (Hierarchy / Project) emit FDragDropOperation, not ImGui payloads. These
    // strings are what a drop target compares before casting.
    // ------------------------------------------------------------------------

    /// A live GameObject dragged by its 64-bit GObjectID (FDragDropOperation::Id).
    /// Emitted by the ZSlate Hierarchy rows. The Scene viewport accepts it to
    /// reparent the dragged object under the object picked at the drop position.
    constexpr const char* kZSlateAssetPayloadGObjectId = "GObjectID";

    /// A Project `.zasset` dragged by absolute path (FAssetDragDropOp::AssetPath).
    /// Emitted by ZSlate Project asset rows. The Scene viewport accepts it to
    /// instantiate the asset (Prefab / MeshData) into the active Level.
    constexpr const char* kZSlateAssetPayloadAssetPath = "AssetPath";
}  // namespace EditorDragDrop

// ZSlate drag operation that carries a Project asset's absolute path. Subclass of
// the base FDragDropOperation so the common Id/DecoratorText still apply (the
// DecoratorText holds the display name for the drag label).
struct FAssetDragDropOp : public ZSlate::FDragDropOperation
{
    std::string AssetPath;  // absolute .zasset filesystem path
};
