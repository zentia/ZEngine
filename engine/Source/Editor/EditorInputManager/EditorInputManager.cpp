#include "EditorInputManager.h"

#include "Editor/EditorApplication/EditorApplication.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/ZSlate/Backend/EditorSlateHost.h"
#include "Runtime/Application/Application.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool isKeyDown(int key)
    {
        WindowSystem* window_system = GET_SYSTEM(WindowSystem).get();
        return window_system != nullptr && window_system->GetWindow() != nullptr &&
               glfwGetKey(window_system->GetWindow(), key) == GLFW_PRESS;
    }

    bool isAltDown()
    {
        return isKeyDown(GLFW_KEY_LEFT_ALT) || isKeyDown(GLFW_KEY_RIGHT_ALT);
    }

    bool isActionDown()
    {
        return isKeyDown(GLFW_KEY_LEFT_CONTROL) || isKeyDown(GLFW_KEY_RIGHT_CONTROL) || isKeyDown(GLFW_KEY_LEFT_SUPER) ||
               isKeyDown(GLFW_KEY_RIGHT_SUPER);
    }

    bool isShiftDown()
    {
        return isKeyDown(GLFW_KEY_LEFT_SHIFT) || isKeyDown(GLFW_KEY_RIGHT_SHIFT);
    }
}  // namespace

bool EditorInputManager::Initialize()
{
    RegisterInput();
    return true;
}

void EditorInputManager::Tick(float delta_time)
{
    ProcessEditorCommand();
}

void EditorInputManager::RegisterInput()
{
    GET_SYSTEM(WindowSystem)->registerOnResetFunc(std::bind(&EditorInputManager::OnReset, this));
    GET_SYSTEM(WindowSystem)
        ->registerOnCursorPosFunc(
            std::bind(&EditorInputManager::OnCursorPos, this, std::placeholders::_1, std::placeholders::_2));
    GET_SYSTEM(WindowSystem)
        ->registerOnCursorEnterFunc(std::bind(&EditorInputManager::OnCursorEnter, this, std::placeholders::_1));
    GET_SYSTEM(WindowSystem)
        ->registerOnScrollFunc(
            std::bind(&EditorInputManager::OnScroll, this, std::placeholders::_1, std::placeholders::_2));
    GET_SYSTEM(WindowSystem)
        ->registerOnMouseButtonFunc(
            std::bind(&EditorInputManager::OnMouseButtonClicked, this, std::placeholders::_1, std::placeholders::_2));
    GET_SYSTEM(WindowSystem)->registerOnWindowCloseFunc(std::bind(&EditorInputManager::OnWindowClosed, this));
    GET_SYSTEM(WindowSystem)
        ->registerOnKeyFunc(std::bind(&EditorInputManager::OnKey,
                                      this,
                                      std::placeholders::_1,
                                      std::placeholders::_2,
                                      std::placeholders::_3,
                                      std::placeholders::_4));
}

void EditorInputManager::UpdateCursorOnAxis(Vector2 cursor_uv)
{
    if (GET_SYSTEM(EditorSceneManager)->getEditorCamera())
    {
        Vector2 window_size(m_EngineWindowSize.x, m_EngineWindowSize.y);
        m_CursorOnAxis = GET_SYSTEM(EditorSceneManager)->UpdateCursorOnAxis(cursor_uv, window_size);
    }
}

void EditorInputManager::SetCameraSpeed(float speed)
{
    m_CameraSpeed = std::clamp(speed, m_CameraSpeedMin, m_CameraSpeedMax);
}

void EditorInputManager::SetCameraSpeedRange(float min_speed, float max_speed)
{
    min_speed = std::clamp(min_speed, 0.0001f, 1000.0f);
    max_speed = std::clamp(max_speed, min_speed + 0.0001f, 1000.0f);
    m_CameraSpeedMin = min_speed;
    m_CameraSpeedMax = max_speed;
    SetCameraSpeed(m_CameraSpeed);
}

