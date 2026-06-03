#include "Runtime/Function/Render/RenderCamera.h"

#include "Runtime/Core/Math/Rect.h"

#include <algorithm>

static inline Rectf GetCameraTargetRect(const RenderCamera& camera)
{
    RenderTexture* target = camera.GetTargetTexture();
    return Rectf(0, 0, target->GetWidth(), target->GetHeight());
}

IMPLEMENT_REGISTER_CLASS(RenderCamera)
IMPLEMENT_OBJECT_SERAILIZE(RenderCamera);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(RenderCamera);

void RenderCamera::SetCurrentCameraType(RenderCameraType type)
{
    std::lock_guard<std::mutex> lock_guard(m_ViewMatrixMutex);
    m_CurrentCameraType = type;
}

void RenderCamera::SetMainViewMatrix(const Matrix4x4& view_matrix, RenderCameraType type)
{
    std::lock_guard<std::mutex> lock_guard(m_ViewMatrixMutex);
    m_CurrentCameraType = type;
    m_ViewMatrices[MAIN_VIEW_MATRIX_INDEX] = view_matrix;

    Vector3 s = Vector3(view_matrix[0][0], view_matrix[0][1], view_matrix[0][2]);
    Vector3 u = Vector3(view_matrix[1][0], view_matrix[1][1], view_matrix[1][2]);
    Vector3 f = Vector3(-view_matrix[2][0], -view_matrix[2][1], -view_matrix[2][2]);
    m_Position = s * (-view_matrix[0][3]) + u * (-view_matrix[1][3]) + f * view_matrix[2][3];
}

void RenderCamera::move(Vector3 delta)
{
    m_Position += delta;
}

void RenderCamera::Rotate(Vector2 delta)
{
    // rotation around x, y axis
    delta = Vector2(Radian(Degree(delta.x)).valueRadians(), Radian(Degree(delta.y)).valueRadians());

    // limit pitch
    float dot = m_UpAxis.dotProduct(forward());
    if ((dot < -0.99f && delta.x > 0.0f) ||  // angle nearing 180 degrees
        (dot > 0.99f && delta.x < 0.0f))     // angle nearing 0 degrees
        delta.x = 0.0f;

    // pitch is relative to current sideways rotation
    // yaw happens independently
    // this prevents roll
    Quaternion pitch, yaw;
    pitch.FromAngleAxis(Radian(delta.x), X);
    yaw.FromAngleAxis(Radian(delta.y), Z);

    m_Rotation = pitch * m_Rotation * yaw;

    m_Invrotation = m_Rotation.conjugate();
}

void RenderCamera::Zoom(float offset)
{
    // > 0 = zoom in (decrease FOV by <offset> angles)
    m_Fovx = Math::Clamp(m_Fovx - offset, MIN_FOV, MAX_FOV);
}

void RenderCamera::LookAt(const Vector3& position, const Vector3& target, const Vector3& up)
{
    m_Position = position;

    // model rotation
    // maps vectors to camera space (x, y, z)
    Vector3 forward = (target - position).normalisedCopy();
    m_Rotation = forward.getRotationTo(Y);

    // correct the up vector
    // the cross product of non-orthogonal vectors is not normalized
    Vector3 right = forward.crossProduct(up.normalisedCopy()).normalisedCopy();
    Vector3 orthUp = right.crossProduct(forward);

    Quaternion upRotation = (m_Rotation * orthUp).getRotationTo(Z);

    m_Rotation = Quaternion(upRotation) * m_Rotation;

    // inverse of the model rotation
    // maps camera space vectors to model vectors
    m_Invrotation = m_Rotation.conjugate();
}

Matrix4x4 RenderCamera::GetViewMatrix()
{
    std::lock_guard<std::mutex> lock_guard(m_ViewMatrixMutex);
    auto view_matrix = Matrix4x4::IDENTITY;
    switch (m_CurrentCameraType)
    {
        case RenderCameraType::Editor:
            view_matrix = Math::MakeLookAtMatrix(position(), position() + forward(), up());
            break;
        case RenderCameraType::Game:
            view_matrix = m_ViewMatrices[MAIN_VIEW_MATRIX_INDEX];
            break;
        default:
            break;
    }
    return view_matrix;
}

Matrix4x4 RenderCamera::GetPersProjMatrix() const
{
    Matrix4x4 fix_mat(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    Matrix4x4 proj_mat = fix_mat * Math::MakePerspectiveMatrix(Radian(Degree(m_Fovy)), m_Aspect, m_Znear, m_Zfar);

    return proj_mat;
}

Matrix4x4 RenderCamera::GetProjectionMatrix() const
{
    if (!m_Orthographic || m_Aspect <= 0.0f)
    {
        return GetPersProjMatrix();
    }

    const float half_h = m_OrthoHalfHeight;
    const float half_w = half_h * m_Aspect;
    Matrix4x4 fix_mat(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    Matrix4x4 ortho = Math::MakeOrthographicProjectionMatrix(
        -half_w, half_w, -half_h, half_h, m_Zfar, m_Znear);
    return fix_mat * ortho;
}

void RenderCamera::AdjustOrthoHalfHeight(float wheel_delta)
{
    if (wheel_delta == 0.0f)
    {
        return;
    }
    const float scale = 1.0f - wheel_delta * 0.12f;
    SetOrthoHalfHeight(m_OrthoHalfHeight * std::max(scale, 0.05f));
}

void RenderCamera::SetAspect(float aspect)
{
    m_Aspect = aspect;

    // 1 / tan(fovy * 0.5) / aspect = 1 / tan(fovx * 0.5)
    // 1 / tan(fovy * 0.5) = aspect / tan(fovx * 0.5)
    // tan(fovy * 0.5) = tan(fovx * 0.5) / aspect

    m_Fovy = Radian(Math::atan(Math::tan(Radian(Degree(m_Fovx) * 0.5f)) / m_Aspect) * 2.0f).valueDegrees();
}

void RenderCamera::RenderEditorCamera() {}

bool RenderCamera::ExecuteCustomRenderPipeline()
{
    return true;
}

template<typename TransferFunction>
void RenderCamera::Transfer(TransferFunction& transfer)
{
}

RenderTexture* RenderCamera::GetTargetTexture() const
{
    return m_State.targetTexture;
}

Rectf RenderCamera::GetCameraRect() const
{
    Rectf screenRect = GetCameraTargetRect(*this);

    Rectf viewRect = m_State.normalizedViewPortRect;

    viewRect.Scale(screenRect.width, screenRect.height);
    viewRect.Move(screenRect.x, screenRect.y);
    viewRect.Clamp(screenRect);

    return viewRect;
}

void RenderCamera::ResetAspect()
{
    Rectf r = GetCameraRect();
    if (r.height != 0)
        m_State.aspect = r.width / r.height;
    else
        m_State.aspect = 1.0f;

    m_State.dirtyProjectionMatrix = true;
    m_State.dirtySkyboxProjectionMatrix = true;
    m_State.implicitAspect = true;
}