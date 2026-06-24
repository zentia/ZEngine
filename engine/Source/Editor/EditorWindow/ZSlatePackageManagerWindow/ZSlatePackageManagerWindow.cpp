#include "ZSlatePackageManagerWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/PackageManager/PackageManager.h"
#include "Editor/PackageManager/PackageTypes.h"
#include "Editor/Platform/Interface/EditorUtility.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"      // native input / metrics
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"  // native RHI backend
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/STextBlock.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace ZSlate;

namespace
{
const ZSlate::UIColor kPanelColor(0.10f, 0.10f, 0.12f, 1.0f);
const ZSlate::UIColor kRowHeaderColor(0.78f, 0.80f, 0.85f, 1.0f);
const ZSlate::UIColor kValueColor(0.86f, 0.88f, 0.92f, 1.0f);
const ZSlate::UIColor kDimColor(0.55f, 0.57f, 0.62f, 1.0f);
const ZSlate::UIColor kErrorColor(1.0f, 0.40f, 0.40f, 1.0f);
const ZSlate::UIColor kLabelColor(0.82f, 0.84f, 0.88f, 1.0f);

// Column widths (unscaled px) shared between the header row and data rows so the
// columns line up; the Path column fills the remaining width.
constexpr float kColName = 200.0f;
constexpr float kColVersion = 80.0f;
constexpr float kColSource = 90.0f;
constexpr float kColDepth = 50.0f;

std::shared_ptr<STextBlock> MakeText(const std::string& text, float font_size, const ZSlate::UIColor& color)
{
    auto t = std::make_shared<STextBlock>();
    t->Text = text;
    t->FontSize = font_size;
    t->Color = color;
    t->Alignment = ZSlate::TextAnchor::MiddleLeft;
    return t;
}

// A fixed-width column cell wrapping a text block.
std::shared_ptr<SBox> MakeCell(const std::string& text, float width, float font, const ZSlate::UIColor& color)
{
    auto box = std::make_shared<SBox>();
    box->WidthOverride = width;
    box->VAlign = EVerticalAlignment::Center;
    box->SetContent(MakeText(text, font, color));
    return box;
}

// One package/header row: Name | Version | Source | Depth | Path (fill).
std::shared_ptr<SHorizontalBox> MakeRow(const std::string& name,
                                        const std::string& version,
                                        const std::string& source,
                                        const std::string& depth,
                                        const std::string& path,
                                        float scale,
                                        const ZSlate::UIColor& color)
{
    const float font = 13.0f * scale;
    auto row = std::make_shared<SHorizontalBox>();
    row->AddSlot(MakeCell(name, kColName * scale, font, color)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    row->AddSlot(MakeCell(version, kColVersion * scale, font, color)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    row->AddSlot(MakeCell(source, kColSource * scale, font, color)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    row->AddSlot(MakeCell(depth, kColDepth * scale, font, color)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    row->AddSlot(MakeText(path, font, color)).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
    return row;
}

bool ContainsFilter(const std::string& haystack, const std::string& filter)
{
    if (filter.empty())
        return true;
    return haystack.find(filter) != std::string::npos;
}
}  // namespace

ZSlatePackageManagerWindow::ZSlatePackageManagerWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kPackageManager)
{
    m_Open = false;
}

size_t ZSlatePackageManagerWindow::ComputeResolveSignature(PackageManager& pm) const
{
    // Cheap content fingerprint so a re-resolve that changes the set (not just the
    // count) still forces a table rebuild.
    std::vector<const ResolvedPackage*> packages = pm.GetResolvedPackages();
    size_t hash = packages.size();
    const std::hash<std::string> hasher;
    for (const ResolvedPackage* pkg : packages)
    {
        if (pkg == nullptr)
            continue;
        hash ^= hasher(pkg->name) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= hasher(pkg->version) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= static_cast<size_t>(pkg->depth) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    }
    return hash;
}

void ZSlatePackageManagerWindow::RebuildTable(PackageManager& pm)
{
    if (!m_PackageList)
        return;
    m_PackageList->ClearChildren();

    const float scale = m_BuiltScale > 0.0f ? m_BuiltScale : 1.0f;

    std::vector<const ResolvedPackage*> packages = pm.GetResolvedPackages();
    std::sort(packages.begin(), packages.end(), [this](const ResolvedPackage* a, const ResolvedPackage* b) {
        if (a->depth != b->depth)
            return a->depth < b->depth;
        return m_SortByName ? a->name < b->name : false;
    });

    size_t shown = 0;
    for (const ResolvedPackage* pkg : packages)
    {
        if (pkg == nullptr)
            continue;
        const std::string path_str = pkg->root_path.generic_string();
        if (!ContainsFilter(pkg->name, m_Filter) && !ContainsFilter(path_str, m_Filter))
            continue;

        m_PackageList->AddChild(MakeRow(pkg->name,
                                        pkg->version,
                                        PackageSourceToString(pkg->source),
                                        std::to_string(pkg->depth),
                                        path_str,
                                        scale,
                                        kValueColor));
        ++shown;
    }

    if (shown == 0)
    {
        m_PackageList->AddChild(
            MakeText("No resolved packages. Click Re-resolve or check [ZPackage] logs.", 13.0f * scale, kDimColor));
    }
}

void ZSlatePackageManagerWindow::BuildLayout(float scale)
{
    const float font = 13.0f * scale;

    auto root = std::make_shared<SBorder>();
    root->BackgroundColor = kPanelColor;
    root->Padding = ZSlate::FMargin(6.0f * scale, 6.0f * scale);
    root->HAlign = EHorizontalAlignment::Fill;
    root->VAlign = EVerticalAlignment::Fill;

    auto column = std::make_shared<SVerticalBox>();

    // ---- Toolbar: Re-resolve | Open manifest | Open lock file ---------------
    auto toolbar = std::make_shared<SHorizontalBox>();

    auto make_button = [&](const char* label, std::function<void()> on_click) {
        auto btn = std::make_shared<SButton>();
        btn->Padding = ZSlate::FMargin(8.0f * scale, 3.0f * scale);
        btn->SetContent(MakeText(label, font, kLabelColor));
        btn->OnClicked = std::move(on_click);
        toolbar->AddSlot(btn).AutoSize().SetVAlign(EVerticalAlignment::Center);
        toolbar->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(6.0f * scale, 0.0f))).AutoSize();
    };

    make_button("Re-resolve", [this]() {
        if (auto pm = GET_SYSTEM(PackageManager))
        {
            pm->Resolve(true);
            m_TableDirty = true;
        }
    });
    make_button("Open manifest", [this]() {
        if (auto pm = GET_SYSTEM(PackageManager))
        {
            const auto path = pm->GetProjectManifestPath();
            if (!path.empty())
                EditorUtility::OpenInExternalEditor(path.generic_string().c_str());
        }
    });
    make_button("Open lock file", [this]() {
        if (auto pm = GET_SYSTEM(PackageManager))
        {
            const auto path = pm->GetProjectLockPath();
            if (!path.empty())
                EditorUtility::OpenInExternalEditor(path.generic_string().c_str());
        }
    });

    m_ErrorText = MakeText("", font, kErrorColor);
    toolbar->AddSlot(m_ErrorText).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);

