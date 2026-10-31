#include "Runtime/Function/Render/RenderCamera.h"

#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/Core/Math/Rect.h"
#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <array>

static inline Rectf GetCameraTargetRect(const RenderCamera& camera)
{
    RenderTexture* target = camera.GetTargetTexture();
    if (target != nullptr)
    {
        return Rectf(0, 0, target->GetWidth(), target->GetHeight());
    }

    // 没有 RenderTexture 时渲染目标就是**屏幕** —— 也就是绝大多数相机。原来这里
    // 直接解引用 target，于是 `GetCameraRect()` / `ResetAspect()` 在任何没设
    // targetTexture 的相机上都是段错误，而不是退回屏幕尺寸。
    if (auto* window_system = GET_SYSTEM(WindowSystem))
    {
        const std::array<int, 2> size = window_system->GetWindowSize();
        return Rectf(0.f, 0.f, static_cast<float>(size[0]), static_cast<float>(size[1]));
    }

    // 连 WindowSystem 都没有（headless / 编辑器批处理）：回一个空矩形，让调用方
    // 走它自己的"尺寸为 0"分支，而不是拿一个假的尺寸去算 aspect。
    return Rectf();
}


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
    m_WorldPositionD = Vector3d(m_Position);
}

Vector3 RenderCamera::position() const
{
    if (LargeWorldCoordinates::IsEnabled())
    {
        return LargeWorldCoordinates::WorldToRender(m_WorldPositionD);
    }
    return m_Position;
}

void RenderCamera::SetWorldPosition(Vector3d world_position)
{
    m_WorldPositionD = world_position;
    m_Position = world_position.ToVector3();
}

Vector3 RenderCamera::GetPreViewTranslation() const
{
    return LargeWorldCoordinates::GetPreViewTranslation(m_WorldPositionD);
}

void RenderCamera::move(Vector3 delta)
{
    if (LargeWorldCoordinates::IsEnabled())
    {
        m_WorldPositionD += Vector3d(delta);
        m_Position = LargeWorldCoordinates::WorldToRender(m_WorldPositionD);
    }
    else
    {
        m_Position += delta;
        m_WorldPositionD = Vector3d(m_Position);
    }
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
    SetWorldPosition(Vector3d(position));

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

    // A Game camera's GetViewMatrix() returns m_ViewMatrices[MAIN] -- which only
    // CameraComponent (i.e. play mode) ever wrote. In edit mode it stayed IDENTITY
    // although RenderSystem had already posed the camera with LookAt(), so the Game
    // viewport looked out of the origin along -Z, back-face culled every mesh and
    // showed nothing but the clear colour. Keep the matrix in step with the pose;
    // SetMainViewMatrix() still overrides it once a CameraComponent drives the camera.
    std::lock_guard<std::mutex> lock_guard(m_ViewMatrixMutex);
    m_ViewMatrices[MAIN_VIEW_MATRIX_INDEX] = Math::MakeLookAtMatrix(position, target, up);
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

    ApplyTaaJitter(proj_mat);
    return proj_mat;
}

Matrix4x4 RenderCamera::GetProjectionMatrix() const
{
    if (m_Aspect <= 0.0f)
    {
        return GetPersProjMatrix();
    }
    return GetProjectionMatrixForAspect(m_Aspect);
}

Matrix4x4 RenderCamera::GetProjectionMatrixForAspect(float aspect) const
{
    if (aspect <= 0.0f)
    {
        return GetPersProjMatrix();
    }

    Matrix4x4 fix_mat(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);

    if (!m_Orthographic)
    {
        const float fovy_for_aspect =
            Radian(Math::atan(Math::tan(Radian(Degree(m_Fovx) * 0.5f)) / aspect) * 2.0f).valueDegrees();
        return fix_mat * Math::MakePerspectiveMatrix(Radian(Degree(fovy_for_aspect)), aspect, m_Znear, m_Zfar);
    }

    const float half_h = m_OrthoHalfHeight;
    const float half_w = half_h * aspect;
    Matrix4x4 ortho =
        Math::MakeOrthographicProjectionMatrix(-half_w, half_w, -half_h, half_h, m_Zfar, m_Znear);
    Matrix4x4 proj_mat = fix_mat * ortho;
    ApplyTaaJitter(proj_mat);
    return proj_mat;
}

Matrix4x4 RenderCamera::GetProjectionMatrixNoJitter() const
{
    Matrix4x4 projection = GetProjectionMatrix();
    if (!m_TaaEnabled)
    {
        return projection;
    }

    // The jitter is a single additive term in the third column, so removing it is exact:
    // undo what ApplyTaaJitter did rather than duplicating the projection maths.
    projection.m_Mat[0][2] += m_TaaJitter.x;
    projection.m_Mat[1][2] += m_TaaJitter.y;
    return projection;
}

void RenderCamera::ApplyTaaJitter(Matrix4x4& projection) const
{
    if (!m_TaaEnabled)
    {
        return;
    }
    projection.m_Mat[0][2] -= m_TaaJitter.x;
    projection.m_Mat[1][2] -= m_TaaJitter.y;
}

namespace
{
/// Van der Corput / Halton radical inverse. Cheap, deterministic, and the standard
/// choice for TAA jitter because successive samples never repeat on a short cycle.
float HaltonSequence(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f;
    uint32_t remaining = index;
    while (remaining > 0u)
    {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(remaining % base);
        remaining /= base;
    }
    return result;
}
}  // namespace

void RenderCamera::SetTaaEnabled(bool enabled)
{
    m_TaaEnabled = enabled;
    if (!enabled)
    {
        // Leaving a stale offset behind would keep every frame shifted by a sub-pixel for
        // as long as TAA is off -- a permanent half-pixel blur of the whole scene.
        m_TaaJitter = Vector2(0.0f, 0.0f);
        m_TaaFrameIndex = 0;
    }
}

void RenderCamera::AdvanceTaaJitter(uint32_t width, uint32_t height)
{
    if (!m_TaaEnabled || width == 0u || height == 0u)
    {
        m_TaaJitter = Vector2(0.0f, 0.0f);
        return;
    }

    // Halton index 0 is (0,0), which is the un-jittered centre -- start at 1 so the very
    // first frame is already offset.
    ++m_TaaFrameIndex;
    const float x = HaltonSequence(m_TaaFrameIndex, 2u);
    const float y = HaltonSequence(m_TaaFrameIndex, 3u);

    // Halton lives in [0,1). Centre it on the pixel, then convert a one-pixel offset into
    // NDC (NDC spans 2 units across the whole target).
    m_TaaJitter = Vector2((x - 0.5f) * 2.0f / static_cast<float>(width),
                          (y - 0.5f) * 2.0f / static_cast<float>(height));
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

    // 走 SetAspect()，不是直接写 m_State.aspect：投影矩阵读的是 **m_Aspect**，
    // 写 m_State.aspect 的话 `GetAspect()` 和画面都纹丝不动 —— ResetAspect 就成
    // 了一个"调用成功但什么都没发生"的空操作，而这正是最难查的那类错。
    SetAspect((r.height > 0.f) ? (r.width / r.height) : 1.0f);

    m_State.dirtyProjectionMatrix = true;
    m_State.dirtySkyboxProjectionMatrix = true;
    m_State.implicitAspect = true;
}