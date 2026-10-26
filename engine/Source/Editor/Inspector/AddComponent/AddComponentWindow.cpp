#include "AddComponentWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorUI/EditorSelection.h"
#include "Editor/Undo/EditorUndo.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"       // native input bus
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"   // native RHI backend
#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Text/STextBlock.h"

#include <algorithm>
#include <cctype>
#include <cstring>

using namespace ZSlate;

namespace
{
const ZSlate::UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const ZSlate::UIColor kListColor(0.07f, 0.07f, 0.09f, 1.0f);
const ZSlate::UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
const ZSlate::UIColor kLabelColor(0.82f, 0.84f, 0.88f, 1.0f);
const ZSlate::UIColor kCategoryColor(0.52f, 0.62f, 0.78f, 1.0f);
const ZSlate::UIColor kRowHoverColor(0.18f, 0.18f, 0.22f, 1.0f);
const ZSlate::UIColor kRowPressedColor(0.25f, 0.30f, 0.40f, 0.75f);

// ---- Curated metadata -----------------------------------------------------------
// The catalog itself comes from the reflection registry; this table only decides
// where each entry appears in the list. A component missing from it is not lost --
// it just lands in the "Component" bucket.
struct CategoryMapping
{
    const char* type_name;
    const char* category;
};

const CategoryMapping kCategoryMap[] = {
    {"MeshRenderer", "Rendering"},
    {"SkinMeshRenderer", "Rendering"},
    {"SpineRenderer", "Rendering"},
    {"CameraComponent", "Rendering"},
    {"LightComponent", "Rendering"},
    {"ParticleComponent", "Rendering"},
    {"RigidBodyComponent", "Physics"},
    {"AudioSourceComponent", "Audio"},
    {"AudioListenerComponent", "Audio"},
    {"AnimationComponent", "Animation"},
    {"TimelineComponent", "Animation"},
    {"MotorComponent", "Navigation"},
    {"AIControllerComponent", "AI"},
    {"AIPerceptionComponent", "AI"},
    {"TypeScriptComponent", "Scripts"},
};

// Types that must never be offered, even though reflection lists them as concrete.
//
//   Transform          every GameObject has exactly one; a second one would make
//                      tryGetComponent(Transform) ambiguous.
//   BaseRenderer       shared base of MeshRenderer / SkinMeshRenderer. It is not
//                      marked ZCLASS(Abstract) and has no pure virtual, so
//                      reflection reports it as instantiable -- but a bare
//                      BaseRenderer renders nothing.
//   PrefabRefComponent prefab-instance bookkeeping, authored by the prefab
//                      pipeline, not by hand.
const char* const kHiddenTypes[] = {
    "Component",
    "Behaviour",
    "Transform",
    "BaseRenderer",
    "PrefabRefComponent",
};

std::vector<AddComponentEntry> g_Entries;
bool g_EntriesBuilt {false};

bool EqualsIgnoreCase(const char* lhs, const char* rhs)
{
    return std::strcmp(lhs, rhs) == 0;
}

bool ContainsIgnoreCase(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0')
    {
        return true;
    }
    const std::size_t hay_len = std::strlen(haystack);
    const std::size_t needle_len = std::strlen(needle);
    if (needle_len > hay_len)
    {
        return false;
    }
    for (std::size_t i = 0; i + needle_len <= hay_len; ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < needle_len; ++j)
        {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j])))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = ZSlate::TextAnchor::MiddleLeft;
    return t;
}
}  // namespace

// =====================================================================================
// AddComponentCatalog
// =====================================================================================

const char* AddComponentCatalog::CategoryFor(const Type* type)
{
    if (type == nullptr || type->GetName() == nullptr)
    {
        return "Component";
    }
    for (const CategoryMapping& mapping : kCategoryMap)
    {
        if (EqualsIgnoreCase(type->GetName(), mapping.type_name))
        {
            return mapping.category;
        }
    }
    return "Component";
}

bool AddComponentCatalog::IsHidden(const Type* type)
{
    if (type == nullptr || type->GetName() == nullptr)
    {
        return true;
    }
    for (const char* hidden : kHiddenTypes)
    {
        if (EqualsIgnoreCase(type->GetName(), hidden))
        {
            return true;
        }
    }
    return false;
}

