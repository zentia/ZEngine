#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/MathHeaders.h"
#include "Runtime/Core/Math/Rect.h"
#include "Runtime/Function/Framework/Component/Behaviour.h"
#include "Runtime/Function/Render/Texture/RenderTexture.h"

#include <mutex>

enum class RenderCameraType : int
{
    Editor,
    Game
};

class RenderCamera : public Behaviour
{
    REGISTER_CLASS_TRAITS(kTypeNoFlags);
    REGISTER_CLASS(RenderCamera);
    DECLARE_OBJECT_SERIALIZE();

public:
    RenderCameraType m_CurrentCameraType {RenderCameraType::Editor};

    static const Vector3 X, Y, Z;

    Vector3 m_Position {0.0f, 0.0f, 0.0f};
    Quaternion m_Rotation {Quaternion::IDENTITY};
    Quaternion m_Invrotation {Quaternion::IDENTITY};
    float m_Znear {1000.0f};
    float m_Zfar {0.1f};
    Vector3 m_UpAxis {Z};

    static constexpr float MIN_FOV {4.0f};
    static constexpr float MAX_FOV {120.0f};

    static constexpr int MAIN_VIEW_MATRIX_INDEX {0};

    std::vector<Matrix4x4> m_ViewMatrices {Matrix4x4::IDENTITY};

    // Unity-style: Camera.targetTexture support
    // If null, camera renders to screen/swapchain
    // If set, camera renders to the specified RenderTexture (for picture-in-picture, etc.)
    std::string m_TargetTextureId;  // ID of the RenderTexture to render to (empty = render to screen)

    void SetCurrentCameraType(RenderCameraType type);
    void SetMainViewMatrix(const Matrix4x4& view_matrix,
                           RenderCameraType type = RenderCameraType::Editor);

    void move(Vector3 delta);
    void Rotate(Vector2 delta);
    void Zoom(float offset);
    void LookAt(const Vector3& position, const Vector3& target, const Vector3& up);

    void SetAspect(float aspect);
    void setFOVx(float fovx)
    {
        m_Fovx = Math::Clamp(fovx, MIN_FOV, MAX_FOV);
        if (m_Aspect > 0.0f)
        {
            SetAspect(m_Aspect);
        }
    }

    // Unity-style: Set/get target RenderTexture
    void setTargetTexture(const std::string& texture_id) { m_TargetTextureId = texture_id; }
    const std::string& getTargetTexture() const { return m_TargetTextureId; }
    bool hasTargetTexture() const { return !m_TargetTextureId.empty(); }

    Vector3 position() const { return m_Position; }
    Quaternion rotation() const { return m_Rotation; }

    Vector3 forward() const { return (m_Invrotation * Y); }
    Vector3 up() const { return (m_Invrotation * Z); }
    Vector3 right() const { return (m_Invrotation * X); }
    Vector2 getFOV() const { return {m_Fovx, m_Fovy}; }
    float getAspect() const { return m_Aspect; }
    Matrix4x4 GetViewMatrix();
    Matrix4x4 GetPersProjMatrix() const;
    /// Perspective or orthographic projection (editor scene 2D mode uses ortho).
    Matrix4x4 GetProjectionMatrix() const;
    /// Same as GetProjectionMatrix but with an explicit aspect override (scene sub-viewport).
    Matrix4x4 GetProjectionMatrixForAspect(float aspect) const;

    bool IsOrthographic() const { return m_Orthographic; }
    void SetOrthographic(bool orthographic) { m_Orthographic = orthographic; }
    float GetOrthoHalfHeight() const { return m_OrthoHalfHeight; }
    void SetOrthoHalfHeight(float half_height) { m_OrthoHalfHeight = std::max(half_height, 0.01f); }
    void AdjustOrthoHalfHeight(float wheel_delta);
    Matrix4x4 getLookAtMatrix() const { return Math::MakeLookAtMatrix(position(), position() + forward(), up()); }
    float getFovYDeprecated() const { return m_Fovy; }
    void RenderEditorCamera();
    bool ExecuteCustomRenderPipeline();
    RenderTexture* GetTargetTexture() const;
    Rectf GetCameraRect() const;
    void ResetAspect();

protected:
    float m_Aspect {0.f};
    float m_Fovx {Degree(89.f).valueDegrees()};
    float m_Fovy {0.f};
    bool m_Orthographic {false};
    /// Half of the visible world height in orthographic mode (Unity Scene view "Size").
    float m_OrthoHalfHeight {10.0f};

    std::mutex m_ViewMatrixMutex;

private:
    struct CopiableState
    {
        PPtr<RenderTexture> targetTexture;
        Rectf normalizedViewPortRect;
        float aspect;
        bool dirtyProjectionMatrix;
        bool dirtySkyboxProjectionMatrix;
        bool implicitWorldToCameraMatrix;
        bool implicitAspect;
    };
    CopiableState m_State;
};

inline const Vector3 RenderCamera::X = {1.0f, 0.0f, 0.0f};
inline const Vector3 RenderCamera::Y = {0.0f, 1.0f, 0.0f};
inline const Vector3 RenderCamera::Z = {0.0f, 0.0f, 1.0f};