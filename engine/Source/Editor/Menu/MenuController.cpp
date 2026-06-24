#include "MenuController.h"

#include "Editor/AssetPipeline/TextureImporter/TextureImporter.h"
#include "Editor/AssetPipeline/TextureImporter/TextureImporterSettings.h"
#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/EditorUI/EditorUI.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"
#include "FileMenu/FileMenu.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Render/RenderDebugConfig.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Widgets/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/SLeafWidget.h"
#include "ZSlate/Widgets/SMenu.h"
#include "ZSlate/Widgets/SSpacer.h"
#include "ZSlate/Widgets/SWidget.h"
#include "WindowMenu/WindowMenu.h"

#include <algorithm>
#include <functional>
#include <memory>

namespace
{
    enum class PlaybackToolbarIcon
    {
        Play,
        Stop,
        Pause,
        Step
    };

    // Native ZSlate leaf widget that paints a single playback glyph (play
    // triangle / stop square / pause bars / step bar+triangle) centered in its
    // geometry. Mirrors the vector geometry of drawPlaybackToolbarIcon below,
    // but through the backend-agnostic UIRenderer (drawConvexPoly / drawQuad)
    // so it composites in the ZSlate batch instead of an ImGui draw list.
    class SPlaybackIcon : public ZSlate::SLeafWidget
    {
    public:
        PlaybackToolbarIcon Icon {PlaybackToolbarIcon::Play};
        ZSlate::UIColor Color {0.94f, 0.96f, 0.99f, 1.0f};
        ZSlate::Vector2 IconSize {30.0f, 22.0f};

        ZSlate::Vector2 ComputeDesiredSize() const override { return IconSize; }

        void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override
        {
            if (ctx.Renderer == nullptr)
                return;

            const ZSlate::UIRect r = geom.ToRect();
            const float w = r.w;
            const float h = r.h;
            const float cx = r.x + w * 0.5f;
            const float cy = r.y + h * 0.5f;

            switch (Icon)
            {
                case PlaybackToolbarIcon::Play:
                {
                    const float tw = w * 0.34f;
                    const float th = h * 0.52f;
                    const ZSlate::Vector2 pts[3] = {ZSlate::Vector2(cx - tw * 0.40f, cy - th * 0.60f),
                                            ZSlate::Vector2(cx - tw * 0.40f, cy + th * 0.60f),
                                            ZSlate::Vector2(cx + tw * 0.72f, cy)};
                    ctx.Renderer->drawConvexPoly(pts, 3, Color);
                    break;
                }
                case PlaybackToolbarIcon::Stop:
                {
                    const float he = std::min(w, h) * 0.23f;
                    ctx.Renderer->drawQuad(ZSlate::UIRect(cx - he, cy - he, he * 2.0f, he * 2.0f), Color);
                    break;
                }
                case PlaybackToolbarIcon::Pause:
                {
                    const float bhw = w * 0.055f;
                    const float bhh = h * 0.28f;
                    const float bo = w * 0.12f;
                    ctx.Renderer->drawQuad(ZSlate::UIRect(cx - bo - bhw, cy - bhh, bhw * 2.0f, bhh * 2.0f), Color);
                    ctx.Renderer->drawQuad(ZSlate::UIRect(cx + bo - bhw, cy - bhh, bhw * 2.0f, bhh * 2.0f), Color);
                    break;
                }
                case PlaybackToolbarIcon::Step:
                {
                    const float bhw = w * 0.032f;
                    const float bhh = h * 0.28f;
                    const float bx = cx - w * 0.17f;
                    ctx.Renderer->drawQuad(ZSlate::UIRect(bx - bhw, cy - bhh, bhw * 2.0f, bhh * 2.0f), Color);

                    const float tw = w * 0.28f;
                    const float th = h * 0.50f;
                    const ZSlate::Vector2 pts[3] = {ZSlate::Vector2(cx - tw * 0.10f, cy - th * 0.60f),
                                            ZSlate::Vector2(cx - tw * 0.10f, cy + th * 0.60f),
                                            ZSlate::Vector2(cx + tw * 0.80f, cy)};
                    ctx.Renderer->drawConvexPoly(pts, 3, Color);
                    break;
                }
                default:
                    break;
            }
        }
    };

}  // namespace

MenuController::MenuController(EditorUI* editor_ui)
    : EditorView(editor_ui, "Menu")
{
    m_WindowFlags = EditorViewFlags_MenuBar | EditorViewFlags_NoTitleBar | EditorViewFlags_NoCollapse |
                    EditorViewFlags_NoResize | EditorViewFlags_NoMove | EditorViewFlags_NoBackground |
                    EditorViewFlags_NoBringToFrontOnFocus;
    RegisterAllMenu();
}

MenuController::~MenuController()
{
    UnregsiterAllMenu();
}

