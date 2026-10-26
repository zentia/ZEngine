#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/MathHeaders.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Core/Math/Rect.h"
#include "Runtime/Function/Framework/Component/Behaviour.h"
#include "Runtime/Function/Render/Texture/RenderTexture.h"

#include <mutex>
#include "RenderCamera.generated.h"

enum class RenderCameraType : int
{
    Editor,
    Game
};

ZCLASS()
class RenderCamera : public Behaviour
{
    ZGENERATED_BODY(RenderCamera)

public:
    RenderCameraType m_CurrentCameraType {RenderCameraType::Editor};

    static const Vector3 X, Y, Z;

    Vector3 m_Position {0.0f, 0.0f, 0.0f};
    Vector3d m_WorldPositionD {0.0, 0.0, 0.0};
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

    /// Render-space position (LWC: relative to current tile; otherwise absolute float).
    Vector3 position() const;
    Vector3d worldPosition() const { return m_WorldPositionD; }
    void SetWorldPosition(Vector3d world_position);
    Vector3 GetPreViewTranslation() const;

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
    /// Projection with the TAA jitter removed. Anything that must not wobble sub-pixel --
    /// picking being the one that exists today -- asks for this instead.
    Matrix4x4 GetProjectionMatrixNoJitter() const;

    // ---- TAA sub-pixel jitter -------------------------------------------------------
    // TAA is only worth anything if successive frames sample different sub-pixel
    // positions; without jitter the resolve converges to the same image and there is no
    // anti-aliasing at all.
    //
    // Off by default: jittering the projection is a visible change to every pixel the
    // scene writes, so it must only be on once the resolve is actually running.
    void SetTaaEnabled(bool enabled);
    bool IsTaaEnabled() const { return m_TaaEnabled; }
    /// Jitter in NDC units (a fraction of a pixel). Set by AdvanceTaaJitter.
    void SetTaaJitter(const Vector2& jitter_ndc) { m_TaaJitter = jitter_ndc; }
    Vector2 GetTaaJitter() const { return m_TaaJitter; }
    /// Advance the Halton(2,3) sequence and rescale it to one pixel of `width`x`height`.
    /// Cheap no-op (and zeroes the jitter) when TAA is off.
    void AdvanceTaaJitter(uint32_t width, uint32_t height);

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
    /// Subtract the jitter from the projection's third column. With column vectors
    /// (clip = P * v) and w_clip = -v.z, adding `j` to NDC x means x_clip -= j * v.z,
    /// which is exactly this entry.
    void ApplyTaaJitter(Matrix4x4& projection) const;

    Vector2 m_TaaJitter {0.0f, 0.0f};
    bool m_TaaEnabled {false};
    uint32_t m_TaaFrameIndex {0};

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