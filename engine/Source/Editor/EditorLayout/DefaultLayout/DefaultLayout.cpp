#include "DefaultLayout.h"

#include "Editor/EditorLayout/EditorLayoutConstants.h"
#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorLayout/ZSlateDock/DockHost.h"
#include "Editor/EditorLayout/ZSlateDock/DockTree.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/EditorWindow/EditorWindow.h"  // close-button: clear EditorWindow::m_Open
#include "Editor/FloatingPanel/FloatingPanelManager.h"  // P6: tab-drag tear-off
#include "Editor/Platform/Interface/EditorUtility.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "ZSlate/Widgets/SBoxPanel.h"        // P11f: native Save Layout dialog
#include "ZSlate/Widgets/SButton.h"          // P11f
#include "ZSlate/Widgets/SEditableTextBox.h"  // P11f
#include "ZSlate/Widgets/SSpacer.h"           // P11f
#include "ZSlate/Widgets/STextBlock.h"        // P11f
#include "Runtime/UI/Render/BatchedUIRenderer.h"  // P7 placeholder / maximized chrome painting

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    using EditorLayoutConstants::kReservedTopHeight;

    // P11a: dock chrome mouse source is the GLFW-backed EditorSlateHost (the
    // transitional r.ZSlate.NativeInput CVar was retired in P10c, so the old
    // ImGui::GetIO() fallbacks were dead and are gone).
    ZSlate::Vector2 ChromeMouse()
    {
        return ZSlate::EditorSlateHost::Get().GetPointerPos();
    }

    bool ChromeLeftDown()
    {
        return ZSlate::EditorSlateHost::Get().IsLeftDown();
    }

    std::string trim(const std::string& value)
    {
        size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        {
            ++begin;
        }

        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        {
            --end;
        }

        return value.substr(begin, end - begin);
    }

    std::string deriveLayoutNameFromPath(const std::filesystem::path& file_path)
    {
        std::string file_name = file_path.filename().string();
        constexpr const char* suffix = ".zlayout.json";
        if (file_name.size() > std::char_traits<char>::length(suffix) &&
            file_name.ends_with(suffix))
        {
            return file_name.substr(0, file_name.size() - std::char_traits<char>::length(suffix));
        }

        return file_path.stem().string();
    }

}  // namespace

DefaultLayout::DefaultLayout(EditorUI* editor_ui)
    : EditorLayout(editor_ui)
{
    std::fill(m_LayoutNameBuffer.begin(), m_LayoutNameBuffer.end(), '\0');
}

DefaultLayout::~DefaultLayout()
{
    // Best-effort flush so a clean shutdown captures the last sub-second of edits the
    // debounced autosave may not have written yet. Guarded: only if the native tree was
    // actually used this session, and never throwing out of a destructor.
    try
    {
        if (m_NativeTreeInitialized)
        {
            const std::string json = m_NativeDockTree.SerializeToJson();
            if (!json.empty() && json != m_LastSavedNativeJson)
            {
                SaveNativeSession(json);
            }
        }
    }
    catch (...)
    {
    }
}

void DefaultLayout::OnGUI()
{
    // P9: native dock hosting is the ONLY path -- the ImGui DockSpace, the DockBuilder
    // authoring, and the stale-ini repair are all gone. Apply any queued builtin/snapshot/
    // reset to the native DockTree, then paint the native dock over the content region;
    // EditorView::BeginGUI positions each panel into its leaf rect (see QueryNativeDockPanel).
    ApplyPendingLayout();

    // P11d: dock host geometry comes from the native EditorSlateHost display metrics
    // instead of the ImGui "Editor menu" host window (GetMainViewport / GetCursorScreenPos /
    // GetContentRegionAvail). The dock fills the area below the editor chrome (menu bar +
    // playback toolbar = kReservedTopHeight) down to the window's right/bottom edges.
    // NOTE: the old ImGui path inset the right/bottom by the host window's WindowPadding
    // (space_m = 12px), leaving a small transparent gap; the native path fills to the edge.
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const ZSlate::Vector2 disp_pos = host.GetDisplayPos();
    const ZSlate::Vector2 disp_size = host.GetDisplaySize();
    const float reserved = EditorLayoutConstants::kReservedTopHeight;
    const ZSlate::Vector2 native_origin(disp_pos.x, disp_pos.y + reserved);
    const ZSlate::Vector2 dock_host_avail(disp_size.x, disp_size.y - reserved);
    RenderNativeDockPreview(native_origin, dock_host_avail);
}

void DefaultLayout::ResetNativeInteractionState()
{
    // Any structural rebuild frees every node -> drop in-flight drag pointers.
    m_DraggingSplitter = nullptr;
    m_TabDragId.clear();
    m_TabDragActive = false;
    m_DropTargetLeaf = nullptr;
    m_HasDrop = false;
    // A builtin/snapshot/reset replaces the arrangement; do not carry a maximized panel
    // across it (the panel may not even exist in the new tree).
    m_MaximizedPanelId.clear();
}

void DefaultLayout::BuildNativeBuiltin(BuiltinLayoutType type)
{
    using namespace EditorLayoutWindowIds;
    using EditorDock::EDockDir;

    ResetNativeInteractionState();
    EditorDock::DockTree& t = m_NativeDockTree;
    t.Clear();
    EditorDock::DockNode* root = t.Root();

    // Re-find a leaf by a panel that stays in the "center" after each split (Scene is in
    // every builtin's center, so it is the stable anchor). SplitLeaf converts the passed
    // leaf in place into a split, so the old tabs migrate to a child leaf we must relocate.
    auto leaf = [&](const char* anchor) { return t.FindPanelLeaf(anchor); };

    switch (type)
    {
        case BuiltinLayoutType::TwoByThree:
        {
            // Scene center; Hierarchy left; Inspector/Preview right; Project/Console/Timeline bottom.
            t.AddTab(*root, kScene);
            if (auto* c = leaf(kScene)) { auto* r = t.SplitLeaf(*c, EDockDir::Right, kInspector, 0.30f); t.AddTab(*r, kPreview); }
            if (auto* c = leaf(kScene)) t.SplitLeaf(*c, EDockDir::Left, kHierarchy, 0.22f);
            if (auto* c = leaf(kScene)) { auto* b = t.SplitLeaf(*c, EDockDir::Bottom, kContentBrowser, 0.32f); t.AddTab(*b, kConsole); t.AddTab(*b, kTimeline); }
            break;
        }
        case BuiltinLayoutType::FourSplit:
        {
            // Right Inspector/Preview; bottom Project/Console/Timeline; top-left Hierarchy, top-center Scene/Game.
            t.AddTab(*root, kScene); t.AddTab(*root, kGame);
            if (auto* c = leaf(kScene)) { auto* r = t.SplitLeaf(*c, EDockDir::Right, kInspector, 0.28f); t.AddTab(*r, kPreview); }
            if (auto* c = leaf(kScene)) { auto* b = t.SplitLeaf(*c, EDockDir::Bottom, kContentBrowser, 0.42f); t.AddTab(*b, kConsole); t.AddTab(*b, kTimeline); }
            if (auto* c = leaf(kScene)) t.SplitLeaf(*c, EDockDir::Left, kHierarchy, 0.25f);
            break;
        }
        case BuiltinLayoutType::Tall:
        {
            // Narrow left column (Hierarchy over Project/Console); center Scene/Game over Timeline; right Inspector/Preview.
            t.AddTab(*root, kScene); t.AddTab(*root, kGame);
            if (auto* c = leaf(kScene)) { auto* r = t.SplitLeaf(*c, EDockDir::Right, kInspector, 0.25f); t.AddTab(*r, kPreview); }
            if (auto* c = leaf(kScene))
            {
                auto* l = t.SplitLeaf(*c, EDockDir::Left, kHierarchy, 0.20f);
                if (l) { auto* lb = t.SplitLeaf(*l, EDockDir::Bottom, kContentBrowser, 0.45f); t.AddTab(*lb, kConsole); }
            }
            if (auto* c = leaf(kScene)) { auto* b = t.SplitLeaf(*c, EDockDir::Bottom, kTimeline, 0.28f); t.AddTab(*b, kAnimation); t.AddTab(*b, kBlueprint); }
            break;
        }
        case BuiltinLayoutType::Wide:
        {
            // Wide bottom bar (Project/Console/Timeline); top Hierarchy | Scene/Game | Inspector/Preview.
            t.AddTab(*root, kScene); t.AddTab(*root, kGame);
            if (auto* c = leaf(kScene)) { auto* b = t.SplitLeaf(*c, EDockDir::Bottom, kContentBrowser, 0.35f); t.AddTab(*b, kConsole); t.AddTab(*b, kTimeline); }
            if (auto* c = leaf(kScene)) { auto* r = t.SplitLeaf(*c, EDockDir::Right, kInspector, 0.25f); t.AddTab(*r, kPreview); }
            if (auto* c = leaf(kScene)) t.SplitLeaf(*c, EDockDir::Left, kHierarchy, 0.20f);
            break;
        }
        case BuiltinLayoutType::Default:
        case BuiltinLayoutType::Unknown:
        default:
        {
            // Hierarchy left; Scene/Game center-top; Project/Console/PackageManager center-bottom;
            // Inspector/Preview right. Timeline/Blueprint/Animation default-closed (reconcile trims).
            t.AddTab(*root, kScene); t.AddTab(*root, kGame);
            if (auto* c = leaf(kScene)) { auto* r = t.SplitLeaf(*c, EDockDir::Right, kInspector, 0.25f); t.AddTab(*r, kPreview); }
            if (auto* c = leaf(kScene)) t.SplitLeaf(*c, EDockDir::Left, kHierarchy, 0.20f);
            if (auto* c = leaf(kScene)) { auto* b = t.SplitLeaf(*c, EDockDir::Bottom, kContentBrowser, 0.30f); t.AddTab(*b, kConsole); t.AddTab(*b, kPackageManager); }
            break;
        }
    }
}

