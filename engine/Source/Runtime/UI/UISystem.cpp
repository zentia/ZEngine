#include "Runtime/UI/UISystem.h"

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Backend/SlateUIRendererBackend.h"
#include "ZSlate/Backend/ZEngineSlateRenderer.h"
#include "Runtime/UI/Render/UIRenderer.h"
#include "Runtime/Function/Input/KeyCodes.h"

namespace
{
    ZSlate::EKey MapKeyToSlate(int key)
    {
        // Letter keys A-Z (KEY_A=65 .. KEY_Z=90, contiguous)
        if (key >= KeyCodes::KEY_A && key <= KeyCodes::KEY_Z)
        {
            return static_cast<ZSlate::EKey>(
                static_cast<int>(ZSlate::EKey::A) + (key - KeyCodes::KEY_A));
        }

        switch (key)
        {
            case KeyCodes::KEY_Backspace: return ZSlate::EKey::Backspace;
            case KeyCodes::KEY_Delete:    return ZSlate::EKey::Delete;
            case KeyCodes::KEY_Enter:
            case KeyCodes::KEY_KPEnter:  return ZSlate::EKey::Enter;
            case KeyCodes::KEY_Escape:    return ZSlate::EKey::Escape;
            case KeyCodes::KEY_Left:      return ZSlate::EKey::Left;
            case KeyCodes::KEY_Right:     return ZSlate::EKey::Right;
            case KeyCodes::KEY_Home:      return ZSlate::EKey::Home;
            case KeyCodes::KEY_End:       return ZSlate::EKey::End;
            case KeyCodes::KEY_Space:     return ZSlate::EKey::Space;
            default:                       return ZSlate::EKey::Unknown;
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
        window->RegisterOnCursorPosFunc([this](double x, double y) {
            m_PointerPos = Vector2(static_cast<float>(x), static_cast<float>(y));
        });

        window->RegisterOnMouseButtonFunc([this](int button, int action, int /*mods*/) {
            if (button == static_cast<int>(KeyCodes::MouseButtonLeft))
            {
                m_SlateLeftDown = (action != KeyCodes::RELEASE);
            }
        });

        window->RegisterOnScrollFunc([this](double /*xoff*/, double yoff) {
            m_SlateWheelAccum += static_cast<float>(yoff);
        });

        window->RegisterOnKeyFunc([this](int key, int /*scan*/, int action, int /*mods*/) {
            if (action == KeyCodes::PRESS || action == KeyCodes::REPEAT)
            {
                const ZSlate::EKey slate_key = MapKeyToSlate(key);
                if (slate_key != ZSlate::EKey::Unknown)
                {
                    m_SlateKeys.push_back(slate_key);
                }
            }
        });

        window->RegisterOnCharFunc([this](unsigned int codepoint) {
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
    if (m_SlateRenderer)
    {
        delete m_SlateRenderer;
        m_SlateRenderer = nullptr;
    }
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
        m_SlateRenderer = new ZSlate::ZEngineSlateRenderer(nullptr, m_UiRenderer);
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
        // Convert Vector2 to ZSlate::Vector2
        ZSlate::Vector2 slate_pos(m_PointerPos.x, m_PointerPos.y);
        m_SlateInput.ProcessMouse(slate_root, slate_pos, /*is_over_host=*/true, m_SlateLeftDown, m_SlateWheelAccum);
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

        static ZSlate::SlateUIRendererTextMeasurer s_Slate_measurer(m_SlateRenderer);
        ZSlate::SlateApplication::Get().SetTextMeasurer(&s_Slate_measurer);

        // Install native clipboard callbacks for runtime ZSlate text boxes.
        static auto SlateGetClipboard = [](void* userdata) -> const char* {
            static std::string s_Text;
            if (auto* ws = GET_SYSTEM(WindowSystem))
                if (auto* app = ws->GetApplication())
                    s_Text = app->GetClipboardText();
            return s_Text.c_str();
        };
        static auto SlateSetClipboard = [](const char* text, void* userdata) {
            if (auto* ws = GET_SYSTEM(WindowSystem))
                if (auto* app = ws->GetApplication())
                    app->SetClipboardText(text);
        };
        ZSlate::SlateApplication::Get().SetClipboardCallbacks(
            SlateGetClipboard, SlateSetClipboard, nullptr);

        const Vector2 display_size = m_UiRenderer->getDisplaySize();
        ZSlate::SlateApplication::Get().PaintInto(m_SlateRenderer, ZSlate::UIRect(0.0f, 0.0f, display_size.x, display_size.y));
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