const std::vector<AddComponentEntry>& AddComponentCatalog::GetEntries()
{
    if (g_EntriesBuilt)
    {
        return g_Entries;
    }
    g_EntriesBuilt = true;

    // Everything derived from Component, concrete only. FindAllDerivedTypes starts at
    // Component's own type index, so Component itself is in the list -- it is abstract
    // and therefore dropped by the kOnlyNonAbstract filter.
    std::vector<const Type*> types;
    TypeManager::GetInstance().FindAllDerivedTypes(TypeOf<Component>(), types, /*onlyNonAbstract=*/true);

    g_Entries.clear();
    g_Entries.reserve(types.size());
    for (const Type* type : types)
    {
        if (type == nullptr || type->GetName() == nullptr)
        {
            continue;
        }
        if (IsHidden(type))
        {
            continue;
        }
        // No factory means "registered for serialization but not constructible"
        // (or an abstract type whose ZCLASS(Abstract) specifier was missed).
        if (type->GetFactory() == nullptr)
        {
            continue;
        }

        AddComponentEntry entry;
        entry.type = type;
        entry.display_name = type->GetName();
        entry.category = CategoryFor(type);
        g_Entries.push_back(entry);
    }

    std::sort(g_Entries.begin(), g_Entries.end(), [](const AddComponentEntry& lhs, const AddComponentEntry& rhs) {
        const int by_category = std::strcmp(lhs.category, rhs.category);
        if (by_category != 0)
        {
            return by_category < 0;
        }
        return std::strcmp(lhs.display_name, rhs.display_name) < 0;
    });

    return g_Entries;
}

void AddComponentCatalog::Invalidate()
{
    g_Entries.clear();
    g_EntriesBuilt = false;
}

std::vector<int> AddComponentCatalog::Filter(const std::string& query)
{
    const std::vector<AddComponentEntry>& entries = GetEntries();
    std::vector<int> result;
    result.reserve(entries.size());

    if (query.empty())
    {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        {
            result.push_back(i);
        }
        return result;
    }

    const char* needle = query.c_str();
    for (int i = 0; i < static_cast<int>(entries.size()); ++i)
    {
        if (ContainsIgnoreCase(entries[static_cast<std::size_t>(i)].display_name, needle) ||
            ContainsIgnoreCase(entries[static_cast<std::size_t>(i)].category, needle))
        {
            result.push_back(i);
        }
    }
    return result;
}

// =====================================================================================
// AddComponentWindow
// =====================================================================================

AddComponentWindow::AddComponentWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kAddComponent)
{
}

Component* AddComponentWindow::AddComponentTo(GameObject* host, const Type* component_type)
{
    if (host == nullptr || component_type == nullptr)
    {
        return nullptr;
    }
    if (component_type->GetFactory() == nullptr)
    {
        LOG_WARNING(ZEditor, "AddComponentTo: type has no factory (abstract?)");
        return nullptr;
    }

    ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        return nullptr;
    }

    // Produce() -> `new T()` via the generated ProduceHelper, then a real (negative)
    // heap InstanceID. MemoryManager::CreateObject would leave m_InstanceID at 0,
    // which makes the component invisible to PPtr / ImmediatePtr and to the scene
    // serializer.
    Object* raw = object_manager->Produce(component_type, 0);
    if (raw == nullptr)
    {
        return nullptr;
    }
    object_manager->AllocateAndAssignInstanceID(raw);

    Component* component = Cast<Component>(raw);
    if (component == nullptr)
    {
        object_manager->Destroy(raw);
        return nullptr;
    }

    const std::string title = std::string("Add ") + component_type->GetName();
    EditorUndo::ScopedTransaction transaction(title);

    // PostLoadResource first: it binds m_ParentObject, and subclasses resolve their
    // asset paths / register with their subsystem from it. Several of them
    // (AnimationComponent, TypeScriptComponent, AudioSourceComponent) crash or
    // silently no-op when it has not run before the component is reachable.
    component->PostLoadResource(host);
    host->addComponent(component);

    EditorUndo::RegisterCreatedComponent(component, host->GetID(), title);
    LOG_INFO(ZEditor, "AddComponent: '{}' added to GameObject '{}'", component_type->GetName(), host->GetName().c_str());
    return component;
}

GameObject* AddComponentWindow::ResolveTarget() const
{
    return EditorSelection::GetActiveGameObject().lock().get();
}

void AddComponentWindow::Commit(int entry_index)
{
    GameObject* target = ResolveTarget();
    if (target == nullptr)
    {
        return;
    }

    const std::vector<AddComponentEntry>& entries = AddComponentCatalog::GetEntries();
    if (entry_index < 0 || static_cast<std::size_t>(entry_index) >= entries.size())
    {
        return;
    }
    const Type* type = entries[static_cast<std::size_t>(entry_index)].type;
    if (type == nullptr)
    {
        return;
    }
    AddComponentTo(target, type);
}