EditorWindowState MenuController::BeginGUI()
{
    // P11e: the editor chrome (menu bar + playback toolbar + native dock host) paints
    // entirely through the ZSlate overlay (BatchedUIRenderer) using native
    // EditorSlateHost display metrics, so it no longer needs an ImGui "Editor menu"
    // host window. Marking the view native-hosted makes the inherited EditorView::EndGUI
    // skip ImGui::End() (it pairs with the dropped ImGui::Begin). Nothing docks into or
    // finds this window by name, and the editor never read its input-capture state
    // (no ImGui::GetIO().WantCaptureMouse use anywhere in Editor/), so dropping the
    // full-screen background window is behaviour-neutral for input routing.
    m_NativeHostActive = true;
    return EditorWindowState::Opened;
}

void MenuController::OnGUI()
{
    // P9: the ImGui menu-bar / playback-toolbar fallback is retired. The menu bar and
    // toolbar are always ZSlate-native (painted through the overlay's BatchedUIRenderer).
    // P11e: no ImGui::SetCursorPos here -- there is no current ImGui window now that the
    // "Editor menu" host is gone, and the dock host reads its origin from native metrics
    // (DefaultLayout::OnGUI) rather than the ImGui cursor.
    DrawPlaybackToolbarNative();
    m_EditorUi->ShowEditorWindowDock();
    RenderNativeMenuBar();
}

void MenuController::EnsureZSlateMenuEntries()
{
    if (m_ZSlateMenuEntriesBuilt)
    {
        return;
    }

    FileMenu* file_menu = getMenu<FileMenu>();
    WindowMenu* window_menu = getMenu<WindowMenu>();

    auto empty_menu = [](ZSlate::SMenu& menu, float scale) {
        menu.AddItem("(empty)", nullptr, scale, /*disabled=*/true);
    };

    std::vector<ZSlate::ZSlateEditorMenuBar::TopMenu> entries;
    entries.push_back({"File", [file_menu](ZSlate::SMenu& menu, float scale) {
                           if (file_menu != nullptr)
                               file_menu->BuildZSlateMenu(menu, scale);
                       }});
    entries.push_back({"Window", [window_menu](ZSlate::SMenu& menu, float scale) {
                           if (window_menu != nullptr)
                               window_menu->BuildZSlateMenu(menu, scale);
                       }});
    entries.push_back({"Edit", empty_menu});
    entries.push_back({"Tools", empty_menu});
    entries.push_back({"Build", [](ZSlate::SMenu& menu, float scale) {
                           // Texture cook (Phase 6): cook all project textures
                           // for the chosen platform into
                           // Intermediate/Cooked/<Platform>/ (BC on
                           // desktop/WebGL, ASTC on mobile).
                           menu.AddItem("Cook Textures for Standalone", []() {
                               TextureImporter::CookProjectTextures(TextureImporterSettings::BuildTarget::Standalone);
                           }, scale);
                           menu.AddItem("Cook Textures for Android", []() {
                               TextureImporter::CookProjectTextures(TextureImporterSettings::BuildTarget::Android);
                           }, scale);
                           menu.AddItem("Cook Textures for iOS", []() {
                               TextureImporter::CookProjectTextures(TextureImporterSettings::BuildTarget::iOS);
                           }, scale);
                           menu.AddItem("Cook Textures for HarmonyOS", []() {
                               TextureImporter::CookProjectTextures(TextureImporterSettings::BuildTarget::OHOS);
                           }, scale);
                           menu.AddItem("Cook Textures for WebGL", []() {
                               TextureImporter::CookProjectTextures(TextureImporterSettings::BuildTarget::WebGL);
                           }, scale);
                       }});
    entries.push_back({"Select", empty_menu});
    entries.push_back({"Actor", [](ZSlate::SMenu& menu, float scale) {
                           menu.AddItem("Place Actor", []() {}, scale);
                       }});
    entries.push_back({"Help", empty_menu});

    m_ZSlateMenuBar.SetMenus(std::move(entries));
    m_ZSlateMenuEntriesBuilt = true;
}

