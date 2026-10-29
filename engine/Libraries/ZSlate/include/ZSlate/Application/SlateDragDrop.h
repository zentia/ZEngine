#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ZSlate
{
// Payload carried during a drag-drop gesture (UE Slate FDragDropOperation
// analogue). Subclass to attach richer data; the base already covers the common
// editor case of "drag a thing identified by a 64-bit id of some named kind".
//
// Lifecycle: created by a drag source's OnDragDetected(), held by the
// SlateInputRouter for the duration of the gesture, handed to drop targets via
// OnDragOver / OnDrop, and released when the gesture ends.
struct FDragDropOperation
{
    virtual ~FDragDropOperation() = default;

    // Discriminator so a drop target can decide whether it understands the
    // payload (e.g. "GObjectID", "AssetPath"). Compare before casting.
    std::string PayloadType;

    // Optional text drawn under the cursor while dragging (host-driven).
    std::string DecoratorText;

    // Common-case payload: an opaque 64-bit id (GObjectID, asset handle, ...).
    // Subclasses may ignore this and store their own fields instead.
    uint64_t Id {0};
};

// ----------------------------------------------------------------------------
// Process-wide "currently active drag operation".
//
// Each SlateInputRouter owns the drag gesture it STARTED (press path, capture,
// drop dispatch onto its own widget tree). That is enough for drags that begin
// and end inside one window. Cross-window drops -- e.g. dragging a Project asset
// onto the (composited, non-widget) Scene viewport, or a Hierarchy object into
// the viewport to reparent it -- need a channel the *target* side can observe
// even though it lives in a different router. The router that starts a gesture
// publishes its op here; it clears it when the gesture ends.
//
// Consumers must NOT assume the op is still published on the exact release frame
// (the source router may clear it before the consumer's per-frame tick runs,
// depending on window draw order). Cache the last non-null op locally and act on
// your own button-release edge -- see ZSlateSceneWindow. Editor UI is
// single-threaded (game thread), so no locking is needed.
//
// inline + function-local static => exactly one instance across all TUs.
inline std::shared_ptr<FDragDropOperation>& GActiveDragOperation()
{
    static std::shared_ptr<FDragDropOperation> op;
    return op;
}

inline void SetActiveDragOperation(const std::shared_ptr<FDragDropOperation>& op) { GActiveDragOperation() = op; }
inline std::shared_ptr<FDragDropOperation> GetActiveDragOperation() { return GActiveDragOperation(); }
inline bool IsDragDropActive() { return GActiveDragOperation() != nullptr; }
}  // namespace ZSlate
