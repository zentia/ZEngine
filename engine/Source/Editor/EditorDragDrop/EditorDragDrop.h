#pragma once

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Project/ProjectInfo.h"
#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/SDragSource.h"

#include <filesystem>
#include <memory>
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

// ----------------------------------------------------------------------------
// Shared drop-target helpers (single source of truth).
//
// A drag carries an ABSOLUTE path (that is what the Content Browser publishes),
// but serialized fields -- SubMeshRes::m_MeshAsset, material asset lists,
// AnimatorState::clip_file_path -- store paths RELATIVE to the project content
// root (see InspectorAssetCommon::ResolveProjectAssetPath, which resolves them
// back by prefixing that root). Every drop target must therefore convert before
// writing, and all of them must agree on the form. Do the conversion here.
// ----------------------------------------------------------------------------

namespace EditorDragDrop
{

/// Downcast + payload-type check. Returns nullptr unless `op` is a Content
/// Browser asset drag carrying a non-empty path.
inline const FAssetDragDropOp* AsAssetDrop(const std::shared_ptr<ZSlate::FDragDropOperation>& op)
{
    if (op == nullptr || op->PayloadType != EditorDragDrop::kZSlateAssetPayloadAssetPath)
    {
        return nullptr;
    }
    return dynamic_cast<const FAssetDragDropOp*>(op.get());
}

/// Absolute -> project-content-relative, i.e. the form a serialized field
/// stores. Falls back to the absolute path when the asset lives outside the
/// project (better a working-but-ugly path than a broken ".." one).
inline std::string MakeDroppedAssetStoredPath(const std::filesystem::path& absolute_path)
{
    auto* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr)
    {
        return absolute_path.generic_string();
    }

    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(absolute_path, ec).lexically_normal();
    const std::filesystem::path content = std::filesystem::absolute(project_info->GetProjectContent(), ec).lexically_normal();
    if (content.empty())
    {
        return absolute.generic_string();
    }

    const std::filesystem::path relative = absolute.lexically_relative(content);
    const std::string relative_str = relative.generic_string();
    if (relative_str.empty() || relative_str.rfind("..", 0) == 0)
    {
        return absolute.generic_string();
    }
    return relative_str;
}

/// Project-content-relative -> absolute: the inverse of
/// MakeDroppedAssetStoredPath, for a drag SOURCE that stores the relative form
/// (every serialized asset field does) but must publish the absolute form.
inline std::filesystem::path MakeDroppedAssetAbsolutePath(const std::string& stored_path)
{
    std::filesystem::path path(stored_path);
    if (path.is_absolute() || stored_path.empty())
    {
        return path;
    }
    auto* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->GetProjectContent().empty())
    {
        return path;
    }
    return (std::filesystem::path(project_info->GetProjectContent()) / path).lexically_normal();
}

/// Builds the op for one asset path, or nullptr when there is nothing to carry
/// (an empty row must not start a gesture -- otherwise the user drags "nothing"
/// and every target lights up for a payload it cannot use).
inline std::shared_ptr<FAssetDragDropOp> MakeAssetDragOperation(const std::string& stored_path)
{
    if (stored_path.empty())
    {
        return nullptr;
    }
    const std::filesystem::path absolute = MakeDroppedAssetAbsolutePath(stored_path);
    if (absolute.empty())
    {
        return nullptr;
    }

    auto op = std::make_shared<FAssetDragDropOp>();
    op->PayloadType = EditorDragDrop::kZSlateAssetPayloadAssetPath;
    op->AssetPath = absolute.generic_string();
    op->DecoratorText = absolute.filename().generic_string();
    return op;
}

/// Wraps any widget so it is a drag SOURCE for one asset path: `get_path` is
/// polled when the drag starts, so the value travels as it is right now.
/// Returning an empty string opts out (no gesture).
///
/// The generic form is what an arbitrary "asset path row" needs: the row may be a
/// text box, a drop target wrapping one, or a read-only value.
///
/// Caveat: a drag inside a focused text box wins over selecting its text. That is
/// the intended trade for path rows (you rarely select a path by hand), but do
/// not wrap prose fields with this.
inline std::shared_ptr<ZSlate::SWidget> MakeAssetPathDragSource(const std::shared_ptr<ZSlate::SWidget>& content,
                                                                const std::function<std::string()>& get_path)
{
    auto source = std::make_shared<ZSlate::SDragSource>();
    source->SetContent(content);
    source->OnDragDetectedHandler = [get_path](const ZSlate::Vector2& /*screen_pos*/) -> std::shared_ptr<ZSlate::FDragDropOperation> {
        if (!get_path)
        {
            return nullptr;
        }
        return EditorDragDrop::MakeAssetDragOperation(get_path());
    };
    return source;
}

/// Convenience overload for the common case: the row IS the text box holding the
/// stored path.
inline std::shared_ptr<ZSlate::SWidget> MakeAssetPathDragSource(const std::shared_ptr<ZSlate::SEditableTextBox>& box)
{
    return EditorDragDrop::MakeAssetPathDragSource(std::static_pointer_cast<ZSlate::SWidget>(box),
                                                   [box]() -> std::string { return box != nullptr ? box->Text : std::string(); });
}

}  // namespace EditorDragDrop