void EditorInputManager::PanSceneCamera(const std::shared_ptr<RenderCamera>& editor_camera, const Vector2& mouse_delta) const
{
    if (!editor_camera || m_EngineWindowSize.y <= 1.0f)
    {
        return;
    }

    float world_per_pixel = 0.0f;
    if (editor_camera->IsOrthographic())
    {
        world_per_pixel = 2.0f * editor_camera->GetOrthoHalfHeight() / m_EngineWindowSize.y;
    }
    else
    {
        const float camera_height = std::max(std::abs(editor_camera->position().z), 1.0f);
        const float fovy_radians = Math::DegreesToRadians(std::max(editor_camera->getFOV().y, 1.0f));
        world_per_pixel = 2.0f * camera_height * std::tan(fovy_radians * 0.5f) / m_EngineWindowSize.y;
    }

    const float speed_multiplier = isShiftDown() ? 4.0f : 1.0f;

    Vector3 pan_delta = editor_camera->right() * (-mouse_delta.x * world_per_pixel * speed_multiplier) +
                        editor_camera->up() * (mouse_delta.y * world_per_pixel * speed_multiplier);
    editor_camera->move(pan_delta);
}

void EditorInputManager::ZoomSceneCamera(const std::shared_ptr<RenderCamera>& editor_camera, float zoom_delta) const
{
    if (!editor_camera)
    {
        return;
    }

    const float speed_multiplier = isShiftDown() ? 4.0f : 1.0f;
    if (editor_camera->IsOrthographic())
    {
        editor_camera->AdjustOrthoHalfHeight(zoom_delta * speed_multiplier);
        return;
    }

    const float camera_height = std::max(std::abs(editor_camera->position().z), 1.0f);
    const float dolly_distance = zoom_delta * std::max(camera_height * 0.08f, 0.2f) * speed_multiplier;
    editor_camera->move(editor_camera->forward() * dolly_distance);
}

void EditorInputManager::SelectSceneObjectAtMouse()
{
    if (!IsCursorInRect(m_EngineWindowPos, m_EngineWindowSize) || m_EngineWindowSize.x <= 0.0f ||
        m_EngineWindowSize.y <= 0.0f)
    {
        return;
    }

    auto scene_manager = GET_SYSTEM(EditorSceneManager);
    Vector2 picked_uv((m_MouseX - m_EngineWindowPos.x) / m_EngineWindowSize.x,
                      (m_MouseY - m_EngineWindowPos.y) / m_EngineWindowSize.y);
    const GObjectID picked_gobject_id = scene_manager->PickGObjectAtViewportUv(picked_uv);
    const bool toggle_selection = ZSlate::EditorSlateHost::Get().IsCtrlDown();
    const GObjectSelectionOp selection_op =
        toggle_selection ? GObjectSelectionOp::Toggle : GObjectSelectionOp::Replace;

    if (picked_gobject_id == k_invalid_gobject_id)
    {
        if (!toggle_selection)
        {
            scene_manager->OnGObjectSelected(k_invalid_gobject_id, GObjectSelectionOp::Replace);
        }
        return;
    }

    scene_manager->OnGObjectSelected(picked_gobject_id, selection_op);
}