void DefaultLayout::DockNativePanelSomewhere(const std::string& panel_id)
{
    // Dock a newly-opened panel into the largest-area leaf (Unity-style "appears somewhere
    // reasonable"). NodeRect comes from the previous frame's solve; on a cold tree it is
    // zero for all leaves, so we just fall back to the first leaf / root.
    EditorDock::DockNode* best = nullptr;
    float best_area = -1.0f;
    m_NativeDockTree.ForEachLeaf([&](EditorDock::DockNode& leaf) {
        const float area = leaf.NodeRect.width * leaf.NodeRect.height;
        if (area > best_area)
        {
            best_area = area;
            best = &leaf;
        }
    });
    if (best == nullptr)
        best = m_NativeDockTree.Root();
    if (best != nullptr)
        m_NativeDockTree.AddTab(*best, panel_id);
}

void DefaultLayout::ReconcileNativeTreeWithOpenWindows()
{
    const std::unordered_map<std::string, bool> states = m_EditorUi->GetEditorWindowOpenStates();
    auto is_open = [&](const std::string& id) {
        const auto iter = states.find(id);
        return iter != states.end() && iter->second;
    };

    // Remove panels that are no longer open (collapses emptied leaves by promoting siblings).
    for (const std::string& id : m_NativeDockTree.CollectPanelIds())
    {
        if (!is_open(id))
            m_NativeDockTree.RemovePanel(id);
    }

    // Add newly-opened panels that are not docked yet. Floating (torn-off) panels
    // are deliberately NOT in the tree and must not be re-docked here -- they are
    // painted in their own OS window until re-docked (which clears the mark).
    for (const auto& [id, open] : states)
    {
        if (open && !m_NativeDockTree.HasPanel(id) && m_FloatingPanels.find(id) == m_FloatingPanels.end())
            DockNativePanelSomewhere(id);
    }
}

void DefaultLayout::SetPanelFloating(const char* title, bool floating)
{
    if (title == nullptr)
        return;
    const std::string id(title);
    if (floating)
    {
        m_FloatingPanels.insert(id);
        // Pull it out of the docked tree so the remaining leaves reflow to fill
        // the gap (RemovePanel promotes siblings when a leaf empties).
        if (m_NativeDockTree.HasPanel(id))
            m_NativeDockTree.RemovePanel(id);
    }
    else
    {
        m_FloatingPanels.erase(id);
        // Next ReconcileNativeTreeWithOpenWindows re-docks it (it is still open).
    }
}

bool DefaultLayout::IsPanelFloating(const char* title) const
{
    return title != nullptr && m_FloatingPanels.find(std::string(title)) != m_FloatingPanels.end();
}

void DefaultLayout::BeginFloatingPanelRender(const char* title, float x, float y, float width, float height)
{
    m_FloatingRenderTitle = (title != nullptr) ? title : "";
    m_FloatingRenderRect = ZSlate::UIRect(x, y, width, height);
}

void DefaultLayout::EndFloatingPanelRender()
{
    m_FloatingRenderTitle.clear();
}

void DefaultLayout::EnsureNativeDockTree()
{
    // First native frame of the session: restore the saved tree. This runs even if the
    // startup forced-Default (ApplyBuiltinLayout in OnGUI) already built one, so a returning
    // user's manual arrangement wins over the cold Default. An explicit builtin/snapshot the
    // user queued this run is applied via ApplyPendingLayout BEFORE we get here and changes
    // m_CurrentLayoutName off "Default", so it is not clobbered by the restore.
    if (!m_NativeSessionRestoreAttempted)
    {
        m_NativeSessionRestoreAttempted = true;
        if ((!m_NativeTreeInitialized || m_CurrentLayoutName == "Default") && LoadNativeSession())
        {
            m_NativeTreeInitialized = true;
        }
    }

    if (!m_NativeTreeInitialized)
    {
        BuildNativeBuiltin(BuiltinLayoutType::Default);
        m_NativeTreeInitialized = true;
    }
    ReconcileNativeTreeWithOpenWindows();
}

void DefaultLayout::RenderNativeDockPreview(const ZSlate::Vector2& origin, const ZSlate::Vector2& size)
{
    if (size.x <= 1.0f || size.y <= 1.0f)
        return;

    EnsureNativeDockTree();

    // DPI scale: ImGui's font size scales with the global UI scale; 16px is our 1.0x
    // design baseline. Clamp to a sane floor so a degenerate font never zeroes metrics.
    // (P7 DPI re-layout: every metric below derives from ui_scale and geometry is solved
    // fresh each frame, so a DPI / scale change re-lays out the whole dock automatically;
    // split ratios are fractional so they survive the change.)
    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    EditorDock::DockTree::Metrics metrics;
    metrics.SplitterThickness = 4.0f * ui_scale;
    metrics.TabStripHeight = 24.0f * ui_scale;
    metrics.MinNodeExtent = 48.0f * ui_scale;
    m_NativeDockTree.SetMetrics(metrics);

    const ZSlate::UIRect host_rect(origin.x, origin.y, size.x, size.y);
    ZSlate::ZSlateEditorOverlay& overlay = ZSlate::ZSlateEditorOverlay::Get();
    BatchedUIRenderer& renderer = overlay.GetRenderer();

    // P7: a maximize target that was closed / undocked since last frame is no longer valid.
    if (!m_MaximizedPanelId.empty() && !m_NativeDockTree.HasPanel(m_MaximizedPanelId))
        m_MaximizedPanelId.clear();

    // P7: empty-dock placeholder. With no panels there is nothing to solve or host, so
    // paint a centered hint over the dock region instead of leaving it blank.
    if (m_NativeDockTree.Root() == nullptr || m_NativeDockTree.Root()->IsEmptyLeaf())
    {
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZForeground);
        renderer.pushClipRect(host_rect, true);
        renderer.drawQuad(host_rect, m_NativeDockHost.GetStyle().PanelBg);
        renderer.drawText(host_rect,
                          "No panels open -- open one from the Window menu.",
                          14.0f * ui_scale,
                          ZSlate::UIColor(0.52f, 0.55f, 0.60f, 1.0f),
                          ZSlate::TextAnchor::MiddleCenter,
                          ZSlate::TextWrapMode::NoWrap,
                          nullptr);
        renderer.popClipRect();
        return;
    }

    // P7: maximized single-panel view -- fills the whole host, hides all other panels.
    if (!m_MaximizedPanelId.empty())
    {
        // Content-area fill below the maximized panel content (same layering rule as the
        // tiled path); the strip/border/tabs composite above it at the foreground layer.
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZDockBackground);
        renderer.pushClipRect(host_rect, true);
        RenderNativeMaximizedBackground(host_rect, ui_scale);
        renderer.popClipRect();

        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZForeground);
        renderer.pushClipRect(host_rect, true);
        RenderNativeMaximized(host_rect, ui_scale);
        renderer.popClipRect();
        TickNativeSessionAutosave();
        return;
    }

    // 1) Solve geometry, 2) lay out tab rects (so input can hit-test tabs), 3) handle
    // input. A completed drag-to-dock mutates the tree, so re-solve before painting.
    m_NativeDockTree.ComputeGeometry(host_rect);
    m_NativeDockHost.LayoutTabs(m_NativeDockTree, renderer, ui_scale);
    const bool structure_changed = HandleNativeDockInput(host_rect);
    if (structure_changed)
    {
        m_NativeDockTree.ComputeGeometry(host_rect);
        m_NativeDockHost.LayoutTabs(m_NativeDockTree, renderer, ui_scale);
    }

    // Panel-area backgrounds composite BELOW the docked panel content (the fill is
    // opaque and the dock chrome is recorded after the panels each frame, so it must
    // sit on its own sub-panel layer or it would blank every body). Mirrors UE Slate.
    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZDockBackground);
    renderer.pushClipRect(host_rect, true);
    m_NativeDockHost.RenderBackgrounds(m_NativeDockTree, renderer);
    renderer.popClipRect();

    // Composite above the docked panels (same z-order layer as the native menu bar).
    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZForeground);

    renderer.pushClipRect(host_rect, true);
    m_NativeDockHost.Render(m_NativeDockTree, renderer, ui_scale, ChromeMouse());

    // Drag-to-dock drop preview overlay (painted last so it sits above the chrome).
    if (m_TabDragActive && m_HasDrop && m_DropTargetLeaf != nullptr)
    {
        m_NativeDockHost.DrawDropPreview(renderer, *m_DropTargetLeaf, m_DropDir);
    }

    // P6: a floating panel is being dragged over the host -> "dock here" highlight.
    if (m_ExternalDockHint)
    {
        renderer.drawQuad(host_rect, ZSlate::UIColor(0.25f, 0.55f, 0.95f, 0.22f));
        renderer.drawRect(host_rect, ZSlate::UIColor(0.35f, 0.65f, 1.0f, 0.9f), 2.0f);
    }
    renderer.popClipRect();

    // Persist the live arrangement (debounced) so it survives editor restarts.
    TickNativeSessionAutosave();
}

