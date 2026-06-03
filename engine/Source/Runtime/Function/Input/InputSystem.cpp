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
    if (!g_isEditorMode)
    {
        OnKeyInGameMode(key, scancode, action, mods);
    }
}

void InputSystem::OnKeyInGameMode(int key, int scancode, int action, int mods)
{
    m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::jump);

    if (action == GLFW_PRESS)
    {
        switch (key)
        {
            case GLFW_KEY_ESCAPE:
                // close();
                break;
            case GLFW_KEY_R:
                break;
            case GLFW_KEY_A:
                m_GameCommand |= (unsigned int)GameCommand::left;
                break;
            case GLFW_KEY_S:
                m_GameCommand |= (unsigned int)GameCommand::backward;
                break;
            case GLFW_KEY_W:
                m_GameCommand |= (unsigned int)GameCommand::forward;
                break;
            case GLFW_KEY_D:
                m_GameCommand |= (unsigned int)GameCommand::right;
                break;
            case GLFW_KEY_SPACE:
                m_GameCommand |= (unsigned int)GameCommand::jump;
                break;
            case GLFW_KEY_LEFT_CONTROL:
                m_GameCommand |= (unsigned int)GameCommand::squat;
                break;
            case GLFW_KEY_LEFT_ALT:
            {
                GET_SYSTEM(WindowSystem)->SetFocusMode(!GET_SYSTEM(WindowSystem)->getFocusMode());
            }
            break;
            case GLFW_KEY_LEFT_SHIFT:
                m_GameCommand |= (unsigned int)GameCommand::sprint;
                break;
            case GLFW_KEY_F:
                m_GameCommand ^= (unsigned int)GameCommand::free_carema;
                break;
            default:
                break;
        }
    }
    else if (action == GLFW_RELEASE)
    {
        switch (key)
        {
            case GLFW_KEY_ESCAPE:
                // close();
                break;
            case GLFW_KEY_R:
                break;
            case GLFW_KEY_W:
                m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::forward);
                break;
            case GLFW_KEY_S:
                m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::backward);
                break;
            case GLFW_KEY_A:
                m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::left);
                break;
            case GLFW_KEY_D:
                m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::right);
                break;
            case GLFW_KEY_LEFT_CONTROL:
                m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::squat);
                break;
            case GLFW_KEY_LEFT_SHIFT:
                m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::sprint);
                break;
            default:
                break;
        }
    }
}

void InputSystem::OnCursorPos(double current_cursor_x, double current_cursor_y)
{
    if (GET_SYSTEM(WindowSystem)->getFocusMode())
    {
        m_CursorDeltaX = m_LastCursorX - current_cursor_x;
        m_CursorDeltaY = m_LastCursorY - current_cursor_y;
    }
    m_LastCursorX = current_cursor_x;
    m_LastCursorY = current_cursor_y;
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

    std::shared_ptr<RenderCamera> render_camera = GET_SYSTEM(RenderSystem)->GetRenderCamera(ViewportType::scene);
    if (render_camera == nullptr)
    {
        m_CursorDeltaYaw = Radian(0.0f);
        m_CursorDeltaPitch = Radian(0.0f);
        return;
    }
    const Vector2& fov = render_camera->getFOV();

    Radian cursor_delta_x(Math::DegreesToRadians(m_CursorDeltaX));
    Radian cursor_delta_y(Math::DegreesToRadians(m_CursorDeltaY));

    m_CursorDeltaYaw = (cursor_delta_x / (float)window_size[0]) * fov.x;
    m_CursorDeltaPitch = -(cursor_delta_y / (float)window_size[1]) * fov.y;
}

bool InputSystem::Initialize()
{
    GET_SYSTEM(WindowSystem)
        ->registerOnKeyFunc(std::bind(&InputSystem::OnKey,
                                      this,
                                      std::placeholders::_1,
                                      std::placeholders::_2,
                                      std::placeholders::_3,
                                      std::placeholders::_4));
    GET_SYSTEM(WindowSystem)
        ->registerOnCursorPosFunc(
            std::bind(&InputSystem::OnCursorPos, this, std::placeholders::_1, std::placeholders::_2));
    return true;
}

void InputSystem::Tick()
{
    CalculateCursorDeltaAngles();
    clear();

    if (GET_SYSTEM(WindowSystem)->getFocusMode())
    {
        m_GameCommand &= (k_complement_control_command ^ (unsigned int)GameCommand::invalid);
    }
    else
    {
        m_GameCommand |= (unsigned int)GameCommand::invalid;
    }
}