void EditorInputManager::ClearSceneViewInputCapture()
{
    m_SceneViewInputMode = SceneViewInputMode::None;
    m_SceneViewDragged = false;
    glfwSetInputMode(GET_SYSTEM(WindowSystem)->GetWindow(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void EditorInputManager::ProcessEditorCommand()
{
    float camera_speed = m_CameraSpeed;
    std::shared_ptr editor_camera = GET_SYSTEM(EditorSceneManager)->getEditorCamera();
    if (!editor_camera)
    {
        return;
    }

    Quaternion camera_rotate = editor_camera->rotation().inverse();
    Vector3 camera_relative_pos(0, 0, 0);

    if ((unsigned int)EditorCommand::camera_foward & m_EditorCommand)
    {
        camera_relative_pos += camera_rotate * Vector3 {0, camera_speed, 0};
    }
    if ((unsigned int)EditorCommand::camera_back & m_EditorCommand)
    {
        camera_relative_pos += camera_rotate * Vector3 {0, -camera_speed, 0};
    }
    if ((unsigned int)EditorCommand::camera_left & m_EditorCommand)
    {
        camera_relative_pos += camera_rotate * Vector3 {-camera_speed, 0, 0};
    }
    if ((unsigned int)EditorCommand::camera_right & m_EditorCommand)
    {
        camera_relative_pos += camera_rotate * Vector3 {camera_speed, 0, 0};
    }
    if ((unsigned int)EditorCommand::camera_up & m_EditorCommand)
    {
        camera_relative_pos += Vector3 {0, 0, camera_speed};
    }
    if ((unsigned int)EditorCommand::camera_down & m_EditorCommand)
    {
        camera_relative_pos += Vector3 {0, 0, -camera_speed};
    }
    if ((unsigned int)EditorCommand::delete_object & m_EditorCommand)
    {
        GET_SYSTEM(EditorSceneManager)->OnDeleteSelectedGObject();
    }

    editor_camera->move(camera_relative_pos);
}

void EditorInputManager::OnKeyInEditorMode(int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        switch (key)
        {
            case GLFW_KEY_A:
                m_EditorCommand |= (unsigned int)EditorCommand::camera_left;
                break;
            case GLFW_KEY_S:
                if ((mods & GLFW_MOD_CONTROL) || (mods & GLFW_MOD_SUPER))
                {
                    break;
                }
                m_EditorCommand |= (unsigned int)EditorCommand::camera_back;
                break;
            case GLFW_KEY_D:
                m_EditorCommand |= (unsigned int)EditorCommand::camera_right;
                break;
            case GLFW_KEY_Q:
                m_EditorCommand |= (unsigned int)EditorCommand::camera_up;
                break;
            case GLFW_KEY_E:
                if (GET_SYSTEM(WindowSystem)->isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT))
                {
                    m_EditorCommand |= (unsigned int)EditorCommand::camera_down;
                }
                else
                {
                    m_EditorCommand |= (unsigned int)EditorCommand::rotation_mode;
                    GET_SYSTEM(EditorSceneManager)->setEditorAxisMode(EditorAxisMode::RotateMode);
                    GET_SYSTEM(EditorSceneManager)->DrawSelectedEntityAxis();
                }
                break;
            case GLFW_KEY_T:
            case GLFW_KEY_W:
                if (key == GLFW_KEY_W &&
                    (GET_SYSTEM(WindowSystem)->isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT) ||
                     GET_SYSTEM(WindowSystem)->isMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT)))
                {
                    m_EditorCommand |= (unsigned int)EditorCommand::camera_foward;
                }
                else

                {
                    m_EditorCommand |= (unsigned int)EditorCommand::translation_mode;
                    GET_SYSTEM(EditorSceneManager)->setEditorAxisMode(EditorAxisMode::TranslateMode);
                    GET_SYSTEM(EditorSceneManager)->DrawSelectedEntityAxis();
                }
                break;
            case GLFW_KEY_R:
                m_EditorCommand |= (unsigned int)EditorCommand::scale_mode;
                GET_SYSTEM(EditorSceneManager)->setEditorAxisMode(EditorAxisMode::ScaleMode);
                GET_SYSTEM(EditorSceneManager)->DrawSelectedEntityAxis();
                break;
            case GLFW_KEY_C:
                m_EditorCommand |= (unsigned int)EditorCommand::scale_mode;
                GET_SYSTEM(EditorSceneManager)->setEditorAxisMode(EditorAxisMode::ScaleMode);
                GET_SYSTEM(EditorSceneManager)->DrawSelectedEntityAxis();
                break;
            case GLFW_KEY_DELETE:
                m_EditorCommand |= (unsigned int)EditorCommand::delete_object;
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
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::exit);
                break;
            case GLFW_KEY_A:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::camera_left);
                break;
            case GLFW_KEY_S:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::camera_back);
                break;
            case GLFW_KEY_W:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::camera_foward);
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::translation_mode);
                break;
            case GLFW_KEY_D:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::camera_right);
                break;
            case GLFW_KEY_Q:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::camera_up);
                break;
            case GLFW_KEY_E:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::camera_down);
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::rotation_mode);
                break;
            case GLFW_KEY_T:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::translation_mode);
                break;
            case GLFW_KEY_R:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::scale_mode);
                break;
            case GLFW_KEY_C:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::scale_mode);
                break;
            case GLFW_KEY_DELETE:
                m_EditorCommand &= (k_complement_control_command ^ (unsigned int)EditorCommand::delete_object);
                break;
            default:
                break;
        }
    }
}