void AddComponentWindow::BuildLayout(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = ZSlate::FMargin(4.0f * scale, 4.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Fill;

    auto column = std::make_shared<SVerticalBox>();

    // ---- Header: which GameObject the click will land on ----------------------
    m_HintText = MakeText("Select a GameObject to add components to.", font, kDimColor);
    column->AddSlot(m_HintText).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Search ---------------------------------------------------------------
    m_SearchBox = std::make_shared<SEditableTextBox>();
    m_SearchBox->FontSize = font;
    m_SearchBox->HintText = "Search components...";
    m_SearchBox->MinWidth = 120.0f * scale;
    m_SearchBox->OnTextChanged = [this](const std::string& t) {
        m_Search = t;
        m_FilterDirty = true;
    };
    column->AddSlot(m_SearchBox).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Component list -------------------------------------------------------
    auto list_border = std::make_shared<SBorder>();
    list_border->BackgroundColor = kListColor;
    list_border->Padding = ZSlate::FMargin(4.0f * scale, 4.0f * scale);
    list_border->HAlign = EHorizontalAlignment::Fill;
    list_border->VAlign = EVerticalAlignment::Fill;

    m_List = std::make_shared<SScrollBox>();
    list_border->SetContent(m_List);
    column->AddSlot(list_border).Fill();

    root->SetContent(column);
    m_Root = root;
}

void AddComponentWindow::RebuildRows()
{
    if (m_List == nullptr)
    {
        return;
    }
    // Rows are destroyed below; drop stale hover pointers but keep keyboard focus on
    // the search box (Reset() would clear it on every keystroke).
    m_Input.ClearHoverPath();
    m_List->ClearSlots();

    const float font = 13.0f * (m_BuiltScale > 0.0f ? m_BuiltScale : 1.0f);
    const std::vector<AddComponentEntry>& entries = AddComponentCatalog::GetEntries();

    for (int index : m_Filtered)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
        {
            continue;
        }
        const AddComponentEntry& entry = entries[static_cast<std::size_t>(index)];

        // "Rendering / Mesh Renderer" -- the category prefix is what makes the
        // grouping readable without nested headers the list would have to re-layout.
        std::string label = std::string(entry.category) + " / " + entry.display_name;

        auto row = std::make_shared<SButton>();
        row->Padding = ZSlate::FMargin(6.0f * (m_BuiltScale > 0.0f ? m_BuiltScale : 1.0f), 2.0f);
        row->NormalColor = ZSlate::UIColor(0.0f, 0.0f, 0.0f, 0.0f);
        row->HoverColor = kRowHoverColor;
        row->PressedColor = kRowPressedColor;
        row->HAlign = EHorizontalAlignment::Fill;
        row->SetContent(MakeText(label, font, kLabelColor));
        row->OnClicked = [this, index]() { Commit(index); };
        m_List->AddChild(row);
    }
}

void AddComponentWindow::OnGUI()
{
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
    {
        ui_scale = 1.0f;
    }

    if (m_Root == nullptr || ui_scale != m_BuiltScale)
    {
        m_BuiltScale = ui_scale;
        BuildLayout(ui_scale);
        m_FilterDirty = true;
        m_Input.Reset();
    }

    if (m_FilterDirty)
    {
        m_Filtered = AddComponentCatalog::Filter(m_Search);
        RebuildRows();
        m_FilterDirty = false;
    }

    if (m_HintText != nullptr)
    {
        GameObject* target = ResolveTarget();
        if (target == nullptr)
        {
            m_HintText->Text = "Select a GameObject to add components to.";
            m_HintText->Color = kDimColor;
        }
        else
        {
            m_HintText->Text = std::string("Add to: ") + target->GetName().c_str();
            m_HintText->Color = kCategoryColor;
        }
    }

    // ---- Paint ------------------------------------------------------------------
    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
    {
        avail.x = 1.0f;
    }
    if (avail.y < 1.0f)
    {
        avail.y = 1.0f;
    }

    const ZSlate::UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, avail.y));

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &overlay;  // ZSlateEditorOverlay implements ISlateRenderer
        ctx.LayerId = 0;
        renderer.PushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.PopClipRect();
    }

    // ---- Input ------------------------------------------------------------------
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const bool right_down = host.IsRightDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel, right_down);
}
