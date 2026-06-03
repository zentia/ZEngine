#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Slate/Application/SlateInput.h"  // ZSlate::SlateInputRouter, ZSlate::EKey
#include "Runtime/UI/Core/WindowUI.h"

#include <memory>
#include <vector>

class UIRenderer;
class WindowSystem;
class RenderSystem;

// UISystem - runtime UI driver (replaces the retired UGUI CanvasManager).
//
// Owns the shared GPU-backed UIRenderer and acts as the WindowUI PreRender
// driver consumed by UIPass. Each frame it paints the retained-mode ZSlate
// root installed via SlateApplication::SetRootContent (the surface UMG's
// UMGViewport publishes) and routes pointer / keyboard input into it.
//
// Unlike CanvasManager this class has no dependency on the UGUI control
// model (Canvas / Widget / RectTransform / UIEventSystem); those were
// retired in favour of UMG (UE-style retained widgets on top of ZSlate).
class UISystem : public WindowUI, public IEngineSystem
{
public:
    std::string GetName() const override { return "UISystem"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    std::vector<std::type_index> GetDependencies() const override;

    bool Initialize() override;
    void PreRender() override;
    void Shutdown() override;

    Vector2 getPointerPosition() const { return m_PointerPos; }
    UIRenderer* getUiRenderer() const { return m_UiRenderer; }

private:
    void InitializeUIRenderer();
    void RenderSlateRoot();

    UIRenderer* m_UiRenderer {nullptr};
    Vector2 m_PointerPos {0.0f, 0.0f};
    bool m_Initialized {false};

    // ZSlate runtime input plumbing. Driven once per frame from RenderSlateRoot
    // against the installed SlateApplication root (uses the previous frame's
    // cached widget geometry for hit-testing). Populated from GLFW callbacks.
    ZSlate::SlateInputRouter m_SlateInput;
    bool m_SlateLeftDown {false};
    float m_SlateWheelAccum {0.0f};
    std::vector<unsigned int> m_SlateChars;
    std::vector<ZSlate::EKey> m_SlateKeys;
};
