#include "Runtime/Function/Input/InputSystem.h"

#include "Runtime/Application/Application.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "core/base/Macro.h"

#include <GLFW/glfw3.h>

unsigned int k_complement_control_command = 0xFFFFFFFF;

std::vector<std::type_index> InputSystem::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(WindowSystem)};
}

void InputSystem::OnKey(int key, int scancode, int action, int mods)
{
    // No longer used — EnhancedInputSystem handles raw input directly.
    // Kept for API compatibility.
}

void InputSystem::OnCursorPos(double current_cursor_x, double current_cursor_y)
{
    // No longer used — EnhancedInputSystem handles raw input directly.
    // Kept for API compatibility.
}

void InputSystem::clear()
{
    m_CursorDeltaX = 0;
    m_CursorDeltaY = 0;
}

void InputSystem::CalculateCursorDeltaAngles()
{
    std::array<int, 2> window_size = GET_SYSTEM(WindowSystem)->GetWindowSize();

    if (window_size[0] < 1 || window_size[1] < 1)
    {
        return;
    }

    auto* render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr)
    {
        m_CursorDeltaYaw = Radian(0.0f);
        m_CursorDeltaPitch = Radian(0.0f);
        return;
    }

    std::shared_ptr<RenderCamera> render_camera = render_system->GetRenderCamera(ViewportType::scene);
    if (render_camera == nullptr)
    {
        m_CursorDeltaYaw = Radian(0.0f);
        m_CursorDeltaPitch = Radian(0.0f);
        return;
    }
    const Vector2& fov = render_camera->getFOV();

    Radian cursor_delta_x(Math::DegreesToRadians(static_cast<float>(m_CursorDeltaX)));
    Radian cursor_delta_y(Math::DegreesToRadians(static_cast<float>(m_CursorDeltaY)));

    m_CursorDeltaYaw = (cursor_delta_x / (float)window_size[0]) * fov.x;
    m_CursorDeltaPitch = -(cursor_delta_y / (float)window_size[1]) * fov.y;
}

void InputSystem::SyncGameCommands()
{
    if (!m_EnhancedInput)
        return;

    m_GameCommand = 0;

    // Sync from EnhancedInputSystem action values to legacy GameCommand bits
    if (m_EnhancedInput->IsActionActive("Move"))
    {
        auto val = m_EnhancedInput->GetActionValue("Move");
        Vector2 move = val.GetAxis2D();
        if (move.y > 0) m_GameCommand |= (unsigned int)GameCommand::forward;
        if (move.y < 0) m_GameCommand |= (unsigned int)GameCommand::backward;
        if (move.x < 0) m_GameCommand |= (unsigned int)GameCommand::left;
        if (move.x > 0) m_GameCommand |= (unsigned int)GameCommand::right;
    }

    if (m_EnhancedInput->IsActionActive("Jump"))
        m_GameCommand |= (unsigned int)GameCommand::jump;
    if (m_EnhancedInput->IsActionActive("Sprint"))
        m_GameCommand |= (unsigned int)GameCommand::sprint;
    if (m_EnhancedInput->IsActionActive("Crouch"))
        m_GameCommand |= (unsigned int)GameCommand::squat;
    if (m_EnhancedInput->IsActionActive("Fire"))
        m_GameCommand |= (unsigned int)GameCommand::fire;
    if (m_EnhancedInput->IsActionActive("ToggleFreeCamera"))
        m_GameCommand |= (unsigned int)GameCommand::free_carema;
}

bool InputSystem::Initialize()
{
    // Create and initialize the EnhancedInputSystem
    m_EnhancedInput = new EnhancedInputSystem();
    m_EnhancedInput->SetInitialized(true);

    // Let EnhancedInputSystem register its own callbacks with WindowSystem
    // (it handles OnKey, OnCursorPos, OnMouseButton, OnScroll)
    m_EnhancedInput->Initialize();

    // Set up the default gameplay mapping context
    auto gameplay_context = std::make_shared<InputMappingContext>("Gameplay");

    // Movement: WASD -> Move (Axis2D)
    gameplay_context->MapKey(FKey::W(), GameActions::Move, EInputAxis::Y,  1.0f);
    gameplay_context->MapKey(FKey::S(), GameActions::Move, EInputAxis::Y, -1.0f);
    gameplay_context->MapKey(FKey::A(), GameActions::Move, EInputAxis::X, -1.0f);
    gameplay_context->MapKey(FKey::D(), GameActions::Move, EInputAxis::X,  1.0f);

    // Jump: Space
    gameplay_context->MapKey(FKey::Space(), GameActions::Jump);

    // Sprint: Left Shift
    gameplay_context->MapKey(FKey::LeftShift(), GameActions::Sprint);

    // Crouch: Left Control
    gameplay_context->MapKey(FKey::LeftControl(), GameActions::Crouch);

    // Toggle free camera: F (uses toggle trigger)
    auto toggle_trigger = std::make_shared<TriggerToggle>();
    gameplay_context->MapKey(FKey::F(), GameActions::ToggleFreeCamera, toggle_trigger);

    m_EnhancedInput->AddMappingContext(gameplay_context, 0);

    // No longer register separate callbacks — EnhancedInputSystem handles
    // all raw input. Legacy m_CursorDeltaX/Y are populated from
    // EnhancedInputSystem::GetCursorDeltaX/Y during Tick().

    LOG_INFO(ZEngine, "InputSystem initialized (legacy wrapper over EnhancedInputSystem)");
    return true;
}

void InputSystem::Tick()
{
    // First, read cursor deltas from EnhancedInputSystem BEFORE it ticks
    // (Tick clears the deltas at the end).
    if (m_EnhancedInput)
    {
        m_CursorDeltaX = static_cast<int>(m_EnhancedInput->GetCursorDeltaX());
        m_CursorDeltaY = static_cast<int>(m_EnhancedInput->GetCursorDeltaY());
    }

    // Tick the enhanced system (processes actions, then clears deltas)
    if (m_EnhancedInput)
    {
        m_EnhancedInput->Tick();
    }

    // Legacy angle calculation (kept for backward compat with CameraComponent)
    CalculateCursorDeltaAngles();
    clear();

    // Sync GameCommand from EnhancedInputSystem
    SyncGameCommands();

    // Focus mode -> invalid flag
    if (GET_SYSTEM(WindowSystem)->getFocusMode())
    {
        m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::invalid);
    }
    else
    {
        m_GameCommand |= (unsigned int)GameCommand::invalid;
    }
}
