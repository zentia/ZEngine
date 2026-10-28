#pragma once

#include "Runtime/BaseClasses/PPtr.h"

#include <EASTL/string.h>

#include <cstdint>
#include <memory>
#include <vector>

class GameObject;
class IUndoRecord;
class Level;
class Object;
class PrefabInstance;

// =====================================================================================
// PrefabOverrideModel -- what the Inspector does with a prefab instance's overrides
// (doc/PrefabSystem_Design.md §9.4 item 3). No UI here: the ZSlate Inspector calls
// these after an edit / on a button, and the end-to-end smoke test drives them
// headlessly.
// -------------------------------------------------------------------------------------
// Unity model: an override is implicit. Editing a field of an object inside a prefab
// instance records it; setting it back to the source value removes it. So the
// Inspector calls SyncFieldOverride after every committed edit, with the property path
// of the edited field (a leaf like "m_LocalScale.x" or a compound like
// "m_LocalScale" -- a compound is split into the leaves that actually differ).
//
// Transform's position: the field to name is m_LocalPositionD (the float
// m_LocalPosition is a legacy copy the writer derives from it).
// =====================================================================================
namespace PrefabOverrideModel
{
    struct OverrideRow
    {
        enum class Kind
        {
            Property,          ///< a PropertyModification: (target, property_path)
            AddedComponent,    ///< target = the component the instance added
            RemovedComponent,  ///< target = the SOURCE component the instance suppresses
        };

        Kind kind = Kind::Property;
        PPtr<Object> target;
        eastl::string property_path;  ///< empty for the two component kinds
        eastl::string label;          ///< "<GameObject name> / <component type>"
        /// Whether Apply can push this row into the source. Component rows can be
        /// applied only to a Variant (into its own added / removed lists); a plain
        /// prefab has no structural Apply yet, so the Inspector offers Revert only.
        bool can_apply = true;
    };

    /// The instance `game_object` belongs to in `level` (root or any descendant), or null.
    PrefabInstance* FindInstance(const Level* level, const GameObject* game_object);

    /// True when `inner` is `outer` or a sub-path of it ("a" covers "a.x" and "a[0]").
    bool PathCovers(const eastl::string& outer, const eastl::string& inner);

    /// True when an override on `target` covers `path` or lies under it.
    bool IsOverridden(const PrefabInstance* instance, const Object* target, const eastl::string& path);

    /// Property overrides first, then one row per added and per removed component.
    /// The Inspector's "N overrides" header and its Apply All / Revert All buttons are
    /// driven by this list — before component rows were listed here, an instance
    /// whose only overrides were structural showed "0 overrides" and no buttons.
    std::vector<OverrideRow> ListOverrides(const PrefabInstance* instance);

    /// Any property OR structural override.
    bool HasOverrides(const PrefabInstance* instance);

    /// Revert one component row: an added component is detached, a suppressed one
    /// comes back (with the source's — and the Variant chain's — values). Returns
    /// false for a Property row or when the row no longer matches the instance.
    bool RevertStructuralRow(PrefabInstance* instance, const OverrideRow& row);

    /// After an edit of (target, path): record every leaf under `path` whose value now
    /// differs from the source, drop the overrides of leaves that equal it again.
    /// Returns the number of overrides under `path` afterwards (-1: target is not an
    /// object of this instance, or the path does not resolve).
    int SyncFieldOverride(PrefabInstance* instance, Object* target, const eastl::string& path);

    /// Revert every override under (target, path) to the source value. Returns how many.
    int RevertField(PrefabInstance* instance, Object* target, const eastl::string& path);

    struct ApplyReport
    {
        int applied = 0;        ///< overrides pushed into the source
        bool saved = false;     ///< the source prefab file was rewritten
        int merged = 0;         ///< other instances of the same source in the level refreshed
    };

    /// Apply the overrides under (target, path) -- or all of them with ApplyAll -- to the
    /// source prefab, write the source file (same fileIDs, so references into it stay
    /// valid), and refresh the level's other instances of that prefab (their own
    /// overrides survive: MergeFromSource).
    ///
    /// ApplyField applies that one field only — never the instance's structural
    /// overrides. ApplyAll on a Variant instance also commits the added / removed
    /// components; on a plain prefab structural overrides stay on the instance.
    ApplyReport ApplyField(const Level* level, PrefabInstance* instance, Object* target, const eastl::string& path);
    ApplyReport ApplyAll(const Level* level, PrefabInstance* instance);

    /// Rewrite the source prefab file from the loaded (possibly applied-to) source graph,
    /// keeping every object's fileID. YAML (.prefab) or binary by extension.
    bool SaveSourcePrefab(PrefabInstance* instance);

    /// Undo record for the instance's override list (UndoSystem swap semantics). The
    /// Inspector offers it next to RecordComponent before an edit, so Ctrl+Z restores
    /// the list together with the field -- otherwise an undone edit would leave an
    /// override recording a value the object no longer has. Records with the same key
    /// coalesce within a transaction; a record whose list did not change is dropped.
    std::unique_ptr<IUndoRecord> MakeOverrideListUndoRecord(PrefabInstance* instance);
    uint64_t OverrideListUndoKey(const PrefabInstance* instance);

    /// Undo record for the instance's component STRUCTURE: the component list of every
    /// GameObject it owns, plus the added / removed lists and the corresponding-source
    /// map. Offer it (once per transaction, keyed by StructureUndoKey) before anything
    /// that adds, detaches or re-introduces components -- Revert All, a component row's
    /// Revert -- so Ctrl+Z puts the components back and Ctrl+Y takes them away again.
    /// Separate from the override-list record because that one only knows property
    /// overrides. Nothing on these paths destroys a component, so undo/redo just swap
    /// the lists (parked clones live until the level unloads).
    std::unique_ptr<IUndoRecord> MakeStructureUndoRecord(PrefabInstance* instance);
    uint64_t StructureUndoKey(const PrefabInstance* instance);
}  // namespace PrefabOverrideModel
