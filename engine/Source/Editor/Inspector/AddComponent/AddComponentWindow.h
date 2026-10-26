#pragma once

// =====================================================================================
// Add Component -- the editor's "give this GameObject a new component" surface.
// -------------------------------------------------------------------------------------
// Two pieces:
//
//   * AddComponentCatalog  the list of component types that can be attached, built
//                          from the reflection registry (TypeManager::FindAllDerivedTypes
//                          over Component) rather than a hand-maintained table, so a
//                          component added to the engine shows up here without anyone
//                          remembering to edit this file. A small curated table only
//                          supplies Unity-style menu grouping and hides the internal /
//                          abstract bases that must not be instantiated.
//
//   * AddComponentWindow   the panel itself: a search box over the catalog and a
//                          clickable list. It targets the current selection, so it
//                          behaves like Unity's Inspector "Add Component" button.
//
// Attaching goes through AddComponentWindow::AddComponentTo(), which is deliberately
// a free-standing helper: it is the one place that knows the create -> register ->
// PostLoadResource -> attach -> undo order, and the Inspector's own Add Component
// button can call it without going through the window.
// =====================================================================================

#include "Editor/EditorWindow/EditorWindow.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlateGeometry.h"  // Vector2
#include "ZSlate/Core/SlatePaint.h"

#include <memory>
#include <string>
#include <vector>

namespace ZSlate
{
class SWidget;
class SScrollBox;
class SEditableTextBox;
class STextBlock;
}  // namespace ZSlate

class Component;
class GameObject;
class Type;

/// One row of the Add Component list. `type` is the reflection Type of the component
/// class; the display strings are owned by the generated Type and stay valid for the
/// lifetime of the process.
struct AddComponentEntry
{
    const Type* type {nullptr};
    const char* display_name {nullptr};
    const char* category {nullptr};  // Unity-style menu group, e.g. "Rendering"
};

class AddComponentCatalog
{
public:
    /// Every addable component type, sorted by category and then by display name.
    /// Built on first call and cached; the reflection registry is static once
    /// TypeManager::Initialize has run.
    static const std::vector<AddComponentEntry>& GetEntries();

    /// Drop the cache. Only needed if types are registered after startup.
    static void Invalidate();

    /// Indices into GetEntries() whose name (or category) contains `query`,
    /// case-insensitively. An empty query returns everything.
    static std::vector<int> Filter(const std::string& query);

    /// Unity-style menu group for a component type. Unknown types fall back to
    /// "Component" so a new engine component is never unreachable.
    static const char* CategoryFor(const Type* type);

    /// True for types that must not be offered: abstract bases, the internal
    /// prefab bookkeeping component, and Transform (every GameObject has one).
    static bool IsHidden(const Type* type);
};

class AddComponentWindow : public EditorWindow
{
public:
    explicit AddComponentWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    /// Instantiate `component_type` and attach it to `host`. Returns the new
    /// component, or nullptr when the type cannot be produced or either argument
    /// is null. The whole thing is one undo step ("Add <Name>").
    ///
    /// Creation goes through ObjectManager::Produce + AllocateAndAssignInstanceID,
    /// NOT MemoryManager::CreateObject: only the former gives the component a real
    /// InstanceID, and a component with id 0 is invisible to PPtr / ImmediatePtr
    /// (they short-circuit on it), so it would silently fail to serialize.
    static Component* AddComponentTo(GameObject* host, const Type* component_type);

private:
    void BuildLayout(float scale);
    void RebuildRows();
    void Commit(int entry_index);
    GameObject* ResolveTarget() const;

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::SScrollBox> m_List;
    std::shared_ptr<ZSlate::SEditableTextBox> m_SearchBox;
    std::shared_ptr<ZSlate::STextBlock> m_HintText;

    std::string m_Search;
    std::vector<int> m_Filtered;
    bool m_FilterDirty {true};
    float m_BuiltScale {-1.0f};

    ZSlate::SlateInputRouter m_Input;
};