void MenuController::RenderNativeMenuBar()
{
    EnsureZSlateMenuEntries();

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    BatchedUIRenderer& renderer = overlay.GetRenderer();

    // Foreground layer so the bar + dropdowns composite above every other
    // ZSlate window group in the shared batch.
    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZForeground);

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    // P11d: bar geometry comes from the native EditorSlateHost display metrics
    // (= the main viewport pos/size; padding-independent) instead of the ImGui
    // "Editor menu" host window (GetWindowPos / GetWindowSize).
    // P11a: the chrome's mouse comes from the GLFW-backed EditorSlateHost (the
    // transitional r.ZSlate.NativeInput CVar was retired in P10c, so the old
    // ImGui::GetIO() fallback was dead and is gone).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const auto win_pos = host.GetDisplayPos();
    const auto win_size = host.GetDisplaySize();
    // Use ZSlate::UIRect (x,y,w,h) to match ZSlateEditorMenuBar::Render signature.
    const ZSlate::UIRect bar_rect(win_pos.x, win_pos.y, win_size.x, MenuController::kMainMenuBarHeight);
    const ZSlate::UIRect viewport_rect(win_pos.x, win_pos.y, win_size.x, win_size.y);
    const auto mouse = host.GetPointerPos();
    const bool left_down = host.IsLeftDown();

    m_ZSlateMenuBar.Render(overlay, bar_rect, ui_scale, mouse, left_down, viewport_rect);

    // P10b: while a dropdown is open, register a full-viewport Foreground surface so
    // native panels under it report not-hovered (HoveredSurface occlusion). This is
    // the native equivalent of the ##ZSlateMenuCapture window eating click-through;
    // the dropdown handles its own clicks via the mouse passed to Render above.
    if (m_ZSlateMenuBar.IsOpen())
    {
        host.BeginSurface(ZSlate::EditorSlateHost::HashId("##ZSlateMenuForeground"), viewport_rect,
                          ZSlate::ESurfaceLayer::Foreground);
    }

    // P10c: the click-through guard is now the native Foreground surface registered
    // above (host.IsForegroundCapturing()). Native panels are occluded by it via
    // IsSurfaceHovered, so the old "##ZSlateMenuCapture" full-viewport modal ImGui
    // window that used to eat click-through is no longer needed. The dropdown still
    // handles its own clicks via the mouse passed to Render() above.
}

void MenuController::RegisterAllMenu()
{
    registerMenu<FileMenu>();
    registerMenu<WindowMenu>();
}

void MenuController::UnregsiterAllMenu()
{
    for (const auto& menu : m_Menus)
    {
        MemoryManager::DestroyObject(menu);
    }
    m_Menus.clear();
    m_MenuMap.clear();
}

void MenuController::BuildPlaybackToolbar(bool playing, bool paused, float scale)
{
    using namespace ZSlate;

    const float btn_w = 30.0f * scale;
    const float btn_h = 22.0f * scale;

    // Use ZSlate::UIColor (not ::UIColor) to avoid ambiguity.
    const ZSlate::UIColor c_normal(54.0f / 255.0f, 56.0f / 255.0f, 60.0f / 255.0f, 1.0f);
    const ZSlate::UIColor c_hover(68.0f / 255.0f, 70.0f / 255.0f, 75.0f / 255.0f, 1.0f);
    const ZSlate::UIColor c_press(45.0f / 255.0f, 47.0f / 255.0f, 51.0f / 255.0f, 1.0f);
    const ZSlate::UIColor c_tog(40.0f / 255.0f, 84.0f / 255.0f, 124.0f / 255.0f, 1.0f);
    const ZSlate::UIColor c_tog_hover(48.0f / 255.0f, 92.0f / 255.0f, 134.0f / 255.0f, 1.0f);
    const ZSlate::UIColor c_tog_press(54.0f / 255.0f, 98.0f / 255.0f, 140.0f / 255.0f, 1.0f);
    const ZSlate::UIColor c_disabled(48.0f / 255.0f, 50.0f / 255.0f, 54.0f / 255.0f, 1.0f);
    const ZSlate::UIColor icon_on(0.94f, 0.96f, 0.99f, 1.0f);
    const ZSlate::UIColor icon_off(0.70f, 0.74f, 0.80f, 1.0f);

    auto make_btn = [&](PlaybackToolbarIcon icon, bool enabled, bool toggled,
                        std::function<void()> on_click) -> std::shared_ptr<SWidget> {
        auto b = std::make_shared<SButton>();
        b->Padding = ZSlate::FMargin(0.0f);
        b->HAlign = EHorizontalAlignment::Center;
        b->VAlign = EVerticalAlignment::Center;
        if (!enabled)
        {
            b->NormalColor = c_disabled;
            b->HoverColor = c_disabled;
            b->PressedColor = c_disabled;
        }
        else if (toggled)
        {
            b->NormalColor = c_tog;
            b->HoverColor = c_tog_hover;
            b->PressedColor = c_tog_press;
        }
        else
        {
            b->NormalColor = c_normal;
            b->HoverColor = c_hover;
            b->PressedColor = c_press;
        }
        if (enabled && on_click)
            b->OnClicked = std::move(on_click);

        auto glyph = std::make_shared<SPlaybackIcon>();
        glyph->Icon = icon;
        glyph->Color = enabled ? icon_on : icon_off;
        glyph->IconSize = ZSlate::Vector2(btn_w, btn_h);  // ZSlate::Vector2 to avoid ambiguity
        b->SetContent(glyph);
        return b;
    };

    auto row = std::make_shared<SHorizontalBox>();
    row->AddSlot(make_btn(playing ? PlaybackToolbarIcon::Stop : PlaybackToolbarIcon::Play, true, playing,
                          []() { GET_SYSTEM(Editor)->TogglePlayMode(); }))
        .AutoSize();
    row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f * scale, 0.0f))).AutoSize();
    row->AddSlot(make_btn(PlaybackToolbarIcon::Pause, playing, paused,
                          []() { GET_SYSTEM(Editor)->TogglePauseMode(); }))
        .AutoSize();
    row->AddSlot(std::make_shared<SSpacer>(ZSlate::Vector2(1.0f * scale, 0.0f))).AutoSize();
    row->AddSlot(make_btn(PlaybackToolbarIcon::Step, playing, false,
                          []() { GET_SYSTEM(Editor)->RequestStepFrame(); }))
        .AutoSize();

    m_PlaybackToolbar = row;
}