void DefaultLayout::RenderNativeMaximizedBackground(const ZSlate::UIRect& host_rect, float scale)
{
    (void)scale;
    BatchedUIRenderer& renderer = ZSlate::ZSlateEditorOverlay::Get().GetRenderer();
    const EditorDock::DockHost::Style& style = m_NativeDockHost.GetStyle();
    const float strip_h = m_NativeDockTree.GetMetrics().TabStripHeight;

    const ZSlate::UIRect content(host_rect.x, host_rect.y + strip_h, host_rect.width,
                         std::max(1.0f, host_rect.height - strip_h));
    // Cache for QueryNativeDockPanel (the const query places the maximized panel here).
    // Runs before the panel paints this frame, so the rect is fresh on resize.
    m_MaximizedContentRect = content;

    // Opaque content-area fill only; recorded below the panel content.
    // Scene / Game: swapchain already holds the 3D view in this rect (same rule as DockHost).
    if (m_MaximizedPanelId != EditorLayoutWindowIds::kScene &&
        m_MaximizedPanelId != EditorLayoutWindowIds::kGame)
    {
        renderer.drawQuad(content, style.PanelBg);
    }
}

void DefaultLayout::RenderNativeMaximized(const ZSlate::UIRect& host_rect, float scale)
{
    BatchedUIRenderer& renderer = ZSlate::ZSlateEditorOverlay::Get().GetRenderer();
    const EditorDock::DockHost::Style& style = m_NativeDockHost.GetStyle();
    const float font = style.FontSize * scale;
    const float strip_h = m_NativeDockTree.GetMetrics().TabStripHeight;

    const ZSlate::UIRect strip(host_rect.x, host_rect.y, host_rect.width, strip_h);

    // Frame + tab strip band (the content-area fill is RenderNativeMaximizedBackground's,
    // drawn in the kZDockBackground group so the panel content composites above it).
    renderer.drawRect(host_rect, style.BorderColor, 1.0f);
    renderer.drawQuad(strip, style.TabStripBg);

    const ZSlate::Vector2 mouse = ChromeMouse();

    // Single (active) tab for the maximized panel.
    const float pad = style.TabPaddingX * scale;
    const float text_w = renderer.measureText(m_MaximizedPanelId, font, ZSlate::TextWrapMode::NoWrap, 0.0f, nullptr).x;
    const ZSlate::UIRect tab(strip.x, strip.y, text_w + pad * 2.0f, strip.height);
    renderer.drawQuad(tab, style.TabActiveBg);
    renderer.drawQuad(ZSlate::UIRect(tab.x, tab.y, tab.width, std::max(1.0f, 2.0f * scale)), style.ActiveTabAccent);
    renderer.drawText(tab, m_MaximizedPanelId, font, style.TabActiveText, ZSlate::TextAnchor::MiddleCenter,
                      ZSlate::TextWrapMode::NoWrap, nullptr);

    // Restore button at the far right of the strip (a small square = "restore down").
    const float btn_w = strip.height;
    m_RestoreButtonRect = ZSlate::UIRect(strip.x + strip.width - btn_w, strip.y, btn_w, strip.height);
    const bool rb_hover = m_RestoreButtonRect.Contains(mouse);
    renderer.drawQuad(m_RestoreButtonRect, rb_hover ? style.TabHoverBg : style.TabInactiveBg);
    const float ic = btn_w * 0.34f;
    renderer.drawRect(ZSlate::UIRect(m_RestoreButtonRect.x + (btn_w - ic) * 0.5f,
                             m_RestoreButtonRect.y + (btn_w - ic) * 0.5f, ic, ic),
                      rb_hover ? style.TabActiveText : style.TabText, std::max(1.0f, 1.5f * scale));

    // Inline input: restore on button click or a double-click anywhere on the strip.
    // P11a/c: click/double-click edges + the hover gate come from the native host
    // (over_chrome == cursor over a strip/gap, not over a panel/island surface).
    // P11g: the dock chrome gate is now PURELY native (HoveredSurfacePrev). Every popup
    // that can overlap the chrome is a native Foreground surface and is caught above:
    // menu dropdowns, panel context menus, and the P11f Save Layout dialog scrim. The
    // old ImGui::IsPopupOpen term is gone (the last ImGui::BeginCombo, in the legacy
    // ShaderPreviewRenderer ImGui draw path, was removed in P11g), so no ImGui popup exists here.
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const bool over_chrome = host.HoveredSurfacePrev(mouse) == -1;
    if (over_chrome)
    {
        if (host.WasLeftPressedThisFrame() && m_RestoreButtonRect.Contains(mouse))
            m_MaximizedPanelId.clear();
        else if (host.WasLeftDoubleClickedThisFrame() && strip.Contains(mouse))
            m_MaximizedPanelId.clear();
    }
}

void DefaultLayout::CloseNativePanel(const std::string& panel_id)
{
    if (panel_id.empty())
        return;
    if (m_MaximizedPanelId == panel_id)
        m_MaximizedPanelId.clear();
    // Clear the window's open flag so the next ReconcileNativeTreeWithOpenWindows does not
    // immediately re-dock it; remove it from the tree now so the chrome updates this frame.
    if (m_EditorUi != nullptr)
    {
        if (EditorWindow* window = m_EditorUi->FindEditorWindow(panel_id))
            window->m_Open = false;
    }
    m_NativeDockTree.RemovePanel(panel_id);
}

namespace
{
    // Quadrant test for drag-to-dock: returns the edge the cursor is nearest within an
    // outer zone, else Center. Edge zone is 30% of the smaller extent.
    EditorDock::EDockDir ComputeDropDir(const ZSlate::UIRect& r, const ZSlate::Vector2& m)
    {
        if (r.width <= 0.0f || r.height <= 0.0f)
            return EditorDock::EDockDir::Center;

        const float fx = (m.x - r.x) / r.width;
        const float fy = (m.y - r.y) / r.height;
        const float dl = fx, dr = 1.0f - fx, dt = fy, db = 1.0f - fy;
        const float edge = std::min(std::min(dl, dr), std::min(dt, db));

        constexpr float kEdgeZone = 0.30f;
        if (edge > kEdgeZone)
            return EditorDock::EDockDir::Center;
        if (edge == dl)
            return EditorDock::EDockDir::Left;
        if (edge == dr)
            return EditorDock::EDockDir::Right;
        if (edge == dt)
            return EditorDock::EDockDir::Top;
        return EditorDock::EDockDir::Bottom;
    }
}  // namespace

