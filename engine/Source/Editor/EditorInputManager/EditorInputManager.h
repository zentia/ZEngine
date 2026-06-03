#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Render/RenderCamera.h"

#include <cstdint>
#include <memory>
#include <vector>

class Editor;

enum class EditorCommand : unsigned int
{
    camera_left = 1 << 0,       // A
    camera_back = 1 << 1,       // S
    camera_foward = 1 << 2,     // W
    camera_right = 1 << 3,      // D
    camera_up = 1 << 4,         // Q
    camera_down = 1 << 5,       // E
    translation_mode = 1 << 6,  // T
    rotation_mode = 1 << 7,     // R
    scale_mode = 1 << 8,        // C
    exit = 1 << 9,              // Esc
    delete_object = 1 << 10,    // Delete
};

class EditorInputManager : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(EditorInputManager); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    bool Initialize() override;
    void Shutdown() override {}
    void Tick(float delta_time);

    void RegisterInput();
    void UpdateCursorOnAxis(Vector2 cursor_uv);
    void ProcessEditorCommand();
    void OnKeyInEditorMode(int key, int scancode, int action, int mods);

    void OnKey(int key, int scancode, int action, int mods);
    void OnReset();
    void OnCursorPos(double xpos, double ypos);
    void OnCursorEnter(int entered);
    void OnScroll(double xoffset, double yoffset);
    void OnMouseButtonClicked(int key, int action);
    void OnWindowClosed();

    bool IsCursorInRect(Vector2 pos, Vector2 size) const;
    bool IsCursorInRect(Vector2 pos, Vector2 size, float cursor_x, float cursor_y) const;

    Vector2 getEngineWindowPos() const { return m_EngineWindowPos; };
    Vector2 getEngineWindowSize() const { return m_EngineWindowSize; };
    float getCameraSpeed() const { return m_CameraSpeed; };
    float getCameraSpeedMin() const { return m_CameraSpeedMin; }
    float getCameraSpeedMax() const { return m_CameraSpeedMax; }
    bool isCameraEasingEnabled() const { return m_CameraEasingEnabled; }
    bool isCameraAccelerationEnabled() const { return m_CameraAccelerationEnabled; }

    void setEngineWindowPos(Vector2 new_window_pos) { m_EngineWindowPos = new_window_pos; };
    void setEngineWindowSize(Vector2 new_window_size) { m_EngineWindowSize = new_window_size; };
    void SetCameraSpeed(float speed);
    void SetCameraSpeedRange(float min_speed, float max_speed);
    void setCameraEasingEnabled(bool enabled) { m_CameraEasingEnabled = enabled; }
    void setCameraAccelerationEnabled(bool enabled) { m_CameraAccelerationEnabled = enabled; }
    void resetEditorCommand() { m_EditorCommand = 0; }

private:
    enum class SceneViewInputMode : uint8_t
    {
        None,
        Orbit,
        Pan,
        DragZoom,
        Gizmo,
        Selection
    };

    void PanSceneCamera(const std::shared_ptr<RenderCamera>& editor_camera, const Vector2& mouse_delta) const;
    void ZoomSceneCamera(const std::shared_ptr<RenderCamera>& editor_camera, float zoom_delta) const;
    void SelectSceneObjectAtMouse();
    void ClearSceneViewInputCapture();

    Vector2 m_EngineWindowPos {0.0f, 0.0f};
    Vector2 m_EngineWindowSize {1280.0f, 768.0f};
    float m_MouseX {0.0f};
    float m_MouseY {0.0f};
    float m_CameraSpeed {0.05f};
    float m_CameraSpeedMin {0.01f};
    float m_CameraSpeedMax {2.0f};
    bool m_CameraEasingEnabled {true};
    bool m_CameraAccelerationEnabled {true};

    Vector2 m_SceneViewMouseDownPos {0.0f, 0.0f};

    SceneViewInputMode m_SceneViewInputMode {SceneViewInputMode::None};
    bool m_SceneViewDragged {false};

    size_t m_CursorOnAxis {3};
    unsigned int m_EditorCommand {0};
};
