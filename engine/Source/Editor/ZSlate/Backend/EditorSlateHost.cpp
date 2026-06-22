#include "Editor/ZSlate/Backend/EditorSlateHost.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Input/KeyCodes.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ZSlate
{
namespace
{
    EKey MapKeyToSlate(int key_code)
    {
        if (key_code >= KeyCodes::KEY_A && key_code <= KeyCodes::KEY_Z)
        {
            return static_cast<EKey>(
                static_cast<int>(EKey::A) + (key_code - KeyCodes::KEY_A));
        }

        switch (key_code)
        {
            case KeyCodes::KEY_Backspace: return EKey::Backspace;
            case KeyCodes::KEY_Delete:    return EKey::Delete;
            case KeyCodes::KEY_Enter:
            case KeyCodes::KEY_KPEnter:   return EKey::Enter;
            case KeyCodes::KEY_Escape:    return EKey::Escape;
            case KeyCodes::KEY_Left:      return EKey::Left;
            case KeyCodes::KEY_Right:     return EKey::Right;
            case KeyCodes::KEY_Home:      return EKey::Home;
            case KeyCodes::KEY_End:       return EKey::End;
            case KeyCodes::KEY_Space:     return EKey::Space;
            default:                      return EKey::Unknown;
        }
    }
}  // namespace

EditorSlateHost& EditorSlateHost::Get()
{
    static EditorSlateHost instance;
    return instance;
}

bool EditorSlateHost::IsNativeInputEnabled()
{
    return true;
}

void EditorSlateHost::Initialize()
{
    if (m_Initialized)
        return;

    auto window = GET_SYSTEM(WindowSystem);
    if (window == nullptr)
        return;

    if (GenericWindow* main_window = window->GetMainWindow())
        m_UiScale = std::fmax(1.0f, main_window->GetDpiScale());

    {
        const std::array<int, 2> size = window->GetWindowSize();
        m_WindowW = static_cast<float>(size[0]);
        m_WindowH = static_cast<float>(size[1]);
    }
    if (GenericWindow* main_window = window->GetMainWindow())
    {
        const std::array<int, 2> pos = main_window->GetPosition();
        m_WindowX = static_cast<float>(pos[0]);
        m_WindowY = static_cast<float>(pos[1]);
    }

    window->RegisterOnCursorPosFunc([this](double x, double y) {
        int wx = 0, wy = 0;
        if (auto w = GET_SYSTEM(WindowSystem))
        {
            if (GenericWindow* gw = w->GetMainWindow())
            {
                const std::array<int, 2> pos = gw->GetPosition();
                wx = pos[0];
                wy = pos[1];
            }
        }
        m_WindowX = static_cast<float>(wx);
        m_WindowY = static_cast<float>(wy);
        m_PointerScreen = Vector2(static_cast<float>(wx) + static_cast<float>(x),
                                  static_cast<float>(wy) + static_cast<float>(y));
    });

    window->RegisterOnMouseButtonFunc([this](int button, int action, int mods) {
        const bool down = (action != KeyCodes::RELEASE);
        if (button == 0)  // left
        {
            m_LeftDown = down;
            if (action == KeyCodes::PRESS)
            {
                ++m_PendingLeftPresses;
                constexpr double kDoubleClickTime = 0.30;
                constexpr float kDoubleClickMaxDist = 6.0f;
                auto* app = GET_SYSTEM(WindowSystem) ? GET_SYSTEM(WindowSystem)->GetApplication() : nullptr;
                const double now = app != nullptr ? app->GetTime() : 0.0;
                const float ddx = m_PointerScreen.x - m_LastLeftPressPos.x;
                const float ddy = m_PointerScreen.y - m_LastLeftPressPos.y;
                if (m_LastLeftPressTime >= 0.0 && (now - m_LastLeftPressTime) <= kDoubleClickTime &&
                    (ddx * ddx + ddy * ddy) <= (kDoubleClickMaxDist * kDoubleClickMaxDist))
                {
                    ++m_PendingLeftDoubleClicks;
                    m_LastLeftPressTime = -1.0;
                }
                else
                {
                    m_LastLeftPressTime = now;
                }
                m_LastLeftPressPos = m_PointerScreen;
            }
        }
        else if (button == 1)  // right
        {
            m_RightDown = down;
        }
        else if (button == 2)  // middle
        {
            m_MiddleDown = down;
        }
        if (button == 0 && action == KeyCodes::RELEASE)
            ++m_PendingLeftReleases;
        m_CtrlDown  = (mods & 1) != 0;
        m_ShiftDown = (mods & 2) != 0;
        m_AltDown   = (mods & 4) != 0;
    });

    window->RegisterOnScrollFunc([this](double, double yoff) { m_PendingWheel += static_cast<float>(yoff); });

    window->RegisterOnKeyFunc([this](int key, int, int action, int mods) {
        m_CtrlDown  = (mods & 1) != 0;
        m_ShiftDown = (mods & 2) != 0;
        m_AltDown   = (mods & 4) != 0;
        if (action == KeyCodes::PRESS || action == KeyCodes::REPEAT)
        {
            const EKey k = MapKeyToSlate(key);
            if (k != EKey::Unknown)
                m_PendingKeys.push_back(k);
        }
    });

    window->RegisterOnCharFunc([this](unsigned int codepoint) { m_PendingChars.push_back(codepoint); });

    window->RegisterOnWindowSizeFunc([this](int w, int h) {
        m_WindowW = static_cast<float>(w);
        m_WindowH = static_cast<float>(h);
        if (auto win = GET_SYSTEM(WindowSystem))
        {
            if (GenericWindow* gw = win->GetMainWindow())
            {
                const std::array<int, 2> pos = gw->GetPosition();
                m_WindowX = static_cast<float>(pos[0]);
                m_WindowY = static_cast<float>(pos[1]);
                const float scale = std::fmax(1.0f, gw->GetDpiScale());
                if (scale > 0.0f)
                    m_UiScale = scale;
            }
        }
    });

    if (auto* app = GET_SYSTEM(WindowSystem) ? GET_SYSTEM(WindowSystem)->GetApplication() : nullptr)
    {
        m_CursorArrow   = app->CreateStandardCursor(0);  // arrow
        m_CursorHand     = app->CreateStandardCursor(1);  // hand
        m_CursorResizeEw = app->CreateStandardCursor(2);  // hresize
        m_CursorResizeNs = app->CreateStandardCursor(3);  // vresize
        m_CursorResizeNwse = app->CreateStandardCursor(4); // nwse-resize
    }

    m_Initialized = true;
}

void EditorSlateHost::NewFrame()
{
    m_PointerDelta = Vector2(m_PointerScreen.x - m_PrevPointerScreen.x,
                             m_PointerScreen.y - m_PrevPointerScreen.y);
    m_PrevPointerScreen = m_PointerScreen;

    m_FrameWheel = m_PendingWheel;
    m_PendingWheel = 0.0f;
    m_FrameChars = std::move(m_PendingChars);
    m_PendingChars.clear();
    m_FrameKeys = std::move(m_PendingKeys);
    m_PendingKeys.clear();

    m_FrameLeftPressed  = (m_PendingLeftPresses > 0);
    m_PendingLeftPresses = 0;
    m_FrameLeftDoubleClicked = (m_PendingLeftDoubleClicks > 0);
    m_PendingLeftDoubleClicks = 0;
    m_FrameLeftReleased = (m_PendingLeftReleases > 0);
    m_PendingLeftReleases = 0;

    m_SurfacesPrev = std::move(m_Surfaces);
    m_Surfaces.clear();
    m_NativeTextInputActive = false;
}

int EditorSlateHost::HashId(const char* name)
{
    uint32_t h = 2166136261u;
    if (name != nullptr)
    {
        for (const char* p = name; *p != '\0'; ++p)
        {
            h ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
            h *= 16777619u;
        }
    }
    return static_cast<int>(h & 0x7FFFFFFFu);
}

void EditorSlateHost::PushInputOverride(const FloatingInputState* state)
{
    m_Override = state;
    m_OverrideSurfaces.clear();
}

void EditorSlateHost::PopInputOverride()
{
    m_Override = nullptr;
    m_OverrideSurfaces.clear();
}

void EditorSlateHost::BeginSurface(int id, const UIRect& rect, ESurfaceLayer layer)
{
    Surface s {};
    s.id    = id;
    s.rect  = rect;
    s.layer = layer;
    if (m_Override != nullptr)
        m_OverrideSurfaces.push_back(s);
    else
        m_Surfaces.push_back(s);
}

int EditorSlateHost::HoveredIn(const std::vector<Surface>& surfaces, const Vector2& point) const
{
    int best_id    = -1;
    int best_layer = -1;
    for (const Surface& s : surfaces)
    {
        if (!s.rect.Contains(point))
            continue;
        const int layer = static_cast<int>(s.layer);
        if (layer >= best_layer)
        {
            best_layer = layer;
            best_id    = s.id;
        }
    }
    return best_id;
}

int EditorSlateHost::HoveredSurface(const Vector2& point) const
{
    return HoveredIn(m_Override != nullptr ? m_OverrideSurfaces : m_Surfaces, point);
}

int EditorSlateHost::HoveredSurfacePrev(const Vector2& point) const
{
    return HoveredIn(m_Override != nullptr ? m_OverrideSurfaces : m_SurfacesPrev, point);
}

bool EditorSlateHost::IsForegroundCapturing() const
{
    for (const Surface& s : (m_Override != nullptr ? m_OverrideSurfaces : m_Surfaces))
    {
        if (s.layer == ESurfaceLayer::Foreground)
            return true;
    }
    return false;
}

Vector2 EditorSlateHost::GetFramebufferScale() const
{
    auto window = GET_SYSTEM(WindowSystem);
    if (!window)
        return Vector2(1.0f, 1.0f);
    const std::array<int, 2> logical     = window->GetWindowSize();
    const std::array<int, 2> framebuffer = window->GetFramebufferSize();
    const float scale_x = logical[0] > 0
                              ? static_cast<float>(framebuffer[0]) / static_cast<float>(logical[0])
                              : 1.0f;
    const float scale_y = logical[1] > 0
                              ? static_cast<float>(framebuffer[1]) / static_cast<float>(logical[1])
                              : 1.0f;
    return Vector2(scale_x, scale_y);
}

double EditorSlateHost::GetTime()
{
    auto* app = GET_SYSTEM(WindowSystem) ? GET_SYSTEM(WindowSystem)->GetApplication() : nullptr;
    return app != nullptr ? app->GetTime() : 0.0;
}

void EditorSlateHost::SetMouseCursor(EMouseCursor cursor)
{
    GenericWindow* target_window = nullptr;
    if (m_Override != nullptr && m_Override->window != nullptr)
        target_window = m_Override->window;
    else if (auto window = GET_SYSTEM(WindowSystem))
        target_window = window->GetMainWindow();
    if (target_window == nullptr)
        return;

    void* native_cursor = nullptr;
    switch (cursor)
    {
        case EMouseCursor::Hand:       native_cursor = m_CursorHand;       break;
        case EMouseCursor::ResizeEW:   native_cursor = m_CursorResizeEw;   break;
        case EMouseCursor::ResizeNS:   native_cursor = m_CursorResizeNs;   break;
        case EMouseCursor::ResizeNWSE: native_cursor = m_CursorResizeNwse; break;
        case EMouseCursor::Default:
        default:                       native_cursor = m_CursorArrow;      break;
    }
    auto* app = GET_SYSTEM(WindowSystem) ? GET_SYSTEM(WindowSystem)->GetApplication() : nullptr;
    if (app != nullptr)
        app->SetCursor(target_window->GetNativeHandle(), native_cursor);
}
}  // namespace ZSlate