bool DefaultLayout::HandleNativeDockInput(const ZSlate::UIRect& host_rect)
{
    // P11a: pointer + button-hold + click/double-click edges + wheel all come from
    // the GLFW-backed EditorSlateHost. P11g: the chrome gate below is now fully native
    // (HoveredSurfacePrev over the registered panel / island / Foreground-popup surfaces)
    // -- no ImGui state is consulted here anymore.
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const ZSlate::Vector2 mouse = ChromeMouse();
    const float thickness = m_NativeDockTree.GetMetrics().SplitterThickness;

    // Baseline the cursor to the arrow every frame; the splitter-drag / tab-drag /
    // splitter-hover paths below re-assert Resize/Hand when the pointer is actually
    // over a splitter or dragging a tab. Without this, a Resize cursor set on splitter
    // hover would stick after the pointer leaves the splitter (onto a panel via the
    // over_panel early-return, or onto empty chrome), because GLFW cursors are sticky
    // per-window. Mirrors the per-frame reset in FloatingPanelManager::BuildBatches.
    // Docked panels register + handle their own cursors AFTER this runs, so they still
    // win over this baseline where they want a non-arrow cursor.
    host.SetMouseCursor(ZSlate::EMouseCursor::Default);

    auto node_in_tree = [&](const EditorDock::DockNode* ptr) {
        bool found = false;
        m_NativeDockTree.ForEachNode([&](EditorDock::DockNode& n) {
            if (&n == ptr)
                found = true;
        });
        return found;
    };

    // --- Continue / finish a tab drag-to-dock (runs before the chrome gate so the drag
    //     keeps tracking when the cursor is over a panel window) ---
    if (!m_TabDragId.empty())
    {
        if (!ChromeLeftDown())
        {
            bool changed = false;
            if (m_TabDragActive && m_HasDrop && m_DropTargetLeaf != nullptr && node_in_tree(m_DropTargetLeaf))
            {
                m_NativeDockTree.MovePanel(m_TabDragId, *m_DropTargetLeaf, m_DropDir, 0.5f);
                changed = true;
            }
            else if (m_TabDragActive && !host_rect.Contains(mouse))
            {
                // P6 tear-off: the tab was dragged outside the dock host -> detach the
                // panel into its own OS window centred under the cursor. Seed the window
                // size from the panel's current dock leaf so it pops out at a sane size.
                int spawn_w = 640;
                int spawn_h = 420;
                if (const EditorDock::DockNode* leaf = m_NativeDockTree.FindPanelLeaf(m_TabDragId))
                {
                    spawn_w = std::max(static_cast<int>(leaf->NodeRect.width), 240);
                    spawn_h = std::max(static_cast<int>(leaf->NodeRect.height), 160);
                }
                const int spawn_x = static_cast<int>(mouse.x) - spawn_w / 2;
                const int spawn_y = static_cast<int>(mouse.y) - 12;
                FloatingPanelManager::Get().RequestFloatAt(m_TabDragId, spawn_x, spawn_y, spawn_w, spawn_h);
                changed = true;  // ProcessPendingFloat pulls it from the tree next tick
            }
            m_TabDragId.clear();
            m_TabDragActive = false;
            m_HasDrop = false;
            m_DropTargetLeaf = nullptr;
            return changed;
        }

        if (!m_TabDragActive)
        {
            const float dx = mouse.x - m_TabDragStartMouse.x;
            const float dy = mouse.y - m_TabDragStartMouse.y;
            constexpr float kThreshold = 6.0f;
            if (dx * dx + dy * dy >= kThreshold * kThreshold)
                m_TabDragActive = true;
        }

        if (m_TabDragActive)
        {
            m_HasDrop = false;
            m_DropTargetLeaf = nullptr;
            if (EditorDock::DockNode* leaf = m_NativeDockTree.LeafAt(mouse))
            {
                m_DropTargetLeaf = leaf;
                m_DropDir = ComputeDropDir(leaf->NodeRect, mouse);
                m_HasDrop = true;
            }
            host.SetMouseCursor(ZSlate::EMouseCursor::Hand);
        }
        return false;
    }

    // --- Continue an in-flight splitter drag (tracks the cursor even past the bar) ---
    if (m_DraggingSplitter != nullptr)
    {
        if (!ChromeLeftDown() || !node_in_tree(m_DraggingSplitter))
        {
            m_DraggingSplitter = nullptr;
            return false;
        }

        EditorDock::DockNode& s = *m_DraggingSplitter;
        const bool horizontal = (s.Orientation == EditorDock::EOrientation::Horizontal);
        const float usable = horizontal ? std::max(1.0f, s.NodeRect.width - thickness)
                                        : std::max(1.0f, s.NodeRect.height - thickness);
        const float offset = horizontal ? (mouse.x - s.NodeRect.x) : (mouse.y - s.NodeRect.y);
        m_NativeDockTree.SetSplitRatioFromDrag(s, offset / usable);
        host.SetMouseCursor(horizontal ? ZSlate::EMouseCursor::ResizeEW : ZSlate::EMouseCursor::ResizeNS);
        return false;
    }

    // Suppress when the mouse is not over the dock chrome. P11c: this gate is now
    // native -- the chrome only acts when the cursor is over a strip/gap, i.e. NOT
    // over any registered panel / island / foreground surface. Islands (Scene/Game/
    // Preview, still ImGui-hosted) register a Panels surface in EditorView::BeginGUI,
    // so they occlude the chrome here just like native panels do. HoveredSurfacePrev
    // (not HoveredSurface) because panels register their surfaces AFTER this runs.
    // P11g: the chrome gate is now PURELY native. The P11f Save Layout dialog is a
    // native overlay modal registering a full-screen Foreground scrim surface, caught by
    // HoveredSurfacePrev (over_panel) like every other native popup (menu dropdowns,
    // panel context menus). The old ImGui::IsPopupOpen term is removed (the last
    // ImGui::BeginCombo, in the legacy ShaderPreviewRenderer ImGui draw path, was removed in P11g).
    const bool over_panel = host.HoveredSurfacePrev(mouse) != -1;
    if (over_panel || !host_rect.Contains(mouse))
        return false;

    // --- P7: wheel-scroll an overflowing tab strip (clamp happens in next frame's layout) ---
    const float wheel = host.GetWheelDelta();
    if (wheel != 0.0f)
    {
        const float wheel_step = m_NativeDockTree.GetMetrics().TabStripHeight * 2.0f;
        m_NativeDockTree.ForEachLeaf([&](EditorDock::DockNode& leaf) {
            if (leaf.ScrollLeftRect.width > 0.0f && leaf.TabStripRect.Contains(mouse))
                leaf.TabScrollOffset -= wheel * wheel_step;
        });
    }

    // --- Splitter: hover cursor, double-click reset, click-to-start-drag ---
    if (EditorDock::DockNode* split = m_NativeDockTree.HitTestSplitter(mouse))
    {
        const bool horizontal = (split->Orientation == EditorDock::EOrientation::Horizontal);
        host.SetMouseCursor(horizontal ? ZSlate::EMouseCursor::ResizeEW : ZSlate::EMouseCursor::ResizeNS);
        if (host.WasLeftDoubleClickedThisFrame())
            m_NativeDockTree.SetSplitRatioFromDrag(*split, 0.5f);
        else if (host.WasLeftPressedThisFrame())
            m_DraggingSplitter = split;
        return false;
    }

    // --- P7: scroll-arrow clicks on an overflowing strip (page the tabs left / right) ---
    if (host.WasLeftPressedThisFrame())
    {
        const float page = m_NativeDockTree.GetMetrics().TabStripHeight * 4.0f;
        bool consumed_arrow = false;
        m_NativeDockTree.ForEachLeaf([&](EditorDock::DockNode& leaf) {
            if (consumed_arrow || leaf.ScrollLeftRect.width <= 0.0f)
                return;
            if (leaf.ScrollLeftRect.Contains(mouse))
            {
                leaf.TabScrollOffset -= page;
                consumed_arrow = true;
            }
            else if (leaf.ScrollRightRect.Contains(mouse))
            {
                leaf.TabScrollOffset += page;
                consumed_arrow = true;
            }
        });
        if (consumed_arrow)
            return false;
    }

    // --- P7: close button on the active tab (closes the panel; structural change) ---
    if (host.WasLeftPressedThisFrame())
    {
        std::string close_id;
        m_NativeDockTree.ForEachLeaf([&](EditorDock::DockNode& leaf) {
            if (!close_id.empty() || leaf.ActiveTabCloseRect.width <= 0.0f)
                return;
            if (leaf.ActiveTabCloseRect.Contains(mouse) && leaf.ActiveTab >= 0 &&
                leaf.ActiveTab < static_cast<int>(leaf.Tabs.size()))
            {
                close_id = leaf.Tabs[leaf.ActiveTab].PanelId;
            }
        });
        if (!close_id.empty())
        {
            CloseNativePanel(close_id);
            return true;  // tree mutated -- caller re-solves geometry
        }
    }

    // --- P7: double-click a tab to maximize that panel (toggles with restore) ---
    if (host.WasLeftDoubleClickedThisFrame())
    {
        std::string maximize_id;
        m_NativeDockTree.ForEachLeaf([&](EditorDock::DockNode& leaf) {
            if (!maximize_id.empty() || !leaf.TabStripRect.Contains(mouse))
                return;
            for (size_t i = 0; i < leaf.TabRects.size(); ++i)
            {
                if (leaf.TabRects[i].Contains(mouse))
                {
                    maximize_id = leaf.Tabs[i].PanelId;
                    break;
                }
            }
        });
        if (!maximize_id.empty())
        {
            m_MaximizedPanelId = maximize_id;
            // Seed the content rect now (host_rect + strip height) so the maximized panel
            // is placed correctly on the very next QueryNativeDockPanel, even before the
            // first RenderNativeMaximized refreshes it.
            const float strip_h = m_NativeDockTree.GetMetrics().TabStripHeight;
            m_MaximizedContentRect = ZSlate::UIRect(host_rect.x, host_rect.y + strip_h, host_rect.width,
                                            std::max(1.0f, host_rect.height - strip_h));
            // Cancel any drag armed by the first click of the double so it does not linger.
            m_TabDragId.clear();
            m_TabDragActive = false;
            return false;
        }
    }

    // --- Tab press: activate the clicked tab AND arm a drag-to-dock candidate ---
    if (host.WasLeftPressedThisFrame())
    {
        m_NativeDockTree.ForEachLeaf([&](EditorDock::DockNode& leaf) {
            if (!m_TabDragId.empty() || !leaf.TabStripRect.Contains(mouse))
                return;
            for (size_t i = 0; i < leaf.TabRects.size(); ++i)
            {
                if (leaf.TabRects[i].Contains(mouse))
                {
                    if (leaf.ActiveTab != static_cast<int>(i))
                        m_NativeDockTree.MarkDirty();  // tab activation is persisted state
                    leaf.ActiveTab = static_cast<int>(i);
                    m_TabDragId = leaf.Tabs[i].PanelId;
                    m_TabDragStartMouse = mouse;
                    m_TabDragActive = false;
                    m_HasDrop = false;
                    m_DropTargetLeaf = nullptr;
                    break;
                }
            }
        });
    }
    return false;
}