void MenuController::DrawPlaybackToolbarNative()
{
    auto editor = GET_SYSTEM(Editor);
    const bool playing = editor->isPlaying();
    const bool paused = editor->isPaused();

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    if (m_PlaybackToolbar == nullptr || playing != m_BuiltPlaying || paused != m_BuiltPaused ||
        ui_scale != m_BuiltToolbarScale)
    {
        BuildPlaybackToolbar(playing, paused, ui_scale);
        m_BuiltPlaying = playing;
        m_BuiltPaused = paused;
        m_BuiltToolbarScale = ui_scale;
        m_PlaybackInput.Reset();
    }

    // P11d: strip geometry from native display metrics (= main viewport pos/size),
    // not the ImGui "Editor menu" host window (GetWindowPos / GetWindowSize).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const auto win_pos = host.GetDisplayPos();
    const auto win_size = host.GetDisplaySize();
    const float strip_min_y = win_pos.y + MenuController::kMainMenuBarHeight;
    const float strip_h = MenuController::kPlaybackToolbarHeight;

    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    BatchedUIRenderer& renderer = overlay.GetRenderer();
    // Panel layer: the playback toolbar lives in the reserved top strip and never
    // overlaps panels; it is inserted before them so it sorts behind harmlessly.
    overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);

    // BatchedUIRenderer expects ::UIRect (x,y,width,height)
    const ::UIRect strip_rect(win_pos.x, strip_min_y, win_size.x, strip_h);
    renderer.pushClipRect(strip_rect, true);

    // Strip background + 1px bottom separator (mirrors the ImGui fallback).
    renderer.drawQuad(strip_rect, ::UIColor(26.0f / 255.0f, 27.0f / 255.0f, 31.0f / 255.0f, 1.0f));
    renderer.drawQuad(::UIRect(win_pos.x, strip_min_y + strip_h - 1.0f, win_size.x, 1.0f),
                      ::UIColor(52.0f / 255.0f, 56.0f / 255.0f, 62.0f / 255.0f, 1.0f));

    m_PlaybackToolbar->CacheDesiredSize();
    const auto size = m_PlaybackToolbar->GetDesiredSize();
    const float tx = win_pos.x + std::max(0.0f, (win_size.x - size.x) * 0.5f);
    const float ty = strip_min_y + std::max(0.0f, (strip_h - size.y) * 0.5f);

    // Panel plate behind the buttons.
    const float pad = 5.0f * ui_scale;
    renderer.drawQuad(::UIRect(tx - pad, ty - pad, size.x + pad * 2.0f, size.y + pad * 2.0f),
                      ::UIColor(46.0f / 255.0f, 48.0f / 255.0f, 52.0f / 255.0f, 0.96f));

    ZSlate::FPaintContext ctx;
    ctx.Renderer = &overlay;  // ZSlateEditorOverlay IS an ISlateRenderer
    ctx.LayerId = 0;
    m_PlaybackToolbar->Paint(ctx, ZSlate::FGeometry(ZSlate::Vector2(tx, ty), size));

    renderer.popClipRect();

    // Route input. A dropdown being open swallows toolbar input (a Foreground
    // surface owns the mouse while open). P11a: native mouse source from the
    // GLFW-backed EditorSlateHost (the r.ZSlate.NativeInput fallback was retired).
    const auto mouse = host.GetPointerPos();
    const bool menu_open = m_ZSlateMenuBar.IsOpen();
    const bool over_strip = !menu_open && mouse.x >= strip_rect.x && mouse.x <= strip_rect.x + strip_rect.width &&
                            mouse.y >= strip_rect.y && mouse.y <= strip_rect.y + strip_rect.height;
    const bool left_down = !menu_open && host.IsLeftDown();
    m_PlaybackInput.ProcessMouse(m_PlaybackToolbar, mouse, over_strip, left_down, 0.0f);
}