    column->AddSlot(toolbar).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Manifest / lock paths ----------------------------------------------
    auto manifest_row = std::make_shared<SHorizontalBox>();
    manifest_row->AddSlot(MakeText("Project manifest:", font, kDimColor))
        .AutoSize()
        .SetVAlign(EVerticalAlignment::Center);
    manifest_row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(6.0f * scale, 0.0f))).AutoSize();
    m_ManifestText = MakeText("(none)", font, kValueColor);
    manifest_row->AddSlot(m_ManifestText).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
    column->AddSlot(manifest_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));

    auto lock_row = std::make_shared<SHorizontalBox>();
    lock_row->AddSlot(MakeText("Lock file:", font, kDimColor)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    lock_row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(6.0f * scale, 0.0f))).AutoSize();
    m_LockText = MakeText("(not generated yet)", font, kValueColor);
    lock_row->AddSlot(m_LockText).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
    column->AddSlot(lock_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Filter + sort ------------------------------------------------------
    auto filter_row = std::make_shared<SHorizontalBox>();
    m_FilterBox = std::make_shared<SEditableTextBox>();
    m_FilterBox->FontSize = font;
    m_FilterBox->HintText = "Filter packages...";
    m_FilterBox->MinWidth = 160.0f * scale;
    m_FilterBox->OnTextChanged = [this](const std::string& t) {
        m_Filter = t;
        m_TableDirty = true;
    };
    filter_row->AddSlot(m_FilterBox).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);

    filter_row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(8.0f * scale, 0.0f))).AutoSize();
    auto sort_cb = std::make_shared<SCheckBox>();
    sort_cb->Checked = m_SortByName;
    sort_cb->BoxSize = 16.0f * scale;
    sort_cb->OnCheckStateChanged = [this](bool b) {
        m_SortByName = b;
        m_TableDirty = true;
    };
    filter_row->AddSlot(sort_cb).AutoSize().SetVAlign(EVerticalAlignment::Center);
    filter_row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(3.0f * scale, 0.0f))).AutoSize();
    filter_row->AddSlot(MakeText("Sort by name", font, kLabelColor)).AutoSize().SetVAlign(EVerticalAlignment::Center);

    column->AddSlot(filter_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

    // ---- Column header (fixed above the scrolling rows) ---------------------
    column->AddSlot(MakeRow("Name", "Version", "Source", "Depth", "Path", scale, kRowHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));

    // ---- Package rows -------------------------------------------------------
    auto list_border = std::make_shared<SBorder>();
    list_border->BackgroundColor = ZSlate::UIColor(0.07f, 0.07f, 0.09f, 1.0f);
    list_border->Padding = ZSlate::FMargin(4.0f * scale, 4.0f * scale);
    list_border->HAlign = EHorizontalAlignment::Fill;
    list_border->VAlign = EVerticalAlignment::Fill;

    m_PackageList = std::make_shared<SScrollBox>();
    list_border->SetContent(m_PackageList);
    column->AddSlot(list_border).Fill();

    root->SetContent(column);
    m_Root = root;
}