bool DefaultLayout::QueryNativeDockPanel(const char* title, float out_rect[4], bool& out_is_active) const
{
    out_is_active = false;
    if (title == nullptr)
        return false;

    // Editor tear-off: while a floating window's panel is being painted, place it
    // at its window's CLIENT-LOCAL origin filling the floating client area. This
    // is only set for the single panel the FloatingPanelManager is rendering this
    // pass, so other panels fall through to the normal docked/floating logic.
    if (!m_FloatingRenderTitle.empty() && m_FloatingRenderTitle == title)
    {
        out_is_active = true;
        out_rect[0] = m_FloatingRenderRect.x;
        out_rect[1] = m_FloatingRenderRect.y;
        out_rect[2] = m_FloatingRenderRect.width;
        out_rect[3] = m_FloatingRenderRect.height;
        return true;
    }

    // FindPanelLeaf is non-const (it walks the tree); the lookup itself is read-only.
    EditorDock::DockTree& tree = const_cast<EditorDock::DockTree&>(m_NativeDockTree);

    // P7 maximize: the maximized panel fills the cached content rect; every other docked
    // panel reports inactive so EditorView::BeginGUI returns Closed (hidden). Panels not
    // docked at all still fall through to the floating path (return false).
    if (!m_MaximizedPanelId.empty())
    {
        if (m_MaximizedPanelId == title)
        {
            out_is_active = true;
            out_rect[0] = m_MaximizedContentRect.x;
            out_rect[1] = m_MaximizedContentRect.y;
            out_rect[2] = m_MaximizedContentRect.width;
            out_rect[3] = m_MaximizedContentRect.height;
            return true;
        }
        if (tree.HasPanel(title))
        {
            out_is_active = false;  // docked but hidden behind the maximized panel
            return true;
        }
        return false;
    }

    const EditorDock::DockNode* leaf = tree.FindPanelLeaf(title);
    if (leaf == nullptr)
        return false;

    if (leaf->ActiveTab >= 0 && leaf->ActiveTab < static_cast<int>(leaf->Tabs.size()))
        out_is_active = (leaf->Tabs[leaf->ActiveTab].PanelId == title);

    out_rect[0] = leaf->ContentRect.x;
    out_rect[1] = leaf->ContentRect.y;
    out_rect[2] = leaf->ContentRect.width;
    out_rect[3] = leaf->ContentRect.height;
    return true;
}

void DefaultLayout::CloseSaveLayoutDialog()
{
    m_SaveDialogOpen = false;
    m_SaveDialogWantCommit = false;
    m_SaveDialogWantClose = false;
    m_SaveDialogInput.Reset();
    m_SaveDialogRoot.reset();
    m_SaveDialogEdit.reset();
}

void DefaultLayout::CommitSaveLayoutDialog()
{
    if (m_SaveDialogEdit != nullptr)
    {
        const std::string name = trim(m_SaveDialogEdit->Text);
        if (name.empty())
        {
            return;  // Empty name is a no-op (Save button is also visually disabled).
        }
        SaveCurrentLayout(name);
    }
    CloseSaveLayoutDialog();
}