void EditorInputManager::OnKey(int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        switch (key)
        {
            case GLFW_KEY_F5:
                GET_SYSTEM(Editor)->TogglePlayMode();
                return;
            case GLFW_KEY_F6:
                GET_SYSTEM(Editor)->TogglePauseMode();
                return;
            case GLFW_KEY_F10:
                GET_SYSTEM(Editor)->RequestStepFrame();
                return;
            case GLFW_KEY_S:
                if (g_isEditorMode && ((mods & GLFW_MOD_CONTROL) || (mods & GLFW_MOD_SUPER)))
                {
                    if (auto world = GET_SYSTEM(WorldManager))
                    {
                        world->SaveCurrentLevel();
                    }
                    return;
                }
                break;
            default:
                break;
        }
    }

    if (g_isEditorMode)
    {
        OnKeyInEditorMode(key, scancode, action, mods);
    }
}

void EditorInputManager::OnReset()
{
    // to do
}

void EditorInputManager::OnCursorPos(double xpos, double ypos)
{
    if (!g_isEditorMode)
        return;

    std::shared_ptr editor_camera = GET_SYSTEM(EditorSceneManager)->getEditorCamera();
    const bool has_last_mouse = m_MouseX >= 0.0f && m_MouseY >= 0.0f;
    const Vector2 mouse_delta(has_last_mouse ? static_cast<float>(xpos) - m_MouseX : 0.0f,
                              has_last_mouse ? static_cast<float>(ypos) - m_MouseY : 0.0f);
    const bool cursor_in_scene = IsCursorInRect(m_EngineWindowPos, m_EngineWindowSize, (float)xpos, (float)ypos);

    if (editor_camera && has_last_mouse && m_SceneViewInputMode != SceneViewInputMode::None)
    {
        const Vector2 drag_delta(static_cast<float>(xpos) - m_SceneViewMouseDownPos.x,
                                 static_cast<float>(ypos) - m_SceneViewMouseDownPos.y);
        if (drag_delta.x * drag_delta.x + drag_delta.y * drag_delta.y > 16.0f)
        {
            m_SceneViewDragged = true;
        }

        const float angular_velocity = 180.0f / Math::max(m_EngineWindowSize.x, m_EngineWindowSize.y);
        switch (m_SceneViewInputMode)
        {
            case SceneViewInputMode::Orbit:
                if (!GET_SYSTEM(EditorSceneManager)->IsSceneView2D())
                {
                    glfwSetInputMode(GET_SYSTEM(WindowSystem)->GetWindow(),
                                     GLFW_CURSOR,
                                     isAltDown() ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
                    editor_camera->Rotate(Vector2(mouse_delta.y, mouse_delta.x) * angular_velocity);
                }
                break;

            case SceneViewInputMode::Pan:
                PanSceneCamera(editor_camera, mouse_delta);
                break;
            case SceneViewInputMode::DragZoom:
            {
                const float dominant_delta = std::abs(mouse_delta.x) > std::abs(mouse_delta.y) ? mouse_delta.x : -mouse_delta.y;
                ZoomSceneCamera(editor_camera, dominant_delta * 0.15f);
                break;
            }
            case SceneViewInputMode::Gizmo:
                GET_SYSTEM(EditorSceneManager)
                    ->MoveEntity(xpos,
                                 ypos,
                                 m_MouseX,
                                 m_MouseY,
                                 m_EngineWindowPos,
                                 m_EngineWindowSize,
                                 m_CursorOnAxis,
                                 GET_SYSTEM(EditorSceneManager)->getSelectedObjectMatrix());
                break;
            case SceneViewInputMode::Selection:
            case SceneViewInputMode::None:
                break;
        }
    }
    else if (cursor_in_scene && editor_camera)
    {
        glfwSetInputMode(GET_SYSTEM(WindowSystem)->GetWindow(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (m_EngineWindowSize.x > 0.0f && m_EngineWindowSize.y > 0.0f)
        {
            Vector2 cursor_uv = Vector2(((float)xpos - m_EngineWindowPos.x) / m_EngineWindowSize.x,
                                        ((float)ypos - m_EngineWindowPos.y) / m_EngineWindowSize.y);
            UpdateCursorOnAxis(cursor_uv);
        }
    }

    m_MouseX = (float)xpos;
    m_MouseY = (float)ypos;
}

void EditorInputManager::OnCursorEnter(int entered)
{
    if (!entered)  // lost focus
    {
        m_MouseX = m_MouseY = -1.0f;
        ClearSceneViewInputCapture();
    }
}

void EditorInputManager::OnScroll(double xoffset, double yoffset)
{
    if (!g_isEditorMode)
    {
        return;
    }

    std::shared_ptr editor_camera = GET_SYSTEM(EditorSceneManager)->getEditorCamera();
    if (!editor_camera)
    {
        return;
    }

    if (IsCursorInRect(m_EngineWindowPos, m_EngineWindowSize))
    {
        if (GET_SYSTEM(WindowSystem)->isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT))
        {
            if (yoffset > 0)
            {
                SetCameraSpeed(m_CameraSpeed * 1.2f);
            }
            else
            {
                SetCameraSpeed(m_CameraSpeed * 0.8f);
            }
        }
        else
        {
            ZoomSceneCamera(editor_camera, static_cast<float>(yoffset));
        }
    }
}

void EditorInputManager::OnMouseButtonClicked(int key, int action)
{
    if (!g_isEditorMode)
        return;

    std::shared_ptr editor_camera = GET_SYSTEM(EditorSceneManager)->getEditorCamera();
    const bool cursor_in_scene = IsCursorInRect(m_EngineWindowPos, m_EngineWindowSize);

    if (action == GLFW_PRESS)
    {
        if (!cursor_in_scene || !editor_camera)
        {
            return;
        }

        m_SceneViewMouseDownPos = Vector2(m_MouseX, m_MouseY);
        m_SceneViewDragged = false;

        const bool alt_down = isAltDown();
        const bool action_down = isActionDown();
        if (key == GLFW_MOUSE_BUTTON_MIDDLE || (alt_down && key == GLFW_MOUSE_BUTTON_LEFT) ||
            (action_down && alt_down && key == GLFW_MOUSE_BUTTON_LEFT))
        {
            m_SceneViewInputMode = SceneViewInputMode::Pan;
        }
        else if ((alt_down && key == GLFW_MOUSE_BUTTON_RIGHT) ||
                 (action_down && alt_down && key == GLFW_MOUSE_BUTTON_RIGHT))
        {
            m_SceneViewInputMode = SceneViewInputMode::DragZoom;
        }
        else if (key == GLFW_MOUSE_BUTTON_RIGHT && !GET_SYSTEM(EditorSceneManager)->IsSceneView2D())
        {
            m_SceneViewInputMode = SceneViewInputMode::Orbit;
        }
        else if (key == GLFW_MOUSE_BUTTON_RIGHT && GET_SYSTEM(EditorSceneManager)->IsSceneView2D())
        {
            m_SceneViewInputMode = SceneViewInputMode::Pan;
        }
        else if (key == GLFW_MOUSE_BUTTON_LEFT)
        {
            m_SceneViewInputMode = m_CursorOnAxis != 3 ? SceneViewInputMode::Gizmo : SceneViewInputMode::Selection;
        }
        return;
    }

    if (action == GLFW_RELEASE)
    {
        const SceneViewInputMode input_mode = m_SceneViewInputMode;
        if (key == GLFW_MOUSE_BUTTON_LEFT && input_mode == SceneViewInputMode::Selection && !m_SceneViewDragged)
        {
            Level* current_active_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
            if (current_active_level != nullptr)
            {
                SelectSceneObjectAtMouse();
            }
        }

        if ((key == GLFW_MOUSE_BUTTON_LEFT &&
             (input_mode == SceneViewInputMode::Selection || input_mode == SceneViewInputMode::Gizmo ||
              input_mode == SceneViewInputMode::Pan)) ||
            (key == GLFW_MOUSE_BUTTON_RIGHT &&
             (input_mode == SceneViewInputMode::Orbit || input_mode == SceneViewInputMode::DragZoom)) ||
            (key == GLFW_MOUSE_BUTTON_MIDDLE && input_mode == SceneViewInputMode::Pan))
        {
            ClearSceneViewInputCapture();
        }
    }
}

void EditorInputManager::OnWindowClosed() {}

bool EditorInputManager::IsCursorInRect(Vector2 pos, Vector2 size) const
{
    return IsCursorInRect(pos, size, m_MouseX, m_MouseY);
}

bool EditorInputManager::IsCursorInRect(Vector2 pos, Vector2 size, float cursor_x, float cursor_y) const
{
    return pos.x <= cursor_x && cursor_x <= pos.x + size.x && pos.y <= cursor_y && cursor_y <= pos.y + size.y;
}