void ZSlatePackageManagerWindow::OnGUI()
{
    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    auto pm = GET_SYSTEM(PackageManager);
    const auto project = GET_SYSTEM(ProjectInfo);
    const bool ready = pm != nullptr && project != nullptr && !project->project_path.empty();

    if (m_Root == nullptr || ui_scale != m_BuiltScale)
    {
        m_BuiltScale = ui_scale;
        BuildLayout(ui_scale);
        m_TableDirty = true;
        m_LastSignature = 0;
        m_Input.Reset();
    }

    if (ready)
    {
        // Keep the path / error labels current (cheap; paths are project-stable).
        const auto manifest = pm->GetProjectManifestPath();
        const auto lock = pm->GetProjectLockPath();
        if (m_ManifestText)
            m_ManifestText->Text = manifest.empty() ? "(none)" : manifest.generic_string();
        if (m_LockText)
            m_LockText->Text = lock.empty() ? "(not generated yet)" : lock.generic_string();
        if (m_ErrorText)
        {
            const std::string err = pm->GetLastResolveError();
            m_ErrorText->Text = err.empty() ? "" : ("Last error: " + err);
        }

        const size_t signature = ComputeResolveSignature(*pm);
        if (m_TableDirty || signature != m_LastSignature)
        {
            RebuildTable(*pm);
            m_LastSignature = signature;
            m_TableDirty = false;
        }
    }
    else if (m_TableDirty)
    {
        if (m_PackageList)
        {
            m_PackageList->ClearChildren();
            m_PackageList->AddChild(MakeText("Open a project to manage packages.", 13.0f * ui_scale, kDimColor));
        }
        m_TableDirty = false;
    }

    // ---- Paint --------------------------------------------------------------
    // P10c: native-host panels source their leaf rect from EditorView::NativeRect()
    // (no ImGui::Begin / item to probe); otherwise use the ImGui content region.
    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const ZSlate::UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, avail.y));

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);
        m_Root->CacheDesiredSize();
        FPaintContext ctx;
        ctx.Renderer = &renderer;
        ctx.LayerId = 0;
        renderer.pushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.popClipRect();
    }

    // ---- Input --------------------------------------------------------------
    // P11a: input / hover / wheel / keyboard all come from the GLFW-backed EditorSlateHost.
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel);

    if (m_Input.HasKeyboardFocus())
    {
        for (unsigned int cp : host.GetCharsThisFrame())
            m_Input.ProcessChar(cp);
        // UE pattern: route ALL keys to the focused widget
        for (EKey key : host.GetKeysThisFrame())
            m_Input.ProcessKey(key);
    }
}
