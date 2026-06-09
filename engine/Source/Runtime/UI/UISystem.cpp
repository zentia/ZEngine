#include "Runtime/UI/UISystem.h"

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Slate/Application/SlateApplication.h"
#include "Runtime/Slate/Backend/SlateUIRendererBackend.h"
#include "Runtime/UI/Render/UIRenderer.h"

#include <GLFW/glfw3.h>

namespace
{
ZSlate::EKey MapGlfwKeyToSlate(int glfw_key)
{
    switch (glfw_key)
    {
        case GLFW_KEY_BACKSPACE: return ZSlate::EKey::Backspace;
        case GLFW_KEY_DELETE:    return ZSlate::EKey::Delete;
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:  return ZSlate::EKey::Enter;
        case GLFW_KEY_ESCAPE:    return ZSlate::EKey::Escape;
        case GLFW_KEY_LEFT:      return ZSlate::EKey::Left;
        case GLFW_KEY_RIGHT:     return ZSlate::EKey::Right;
        case GLFW_KEY_HOME:      return ZSlate::EKey::Home;
        case GLFW_KEY_END:       return ZSlate::EKey::End;
        default:                 return ZSlate::EKey::Unknown;
    }
}
}  // namespace

std::vector<std::type_index> UISystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(RenderSystem), GET_SYSTEM_TYPE(WindowSystem)};
}

bool UISystem::Initialize()
{
    InitializeUIRenderer();

    // Hand ourselves over to RenderSystem as the WindowUI driver. UIPass calls
    // our PreRender() once per frame, which is where we paint the ZSlate root.
    GET_SYSTEM(RenderSystem)->InitializeUIRenderBackend(this);

    if (auto window = GET_SYSTEM(WindowSystem))
    {
        window->registerOnCursorPosFunc([this](double x, double y) {
            m_PointerPos = Vector2(static_cast<float>(x), static_cast<float>(y));
        });

        window->registerOnMouseButtonFunc([this](int button, int action, int /*mods*/) {
            if (button == GLFW_MOUSE_BUTTON_LEFT)
            {
                m_SlateLeftDown = (action != GLFW_RELEASE);
            }
        });

        window->registerOnScrollFunc([this](double /*xoff*/, double yoff) {
            m_SlateWheelAccum += static_cast<float>(yoff);
        });

        window->registerOnKeyFunc([this](int key, int /*scan*/, int action, int /*mods*/) {
            if (action == GLFW_PRESS || action == GLFW_REPEAT)
            {
                const ZSlate::EKey slate_key = MapGlfwKeyToSlate(key);
                if (slate_key != ZSlate::EKey::Unknown)
                {
                    m_SlateKeys.push_back(slate_key);
                }
            }
        });

        window->registerOnCharFunc([this](unsigned int codepoint) {
            m_SlateChars.push_back(codepoint);
        });
    }

    m_Initialized = true;
    return true;
}

void UISystem::PreRender()
{
    if (!m_Initialized)
        return;

    RenderSlateRoot();
}

void UISystem::Shutdown()
{
    if (m_UiRenderer)
    {
        DestroyUIRenderer(m_UiRenderer);
        m_UiRenderer = nullptr;
    }
    m_Initialized = false;
}

void UISystem::InitializeUIRenderer()
{
    if (!m_UiRenderer)
    {
        m_UiRenderer = CreateDefaultUIRenderer();
    }
}

void UISystem::RenderSlateRoot()
{
    if (!m_UiRenderer)
    {
        LOG_ERROR(ZEditor, "RenderSlateRoot: m_UiRenderer is null");
        return;
    }

    if (!m_UiRenderer->beginFrame())
    {
        LOG_ERROR(ZEditor, "RenderSlateRoot: beginFrame() failed");
        return;
    }

    // Paint the retained-mode ZSlate root (installed via SetRootContent, i.e.
    // UMGViewport's overlay) through the same batched renderer, so ZSlate/UMG
    // share UIPass, the font atlas, the clip stack and the display size.
    auto slate_root = ZSlate::SlateApplication::Get().GetRootContent();
    if (slate_root)
    {
        // Route input first -- hit-tests against the PREVIOUS frame's cached
        // widget geometry (standard retained-mode order), then paint to refresh
        // geometry for next frame. is_over_host is always true here because the
        // ZSlate surface owns the whole display in the runtime path.
        m_SlateInput.ProcessMouse(slate_root, m_PointerPos, /*is_over_host=*/true, m_SlateLeftDown, m_SlateWheelAccum);
        m_SlateWheelAccum = 0.0f;
        if (m_SlateInput.HasKeyboardFocus())
        {
            for (unsigned int codepoint : m_SlateChars)
                m_SlateInput.ProcessChar(codepoint);
            for (ZSlate::EKey key : m_SlateKeys)
                m_SlateInput.ProcessKey(key);
        }
        m_SlateChars.clear();
        m_SlateKeys.clear();

        static ZSlate::SlateUIRendererTextMeasurer s_slate_measurer;
        s_slate_measurer.SetRenderer(m_UiRenderer);
        ZSlate::SlateApplication::Get().SetTextMeasurer(&s_slate_measurer);

        const Vector2 display_size = m_UiRenderer->getDisplaySize();
        ZSlate::SlateApplication::Get().PaintInto(m_UiRenderer, UIRect(0.0f, 0.0f, display_size.x, display_size.y));
    }
    else
    {
        // No ZSlate surface this frame -- drop queued input so it doesn't
        // accumulate unbounded while nothing is showing.
        m_SlateChars.clear();
        m_SlateKeys.clear();
        m_SlateWheelAccum = 0.0f;
    }

    m_UiRenderer->endFrame();
}
