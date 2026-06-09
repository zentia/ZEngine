#pragma once

#include "Editor/EditorWindow/EditorWindow.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"  // GObjectID, k_invalid_gobject_id
#include "Editor/Menu/ZSlatePopupMenu.h"                   // combo dropdowns for native asset inspectors
#include "Runtime/Slate/Application/SlateInput.h"
#include "Runtime/Slate/Core/SlatePaint.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Component;
class Type;

namespace ZSlate
{
class SWidget;
class SVerticalBox;
class STextBlock;
class SEditableTextBox;
class SCheckBox;
}

namespace ZSlateInspectorDetail
{
// Scalar field categories the generic component drawer can read/write in place.
enum class FieldKind
{
    Float,
    Int,
    UInt,
    UInt8,
    Bool,
    Str,
    Vec3,
};

// Binds a ZSlate widget to a serialized field of a selected component. The field
// is addressed by (component index in GameObject::getComponents(), byte offset in
// the component's TypeTree) and re-resolved every frame so we never dangle when
// the selection or component set changes.
struct FieldBinding
{
    FieldKind kind {FieldKind::Float};
    size_t component_index {0};
    uint32_t byte_offset {0};
    bool read_only {false};
    std::array<std::shared_ptr<ZSlate::SEditableTextBox>, 3> boxes {};
    std::shared_ptr<ZSlate::SCheckBox> check;
    std::shared_ptr<ZSlate::STextBlock> label;  // used for read-only / display rows

    // When set, SyncBindings calls this instead of the offset-based path. Used by
    // custom component sections (e.g. BaseRenderer) whose data is not a flat field.
    std::function<void()> custom_sync;
};

// Unity-style per-component enable checkbox on the section header row.
struct ComponentEnableBinding
{
    size_t component_index {0};
    std::shared_ptr<ZSlate::SCheckBox> check;
    std::shared_ptr<ZSlate::STextBlock> title;
};
}  // namespace ZSlateInspectorDetail

// P5 (extended): a ZSlate-rendered, dockable inspector that shows the selected
// GameObject's name, a hand-built Transform section, AND every other component's
// serialized fields generically (mirroring EditorSerializedFieldDrawer's TypeTree
// walk, but emitting ZSlate widgets). This is the only Inspector now -- the legacy
// ImGui Inspector was removed.
class ZSlateInspectorWindow : public EditorWindow
{
public:
    explicit ZSlateInspectorWindow(EditorUI* editor_ui);
    void OnGUI() override;
    bool SupportsNativeHosting() const override { return true; }

    // Which native asset inspector (if any) the current selection maps to. All
    // supported asset types build a native ZSlate widget tree; unsupported types
    // show the generic .zasset inspector (no immediate-mode ImGui drawer remains).
    enum class NativeAssetKind
    {
        None,
        Font,
        Generic,
        Texture,
        Material,
        DataTable,
        Shader,
    };

private:
    void BuildForObject(float scale);
    void BuildEmpty(float scale);
    // Native ZSlate asset inspectors (replacing the immediate-mode ImGui drawers).
    void BuildFontAsset(const std::filesystem::path& asset_path, float scale);
    void BuildGenericAsset(const std::filesystem::path& asset_path, float scale);
    void BuildTextureAsset(const std::filesystem::path& asset_path, float scale);
    void BuildMaterialAsset(const std::filesystem::path& asset_path, float scale);
    // `asset_type` is the resolved DataTableBase-derived Type* (from ResolveDataTableType).
    void BuildDataTableAsset(const std::filesystem::path& asset_path, const Type* asset_type, float scale);
    void BuildShaderAsset(const std::filesystem::path& asset_path, float scale);

    // ASTC texture preview (Phase 6). Decompresses ASTC-compressed Texture2D
    // and displays the RGBA8 result in the inspector.
    void BuildASTCPreview(const std::filesystem::path& asset_path, float scale);

    // Popup-backed combo button (used by native asset inspectors). Returns an
    // SButton showing `current_label`; clicking raises m_Popup with `options`,
    // and choosing item i invokes on_select(i) + flags a rebuild.
    std::shared_ptr<ZSlate::SWidget> MakeComboButton(const std::string& current_label,
                                                     std::vector<std::string> options,
                                                     std::function<void(int)> on_select,
                                                     float scale);
    void SyncFromSelection();
    void SyncBindings();

    std::shared_ptr<ZSlate::SWidget> m_Root;
    std::shared_ptr<ZSlate::SCheckBox> m_GoActiveCheck;
    std::vector<ZSlateInspectorDetail::ComponentEnableBinding> m_ComponentEnableBindings;
    std::shared_ptr<ZSlate::STextBlock> m_NameLabel;
    std::array<std::shared_ptr<ZSlate::SEditableTextBox>, 3> m_PositionFields {};
    std::array<std::shared_ptr<ZSlate::SEditableTextBox>, 3> m_RotationFields {};
    std::array<std::shared_ptr<ZSlate::SEditableTextBox>, 3> m_ScaleFields {};
    std::vector<ZSlateInspectorDetail::FieldBinding> m_Bindings;

    ZSlate::SlateInputRouter m_Input;

    float m_BuiltScale {-1.0f};
    GObjectID m_BuiltObjectId {k_invalid_gobject_id};
    bool m_BuiltHasObject {false};
    size_t m_BuiltComponentCount {0};
    bool m_BuiltNativeAsset {false};
    NativeAssetKind m_BuiltNativeKind {NativeAssetKind::None};
    std::string m_BuiltAssetPath;

    // Resolved DataTableBase-derived Type* for the current DataTable selection
    // (set in OnGUI before the build dispatch; consumed by BuildDataTableAsset).
    const Type* m_DataTableType {nullptr};

    // Combo dropdowns raised by native asset inspectors (texture/material/...).
    // Rendered as a foreground overlay after the canvas, mirroring the
    // hierarchy/project windows. An item action sets m_ForceRebuild so the
    // retained widget tree refreshes its combo labels / conditional rows.
    ZSlate::ZSlatePopupMenu m_Popup;
    bool m_ForceRebuild {false};
};