void DefaultLayout::BuildSaveLayoutDialog(float scale)
{
    using namespace ZSlate;

    m_SaveDialogInput.Reset();
    m_SaveDialogWantCommit = false;
    m_SaveDialogWantClose = false;

    auto title = std::make_shared<STextBlock>();
    title->Text = "Save Layout Preset";
    title->FontSize = 16.0f * scale;
    title->Color = ZSlate::UIColor(0.93f, 0.95f, 0.98f, 1.0f);

    auto hint = std::make_shared<STextBlock>();
    hint->Text = "Save the current editor layout as a reusable preset.";
    hint->FontSize = 13.0f * scale;
    hint->Color = ZSlate::UIColor(0.62f, 0.66f, 0.72f, 1.0f);

    m_SaveDialogEdit = std::make_shared<SEditableTextBox>();
    m_SaveDialogEdit->Text = trim(m_LayoutNameBuffer.data());
    m_SaveDialogEdit->HintText = "Layout name";
    m_SaveDialogEdit->FontSize = 14.0f * scale;
    m_SaveDialogEdit->MinWidth = 360.0f * scale;
    // Enter commits. Defer the actual save -- this fires from inside the router.
    m_SaveDialogEdit->OnTextCommitted = [this](const std::string&) { m_SaveDialogWantCommit = true; };

    auto make_label = [&](const char* text, const ZSlate::UIColor& color) {
        auto label = std::make_shared<STextBlock>();
        label->Text = text;
        label->FontSize = 14.0f * scale;
        label->Color = color;
        label->Alignment = ZSlate::TextAnchor::MiddleCenter;
        return label;
    };

    auto save_btn = std::make_shared<SButton>();
    save_btn->SetContent(make_label("Save", ZSlate::UIColor(0.95f, 0.97f, 1.0f, 1.0f)));
    save_btn->Padding = ZSlate::FMargin(18.0f * scale, 6.0f * scale);
    save_btn->NormalColor = ZSlate::UIColor(0.20f, 0.42f, 0.66f, 1.0f);
    save_btn->HoverColor = ZSlate::UIColor(0.26f, 0.50f, 0.76f, 1.0f);
    save_btn->PressedColor = ZSlate::UIColor(0.17f, 0.36f, 0.58f, 1.0f);
    save_btn->OnClicked = [this]() { m_SaveDialogWantCommit = true; };

    auto cancel_btn = std::make_shared<SButton>();
    cancel_btn->SetContent(make_label("Cancel", ZSlate::UIColor(0.90f, 0.92f, 0.95f, 1.0f)));
    cancel_btn->Padding = ZSlate::FMargin(18.0f * scale, 6.0f * scale);
    cancel_btn->OnClicked = [this]() { m_SaveDialogWantClose = true; };

    auto button_row = std::make_shared<SHorizontalBox>();
    button_row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(0.0f, 0.0f))).Fill(1.0f);
    button_row->AddSlot(save_btn).AutoSize();
    button_row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(8.0f * scale, 0.0f))).AutoSize();
    button_row->AddSlot(cancel_btn).AutoSize();

    auto col = std::make_shared<SVerticalBox>();
    col->AddSlot(title).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 8.0f * scale));
    col->AddSlot(hint).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 12.0f * scale));
    col->AddSlot(m_SaveDialogEdit).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 16.0f * scale));
    col->AddSlot(button_row).AutoSize().SetHAlign(EHorizontalAlignment::Fill);

    m_SaveDialogRoot = col;
    m_SaveDialogInput.SetKeyboardFocus(m_SaveDialogEdit.get());
}

void DefaultLayout::DrawDialogs()
{
    // P11f: native Save Layout dialog (replaces the ImGui BeginPopupModal). Painted +
    // input-routed through the editor overlay; a full-screen Foreground scrim surface
    // makes it modal for the native chrome + native panels (see header notes).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    float ui_scale = host.GetUiScale();
    if (ui_scale < 0.5f)
    {
        ui_scale = 1.0f;
    }

    if (m_OpenSaveDialogRequested)
    {
        m_OpenSaveDialogRequested = false;
        BuildSaveLayoutDialog(ui_scale);
        m_SaveDialogOpen = true;
    }

    if (!m_SaveDialogOpen || m_SaveDialogRoot == nullptr)
    {
        return;
    }

    // Escape cancels. Handled before routing so we never free the tree mid-iterate.
    for (ZSlate::EKey key : host.GetKeysThisFrame())
    {
        if (key == ZSlate::EKey::Escape)
        {
            CloseSaveLayoutDialog();
            return;
        }
    }

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    BatchedUIRenderer& renderer = overlay.GetRenderer();
    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZForeground);

    const ZSlate::Vector2 disp_pos = host.GetDisplayPos();
    const ZSlate::Vector2 disp_size = host.GetDisplaySize();
    const ZSlate::UIRect display_rect(disp_pos.x, disp_pos.y, disp_size.x, disp_size.y);

    // Full-screen modal scrim (dims everything below; also the Foreground hit-test
    // surface that suppresses the native dock chrome + native panels while open).
    renderer.drawQuad(display_rect, ZSlate::UIColor(0.0f, 0.0f, 0.0f, 0.45f));
    host.BeginSurface(ZSlate::EditorSlateHost::HashId("##SaveLayoutDialogScrim"), display_rect,
                      ZSlate::ESurfaceLayer::Foreground);

    // Center the dialog panel around the widget tree's desired size.
    m_SaveDialogRoot->CacheDesiredSize();
    const ZSlate::Vector2 content = m_SaveDialogRoot->GetDesiredSize();
    const float pad = 18.0f * ui_scale;
    const float panel_w = content.x + pad * 2.0f;
    const float panel_h = content.y + pad * 2.0f;
    const float panel_x = disp_pos.x + (disp_size.x - panel_w) * 0.5f;
    const float panel_y = disp_pos.y + (disp_size.y - panel_h) * 0.5f;
    const ZSlate::UIRect panel_rect(panel_x, panel_y, panel_w, panel_h);

    renderer.drawQuad(panel_rect, ZSlate::UIColor(0.13f, 0.14f, 0.16f, 1.0f));
    renderer.drawRect(panel_rect, ZSlate::UIColor(0.32f, 0.34f, 0.40f, 1.0f), 1.0f);

    // Paint BEFORE routing so each widget's cached geometry is fresh this frame (the
    // router hit-tests against CachedGeometry) -- matches MenuController's toolbar order.
    ZSlate::FPaintContext ctx;
    ctx.Renderer = &renderer;
    ctx.LayerId = 0;
    m_SaveDialogRoot->Paint(ctx, ZSlate::FGeometry(ZSlate::Vector2(panel_x + pad, panel_y + pad), content));

    // Route input. over_panel = cursor over the dialog body (buttons / edit box); clicks
    // on the scrim outside the panel are absorbed by the modal but do nothing.
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_panel = panel_rect.Contains(mouse);
    m_SaveDialogInput.ProcessMouse(m_SaveDialogRoot, mouse, over_panel, host.IsLeftDown(), 0.0f, host.IsRightDown());

    if (m_SaveDialogInput.HasKeyboardFocus())
    {
        for (unsigned int cp : host.GetCharsThisFrame())
        {
            m_SaveDialogInput.ProcessChar(cp);
        }
        for (ZSlate::EKey key : host.GetKeysThisFrame())
        {
            m_SaveDialogInput.ProcessKey(key);  // Backspace / Enter handled by SEditableTextBox
        }
    }

    // Deferred actions: callbacks above only flag intent so the widget tree stays alive
    // through routing + paint. Commit/close (which frees the tree) runs now.
    if (m_SaveDialogWantCommit)
    {
        CommitSaveLayoutDialog();
    }
    else if (m_SaveDialogWantClose)
    {
        CloseSaveLayoutDialog();
    }
}

void DefaultLayout::QueueBuiltinLayout(const std::string& layout_name)
{
    if (ToBuiltinLayoutType(layout_name) == BuiltinLayoutType::Unknown)
    {
        return;
    }

    m_PendingBuiltinLayout = layout_name;
    m_PendingSnapshot.reset();
    m_ResetAllRequested = false;
}

void DefaultLayout::OpenSaveLayoutDialog()
{
    const std::string suggested_name = MakeSuggestedLayoutName();
    std::fill(m_LayoutNameBuffer.begin(), m_LayoutNameBuffer.end(), '\0');
    std::copy_n(suggested_name.c_str(),
                std::min(suggested_name.size(), m_LayoutNameBuffer.size() - 1),
                m_LayoutNameBuffer.data());
    m_OpenSaveDialogRequested = true;
}

void DefaultLayout::QueueUserLayout(const std::string& layout_name)
{
    if (layout_name.empty())
    {
        return;
    }

    LoadLayoutFromPath(GetUserLayoutFilePath(layout_name));
}

void DefaultLayout::SaveCurrentLayoutToFileDialog()

{
    std::string selected_path;
    const std::string default_directory = GetUserLayoutDirectory().generic_string();
    const std::string default_file_name = SanitizeLayoutName(MakeSuggestedLayoutName()) + ".zlayout.json";
    if (!EditorUtility::SaveFileDialog("Save Layout to File", default_directory, default_file_name, selected_path))
    {
        return;
    }

    std::filesystem::path file_path(selected_path);
    if (file_path.extension().empty())
    {
        file_path += ".zlayout.json";
    }

    const LayoutSnapshot snapshot = CaptureCurrentLayout(deriveLayoutNameFromPath(file_path));
    SaveSnapshotToFile(snapshot, file_path);
}

void DefaultLayout::LoadLayoutFromFileDialog()
{
    std::string selected_path;
    if (!EditorUtility::OpenFileDialog(
            "Load Layout from File", GetUserLayoutDirectory().generic_string(), std::string(), selected_path))
    {
        return;
    }

    LoadLayoutFromPath(selected_path);
}

void DefaultLayout::DeleteLayout(const std::string& layout_name)
{
    const std::filesystem::path file_path = GetUserLayoutFilePath(layout_name);
    if (!std::filesystem::exists(file_path))
    {
        return;
    }

    std::filesystem::remove(file_path);
    if (m_CurrentLayoutName == layout_name)
    {
        m_CurrentLayoutName = "Default";
    }
}

void DefaultLayout::ResetAllLayouts()
{
    m_PendingBuiltinLayout.clear();
    m_PendingSnapshot.reset();
    m_ResetAllRequested = true;
}

std::vector<std::string> DefaultLayout::GetBuiltinLayoutNames() const
{
    return {"2 by 3", "4 Split", "Default", "Tall", "Wide"};
}

std::vector<std::string> DefaultLayout::GetUserLayoutNames() const
{
    std::vector<std::string> layout_names;
    const std::filesystem::path layout_directory = GetUserLayoutDirectory();
    if (!std::filesystem::exists(layout_directory))
    {
        return layout_names;
    }

    for (const auto& entry : std::filesystem::directory_iterator(layout_directory))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
        {
            continue;
        }

        LayoutSnapshot snapshot;
        if (LoadSnapshotFromFile(entry.path(), snapshot))
        {
            layout_names.push_back(snapshot.name.empty() ? deriveLayoutNameFromPath(entry.path()) : snapshot.name);
        }
    }

    std::sort(layout_names.begin(), layout_names.end());
    return layout_names;
}

bool DefaultLayout::IsCurrentLayout(const std::string& layout_name) const
{
    return m_CurrentLayoutName == layout_name;
}

bool DefaultLayout::ApplyPendingLayout()
{
    bool applied_layout = false;

    if (m_ResetAllRequested)
    {
        const std::filesystem::path layout_directory = GetUserLayoutDirectory();
        if (std::filesystem::exists(layout_directory))
        {
            for (const auto& entry : std::filesystem::directory_iterator(layout_directory))
            {
                if (entry.is_regular_file())
                {
                    std::filesystem::remove(entry.path());
                }
            }
        }

        m_CurrentLayoutName = "Default";
        m_PendingBuiltinLayout = "Default";
        m_ResetAllRequested = false;
    }

    if (m_PendingSnapshot.has_value())
    {
        ApplySnapshot(*m_PendingSnapshot);
        m_PendingSnapshot.reset();
        applied_layout = true;
    }
    else
    {
        const BuiltinLayoutType requested_layout = ToBuiltinLayoutType(m_PendingBuiltinLayout);
        if (requested_layout != BuiltinLayoutType::Unknown)
        {
            ApplyBuiltinLayout(requested_layout);
            m_PendingBuiltinLayout.clear();
            applied_layout = true;
        }
    }

    return applied_layout;
}

void DefaultLayout::ApplyBuiltinLayout(BuiltinLayoutType type)
{
    ApplyBuiltinWindowStates(type);
    m_CurrentLayoutName = ToBuiltinLayoutName(type);

    // P9: author the native DockTree directly. The next EnsureNativeDockTree reconcile trims
    // it to the open set just applied by ApplyBuiltinWindowStates.
    BuildNativeBuiltin(type);
    m_NativeTreeInitialized = true;
}

void DefaultLayout::ApplySnapshot(const LayoutSnapshot& snapshot)
{
    m_EditorUi->ApplyEditorWindowOpenStates(snapshot.window_open_states);
    m_CurrentLayoutName = snapshot.name.empty() ? std::string("Custom Layout") : snapshot.name;

    // P9: restore the native tree from the snapshot. Legacy snapshots (saved before native
    // docking) carry no dock_tree_json -> rebuild the Default native tree, which the next
    // reconcile trims to the snapshot's open set.
    ResetNativeInteractionState();
    if (!snapshot.dock_tree_json.empty() && m_NativeDockTree.DeserializeFromJson(snapshot.dock_tree_json))
    {
        m_NativeTreeInitialized = true;
    }
    else
    {
        BuildNativeBuiltin(BuiltinLayoutType::Default);
        m_NativeTreeInitialized = true;
    }
}

void DefaultLayout::ApplyBuiltinWindowStates(BuiltinLayoutType type)
{
    std::unordered_map<std::string, bool> states = {
        {EditorLayoutWindowIds::kHierarchy, true},
        {EditorLayoutWindowIds::kInspector, true},
        {EditorLayoutWindowIds::kPreview, true},
        {EditorLayoutWindowIds::kContentBrowser, true},
        {EditorLayoutWindowIds::kConsole, true},
        {EditorLayoutWindowIds::kScene, true},
        {EditorLayoutWindowIds::kGame, true},
        {EditorLayoutWindowIds::kTimeline, false},
        {EditorLayoutWindowIds::kBlueprint, false},
        {EditorLayoutWindowIds::kAnimation, false},
        {EditorLayoutWindowIds::kMaterial, false},
        {EditorLayoutWindowIds::kPackageManager, false}};

    switch (type)
    {
        case BuiltinLayoutType::TwoByThree:
            states[EditorLayoutWindowIds::kGame] = false;
            states[EditorLayoutWindowIds::kTimeline] = true;
            break;
        case BuiltinLayoutType::Tall:
            states[EditorLayoutWindowIds::kTimeline] = true;
            break;
        case BuiltinLayoutType::FourSplit:
        case BuiltinLayoutType::Wide:
        case BuiltinLayoutType::Default:
        case BuiltinLayoutType::Unknown:
        default:
            break;
    }

    m_EditorUi->ApplyEditorWindowOpenStates(states);
}

DefaultLayout::LayoutSnapshot DefaultLayout::CaptureCurrentLayout(const std::string& layout_name) const
{
    LayoutSnapshot snapshot;
    snapshot.name = layout_name;
    snapshot.window_open_states = m_EditorUi->GetEditorWindowOpenStates();

    // P9: capture the native DockTree arrangement (the only layout serialization now).
    snapshot.dock_tree_json = m_NativeDockTree.SerializeToJson();

    return snapshot;
}

bool DefaultLayout::SaveSnapshotToFile(const LayoutSnapshot& snapshot, const std::filesystem::path& file_path) const
{
    try
    {
        std::filesystem::create_directories(file_path.parent_path());

        rapidjson::Document document;
        document.SetObject();
        auto& allocator = document.GetAllocator();

        rapidjson::Value name_value;
        name_value.SetString(snapshot.name.c_str(), static_cast<rapidjson::SizeType>(snapshot.name.size()), allocator);
        document.AddMember("name", name_value, allocator);

        // P9: native DockTree JSON (the only layout payload; omitted when empty).
        if (!snapshot.dock_tree_json.empty())
        {
            rapidjson::Value dock_value;
            dock_value.SetString(snapshot.dock_tree_json.c_str(),
                                 static_cast<rapidjson::SizeType>(snapshot.dock_tree_json.size()), allocator);
            document.AddMember("dock_tree", dock_value, allocator);
        }

        rapidjson::Value windows_value(rapidjson::kObjectType);
        for (const auto& [window_name, is_open] : snapshot.window_open_states)
        {
            rapidjson::Value key_value;
            key_value.SetString(window_name.c_str(), static_cast<rapidjson::SizeType>(window_name.size()), allocator);
            windows_value.AddMember(key_value, is_open, allocator);
        }
        document.AddMember("windows", windows_value, allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);

        std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }

        file << buffer.GetString();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool DefaultLayout::LoadSnapshotFromFile(const std::filesystem::path& file_path, LayoutSnapshot& snapshot) const
{
    if (!std::filesystem::exists(file_path))
    {
        return false;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    rapidjson::IStreamWrapper stream_wrapper(file);
    rapidjson::Document document;
    document.ParseStream(stream_wrapper);
    if (document.HasParseError() || !document.IsObject())
    {
        return false;
    }

    const bool has_dock = document.HasMember("dock_tree") && document["dock_tree"].IsString();
    const bool has_windows = document.HasMember("windows") && document["windows"].IsObject();
    // P9: imgui_ini is no longer read. A snapshot is valid if it carries either a native
    // DockTree or a window-open map (legacy ini-only snapshots fall back to the Default tree).
    if (!has_dock && !has_windows)
    {
        return false;
    }

    snapshot.name = document.HasMember("name") && document["name"].IsString() ? document["name"].GetString()
                                                                              : deriveLayoutNameFromPath(file_path);
    snapshot.dock_tree_json = has_dock ? document["dock_tree"].GetString() : std::string();
    snapshot.window_open_states.clear();

    if (document.HasMember("windows") && document["windows"].IsObject())
    {
        for (auto iter = document["windows"].MemberBegin(); iter != document["windows"].MemberEnd(); ++iter)
        {
            if (iter->name.IsString() && iter->value.IsBool())
            {
                snapshot.window_open_states[iter->name.GetString()] = iter->value.GetBool();
            }
        }
    }

    for (auto iter = snapshot.window_open_states.begin(); iter != snapshot.window_open_states.end();)
    {
        const std::string remapped = EditorLayoutWindowIds::RemapLegacyPanelTitle(iter->first);
        if (remapped != iter->first)
        {
            if (snapshot.window_open_states.find(remapped) == snapshot.window_open_states.end())
            {
                snapshot.window_open_states[remapped] = iter->second;
            }
            iter = snapshot.window_open_states.erase(iter);
        }
        else
        {
            ++iter;
        }
    }

    return true;
}

bool DefaultLayout::SaveCurrentLayout(const std::string& layout_name)
{
    const std::string normalized_name = NormalizeLayoutName(layout_name);
    const LayoutSnapshot snapshot = CaptureCurrentLayout(normalized_name);
    if (!SaveSnapshotToFile(snapshot, GetUserLayoutFilePath(normalized_name)))
    {
        return false;
    }

    m_CurrentLayoutName = normalized_name;
    return true;
}

bool DefaultLayout::LoadLayoutFromPath(const std::filesystem::path& file_path)
{
    LayoutSnapshot snapshot;
    if (!LoadSnapshotFromFile(file_path, snapshot))
    {
        return false;
    }

    m_PendingBuiltinLayout.clear();
    m_PendingSnapshot = snapshot;
    m_ResetAllRequested = false;
    return true;
}

std::filesystem::path DefaultLayout::GetUserLayoutDirectory() const
{
    std::filesystem::path project_root = GET_SYSTEM(ProjectInfo)->GetProjectRoot();
    if (project_root.empty())
    {
        project_root = std::filesystem::current_path();
    }

    const std::string saved_dir = GET_SYSTEM(ProjectInfo)->saved_dir.empty() ? std::string("saved")
                                                                             : GET_SYSTEM(ProjectInfo)->saved_dir;
    return project_root / saved_dir / "layouts";
}

std::filesystem::path DefaultLayout::GetNativeSessionFilePath() const
{
    // Sits next to imgui.ini (<saved>/config/) -- both are the live, per-session layout
    // snapshot. native_dock.json is the DockTree equivalent of imgui.ini's docking block.
    std::filesystem::path project_root = GET_SYSTEM(ProjectInfo)->GetProjectRoot();
    if (project_root.empty())
    {
        project_root = std::filesystem::current_path();
    }

    const std::string saved_dir = GET_SYSTEM(ProjectInfo)->saved_dir.empty() ? std::string("saved")
                                                                             : GET_SYSTEM(ProjectInfo)->saved_dir;
    return project_root / saved_dir / "config" / "native_dock.json";
}

bool DefaultLayout::LoadNativeSession()
{
    const std::filesystem::path file_path = GetNativeSessionFilePath();
    if (!std::filesystem::exists(file_path))
    {
        return false;
    }

    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();
    if (json.empty() || !m_NativeDockTree.DeserializeFromJson(json))
    {
        return false;
    }

    ResetNativeInteractionState();
    m_LastSavedNativeJson = json;
    m_NativeDirtySince = -1.0;
    // We just loaded our own session file -> the tree already matches disk; clear the
    // DeserializeFromJson dirty flag so the autosave does not immediately write it back.
    m_NativeDockTree.ConsumeDirty();
    return true;
}

void DefaultLayout::SaveNativeSession(const std::string& json) const
{
    const std::filesystem::path file_path = GetNativeSessionFilePath();
    try
    {
        std::filesystem::create_directories(file_path.parent_path());
        const std::filesystem::path tmp_path = file_path.string() + ".tmp";
        {
            std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                return;
            }
            file << json;
        }
        std::filesystem::rename(tmp_path, file_path);
    }
    catch (...)
    {
        // Persistence is best-effort; a failed write must never disrupt the editor frame.
    }
}

void DefaultLayout::TickNativeSessionAutosave()
{
    // Dirty-flag driven: the DockTree's structural edits / ratio drags / tab activations
    // set its dirty flag, so we only serialize when something actually changed -- never
    // every frame. ConsumeDirty arms the debounce window; while changes keep arriving the
    // first arming timestamp is kept, so a long continuous drag still flushes ~once/sec.
    if (m_NativeDockTree.ConsumeDirty() && m_NativeDirtySince < 0.0)
    {
        m_NativeDirtySince = ZSlate::EditorSlateHost::GetTime();
    }

    if (m_NativeDirtySince < 0.0)
    {
        return;  // nothing pending; no serialize this frame
    }

    constexpr double kAutosaveDelaySeconds = 1.0;
    if (ZSlate::EditorSlateHost::GetTime() - m_NativeDirtySince >= kAutosaveDelaySeconds)
    {
        const std::string json = m_NativeDockTree.SerializeToJson();
        if (json != m_LastSavedNativeJson)
        {
            SaveNativeSession(json);
            m_LastSavedNativeJson = json;
        }
        m_NativeDirtySince = -1.0;
    }
}

std::filesystem::path DefaultLayout::GetUserLayoutFilePath(const std::string& layout_name) const
{
    return GetUserLayoutDirectory() / (SanitizeLayoutName(layout_name) + ".zlayout.json");
}

std::string DefaultLayout::SanitizeLayoutName(const std::string& layout_name) const
{
    std::string sanitized;
    sanitized.reserve(layout_name.size());

    for (const char ch : layout_name)
    {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_')
        {
            sanitized.push_back(ch);
        }
        else if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            sanitized.push_back('_');
        }
    }

    if (sanitized.empty())
    {
        sanitized = "layout";
    }

    return sanitized;
}

std::string DefaultLayout::NormalizeLayoutName(const std::string& layout_name) const
{
    std::string normalized_name = trim(layout_name);
    if (normalized_name.empty())
    {
        normalized_name = MakeSuggestedLayoutName();
    }

    if (ToBuiltinLayoutType(normalized_name) != BuiltinLayoutType::Unknown)
    {
        normalized_name += " Custom";
    }

    return normalized_name;
}

std::string DefaultLayout::MakeSuggestedLayoutName() const
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm {};
#ifdef _WIN32
    localtime_s(&local_tm, &now_c);
#else
    localtime_r(&now_c, &local_tm);
#endif

    std::ostringstream oss;
    oss << "Layout " << (local_tm.tm_year + 1900) << '-';
    oss << std::setw(2) << std::setfill('0') << (local_tm.tm_mon + 1) << '-';
    oss << std::setw(2) << std::setfill('0') << local_tm.tm_mday << ' ';
    oss << std::setw(2) << std::setfill('0') << local_tm.tm_hour;
    oss << std::setw(2) << std::setfill('0') << local_tm.tm_min;
    return oss.str();
}

DefaultLayout::BuiltinLayoutType DefaultLayout::ToBuiltinLayoutType(const std::string& layout_name) const
{
    if (layout_name == "2 by 3")
    {
        return BuiltinLayoutType::TwoByThree;
    }
    if (layout_name == "4 Split")
    {
        return BuiltinLayoutType::FourSplit;
    }
    if (layout_name == "Default")
    {
        return BuiltinLayoutType::Default;
    }
    if (layout_name == "Tall")
    {
        return BuiltinLayoutType::Tall;
    }
    if (layout_name == "Wide")
    {
        return BuiltinLayoutType::Wide;
    }
    return BuiltinLayoutType::Unknown;
}

const char* DefaultLayout::ToBuiltinLayoutName(BuiltinLayoutType type) const
{
    switch (type)
    {
        case BuiltinLayoutType::TwoByThree:
            return "2 by 3";
        case BuiltinLayoutType::FourSplit:
            return "4 Split";
        case BuiltinLayoutType::Tall:
            return "Tall";
        case BuiltinLayoutType::Wide:
            return "Wide";
        case BuiltinLayoutType::Default:
        case BuiltinLayoutType::Unknown:
        default:
            return "Default";
    }
}
