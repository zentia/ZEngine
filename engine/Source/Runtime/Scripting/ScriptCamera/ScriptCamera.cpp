#include "Runtime/Scripting/ScriptCamera/ScriptCamera.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Scripting/ScriptScreen/ScriptScreen.h"

#include <EASTL/unordered_map.h>

#include <cmath>
#include <memory>

// ---------------------------------------------------------------------------
// Camera 实例的缓存（和 ScriptScene 的 Scene 缓存同一套路）
//
// 按 RenderCamera* 建索引，不是按序号：序号会随 RenderSystem 增删相机而滑动，
// 脚本手里那个 Camera 就会突然指向另一台。同一台 RenderCamera 永远拿到同一个
// Camera 对象，所以脚本可以放心缓存 `const cam = Camera.GetMain()`。
// ---------------------------------------------------------------------------
namespace
{
    eastl::unordered_map<RenderCamera*, std::unique_ptr<Camera>>& CameraCache()
    {
        static eastl::unordered_map<RenderCamera*, std::unique_ptr<Camera>> cache;
        return cache;
    }

    eastl::unordered_map<RenderCamera*, std::unique_ptr<Ray>>& RayScratch()
    {
        static eastl::unordered_map<RenderCamera*, std::unique_ptr<Ray>> scratch;
        return scratch;
    }

    Camera* CameraFor(RenderCamera* render_camera)
    {
        if (render_camera == nullptr)
        {
            return nullptr;
        }
        auto& cache = CameraCache();
        const auto it = cache.find(render_camera);
        if (it != cache.end())
        {
            return it->second.get();
        }
        auto created = std::make_unique<Camera>();
        created->Bind(render_camera);
        Camera* result = created.get();
        cache.emplace(render_camera, std::move(created));
        return result;
    }

    constexpr float kMinClip = 0.0001f;

    /// NDC y → 屏幕 / 视口 y。
    ///
    /// **这里必须翻一次**。投影矩阵里带一块 fix_mat（`[1][1] = -1`），它把 y 翻了
    /// 一道；再加上引擎是 Z-up、视口的 y 轴朝下，两道叠出来的结果是"世界里的
    /// 上方"落在**较小**的 ndc.y 上。直接拿 `ndc.y * 0.5 + 0.5` 当屏幕 y 的话，
    /// 画面上下颠倒，而且**不报错** —— `ScreenPointToRay` 于是点哪都点反（点天
    /// 花板选中地板）。
    ///
    /// Unity 的屏幕坐标以左下角为原点、y 向上，所以翻完正好对上 Unity。
    float NdcToScreenY(float ndc_y, float height) { return (0.5f - ndc_y * 0.5f) * height; }

    /// 屏幕 / 视口 y → NDC y（`NdcToScreenY` 的逆）。
    float ScreenToNdcY(float screen_y, float height) { return 1.0f - (screen_y / height) * 2.0f; }

    /// 投影里"最远"的那个 NDC z。两种深度约定（GL 的 [-1,1] 和 D3D 的 [0,1]）
    /// 在这个值上是一致的：1 都代表远平面。挑它做反投影的第二点，就不用在门面
    /// 里判断引擎到底用的是哪一种。
    constexpr float kFarNdcZ = 1.0f;
}  // namespace

// ---------------------------------------------------------------------------
// 静态：找到相机
// ---------------------------------------------------------------------------
Camera* Camera::GetMain()
{
    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr)
    {
        return nullptr;
    }
    return CameraFor(render_system->GetRenderCamera(ViewportType::game).get());
}

int Camera::GetCount()
{
    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr)
    {
        return 0;
    }
    return static_cast<int>(render_system->GetAllCameras().size());
}

Camera* Camera::GetCameraAt(int index)
{
    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr || index < 0)
    {
        return nullptr;
    }
    const std::vector<std::shared_ptr<RenderCamera>> cameras = render_system->GetAllCameras();
    if (index >= static_cast<int>(cameras.size()))
    {
        return nullptr;
    }
    return CameraFor(cameras[static_cast<size_t>(index)].get());
}

std::vector<Camera*> Camera::GetAllCameras()
{
    std::vector<Camera*> result;
    RenderSystem* render_system = GET_SYSTEM(RenderSystem);
    if (render_system == nullptr)
    {
        return result;
    }
    for (const std::shared_ptr<RenderCamera>& camera : render_system->GetAllCameras())
    {
        if (Camera* facade = CameraFor(camera.get()))
        {
            result.push_back(facade);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// 投影参数
// ---------------------------------------------------------------------------
float Camera::GetFieldOfView() const
{
    if (m_RenderCamera == nullptr)
    {
        return 0.0f;
    }
    // Unity 的 fieldOfView 是**垂直**的。渲染器内部主存 fovx（水平），fovy 是从
    // fovx 和 aspect 推出来的 —— 脚本要的是垂直那个。
    return m_RenderCamera->getFOV().y;
}

void Camera::SetFieldOfView(float fov)
{
    if (m_RenderCamera != nullptr)
    {
        m_RenderCamera->setFOVy(fov);
    }
}

float Camera::GetNearClipPlane() const
{
    return (m_RenderCamera != nullptr) ? m_RenderCamera->m_Znear : 0.0f;
}

void Camera::SetNearClipPlane(float near_clip)
{
    if (m_RenderCamera == nullptr)
    {
        return;
    }
    // 近平面为 0 会让深度精度塌掉（并且 w 可能为 0 → 除零 → NaN 坐标散到整个
    // 场景），所以夹到一个极小正数而不是照单全收。
    m_RenderCamera->m_Znear = (near_clip > kMinClip) ? near_clip : kMinClip;
}

float Camera::GetFarClipPlane() const
{
    return (m_RenderCamera != nullptr) ? m_RenderCamera->m_Zfar : 0.0f;
}

void Camera::SetFarClipPlane(float far_clip)
{
    if (m_RenderCamera == nullptr)
    {
        return;
    }
    // 保持 far > near：反过来的话投影矩阵会算出负的深度范围，物体前后关系整个
    // 反过来，而且不报任何错。
    const float floor_value = m_RenderCamera->m_Znear + kMinClip;
    m_RenderCamera->m_Zfar = (far_clip > floor_value) ? far_clip : floor_value;
}

float Camera::GetAspect() const
{
    return (m_RenderCamera != nullptr) ? m_RenderCamera->getAspect() : 0.0f;
}

void Camera::SetAspect(float aspect)
{
    if (m_RenderCamera != nullptr && aspect > 0.0f)
    {
        m_RenderCamera->SetAspect(aspect);
    }
}

bool Camera::GetOrthographic() const
{
    return (m_RenderCamera != nullptr) && m_RenderCamera->IsOrthographic();
}

void Camera::SetOrthographic(bool orthographic)
{
    if (m_RenderCamera != nullptr)
    {
        m_RenderCamera->SetOrthographic(orthographic);
    }
}

float Camera::GetOrthographicSize() const
{
    return (m_RenderCamera != nullptr) ? m_RenderCamera->GetOrthoHalfHeight() : 0.0f;
}

void Camera::SetOrthographicSize(float size)
{
    if (m_RenderCamera != nullptr)
    {
        m_RenderCamera->SetOrthoHalfHeight(size);
    }
}

int Camera::GetPixelWidth() const
{
    return Screen::GetWidth();
}

int Camera::GetPixelHeight() const
{
    return Screen::GetHeight();
}

// ---------------------------------------------------------------------------
// 坐标转换
//
// clip = P * V * world（列向量，w 未除）。WorldToClip / ClipToWorld 是一对。
// ---------------------------------------------------------------------------
Vector4 Camera::WorldToClip(const Vector3& world_point) const
{
    if (m_RenderCamera == nullptr)
    {
        return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    // NoJitter，不是 GetProjectionMatrix：TAA 的抖动会让射线每帧偏半个像素，
    // 用来点击拾取时表现为"点不中静止的小物体"。
    const Matrix4x4 view_projection =
        m_RenderCamera->GetProjectionMatrixNoJitter() * m_RenderCamera->GetViewMatrix();
    return view_projection * Vector4(world_point.x, world_point.y, world_point.z, 1.0f);
}

Vector3 Camera::ClipToWorld(const Vector4& clip) const
{
    if (m_RenderCamera == nullptr)
    {
        return Vector3::ZERO;
    }
    const Matrix4x4 view_projection =
        m_RenderCamera->GetProjectionMatrixNoJitter() * m_RenderCamera->GetViewMatrix();
    const Vector4 world = view_projection.inverse() * clip;
    if (std::fabs(world.w) < 1.0e-6f)
    {
        return Vector3::ZERO;
    }
    return Vector3(world.x, world.y, world.z) / world.w;
}

Vector3 Camera::WorldToScreenPoint(const Vector3& world_point) const
{
    const Vector4 clip = WorldToClip(world_point);
    if (std::fabs(clip.w) < 1.0e-6f)
    {
        // 点恰好落在相机平面上。返回 0 而不是 NaN：NaN 会顺着脚本的算术传播到
        // 整个 UI 布局，而且打印出来是 -1.#IND，比 0 难查得多。
        return Vector3::ZERO;
    }
    const Vector3 ndc = Vector3(clip.x, clip.y, clip.z) / clip.w;
    const float width = static_cast<float>(GetPixelWidth());
    const float height = static_cast<float>(GetPixelHeight());
    return Vector3((ndc.x * 0.5f + 0.5f) * width, NdcToScreenY(ndc.y, height),
                   DepthOf(world_point));
}

Vector3 Camera::WorldToViewportPoint(const Vector3& world_point) const
{
    const Vector4 clip = WorldToClip(world_point);
    if (std::fabs(clip.w) < 1.0e-6f)
    {
        return Vector3::ZERO;
    }
    const Vector3 ndc = Vector3(clip.x, clip.y, clip.z) / clip.w;
    return Vector3(ndc.x * 0.5f + 0.5f, NdcToScreenY(ndc.y, 1.0f), DepthOf(world_point));
}

Vector3 Camera::ScreenToWorldPoint(const Vector3& screen_point) const
{
    const float width = static_cast<float>(GetPixelWidth());
    const float height = static_cast<float>(GetPixelHeight());
    if (width <= 0.0f || height <= 0.0f)
    {
        return Vector3::ZERO;
    }
    const Vector2 ndc((screen_point.x / width) * 2.0f - 1.0f, ScreenToNdcY(screen_point.y, height));
    return WorldFromNdcDepth(ndc, screen_point.z);
}

Vector3 Camera::ViewportToWorldPoint(const Vector3& viewport_point) const
{
    const Vector2 ndc(viewport_point.x * 2.0f - 1.0f, ScreenToNdcY(viewport_point.y, 1.0f));
    return WorldFromNdcDepth(ndc, viewport_point.z);
}

float Camera::DepthOf(const Vector3& world_point) const
{
    if (m_RenderCamera == nullptr)
    {
        return 0.0f;
    }
    return (world_point - m_RenderCamera->position()).dotProduct(m_RenderCamera->forward());
}

Vector3 Camera::WorldFromNdcDepth(const Vector2& ndc, float depth) const
{
    Vector3 origin;
    Vector3 direction;
    if (!RayFromNdc(ndc, origin, direction))
    {
        return Vector3::ZERO;
    }
    // 要走多远才能让"沿视线的深度"等于 depth：
    //   depth = t * dot(direction, forward)
    // 画面正中间时 dot = 1，两者相等；越靠边 t 越大。按欧氏距离取 t 会让边缘的
    // 点比给定深度更远，于是 WorldToScreenPoint → ScreenToWorldPoint 回不到原
    // 点（而且只在画面边缘错，很容易漏测）。
    const float along = (m_RenderCamera != nullptr) ? direction.dotProduct(m_RenderCamera->forward()) : 1.0f;
    if (std::fabs(along) < 1.0e-6f)
    {
        return origin;
    }
    return origin + direction * (depth / along);
}

bool Camera::RayFromNdc(const Vector2& ndc, Vector3& out_origin, Vector3& out_direction) const
{
    if (m_RenderCamera == nullptr)
    {
        return false;
    }
    if (m_RenderCamera->IsOrthographic())
    {
        // 正交相机没有"会聚点"：所有射线都平行于视线，起点落在近平面上。
        //
        // **不能**用"ndc z = 0 的点 → ndc z = 1 的点"求方向：引擎构造正交矩阵时
        // 传进去的是 `(m_Zfar, m_Znear)`，深度范围是反的，ndc z 变大反而变近 ——
        // 那样算出来的方向是**负的 forward**，整条射线朝相机背后走（实测：
        // d = (0,-1,0)，而 forward = (0,1,0)）。所以方向直接取 forward，起点则
        // 沿视线平移到近平面，两种深度约定下都成立。
        const Vector3 anywhere = ClipToWorld(Vector4(ndc.x, ndc.y, 0.0f, 1.0f));
        const float depth = (anywhere - m_RenderCamera->position()).dotProduct(m_RenderCamera->forward());
        out_origin = anywhere + m_RenderCamera->forward() * (m_RenderCamera->m_Znear - depth);
        out_direction = m_RenderCamera->forward();
        return true;
    }

    // 透视：起点就是相机自身位置（Unity 同），方向朝"视线上一点"。远平面那个点
    // 两种深度约定下都成立（见 kFarNdcZ 的注释），所以这里不用判断引擎用的是
    // GL 还是 D3D 的深度。
    const Vector3 far_point = ClipToWorld(Vector4(ndc.x, ndc.y, kFarNdcZ, 1.0f));
    Vector3 direction = far_point - m_RenderCamera->position();
    const float length = direction.length();
    if (length < 1.0e-6f)
    {
        return false;
    }
    out_origin = m_RenderCamera->position();
    out_direction = direction / length;
    return true;
}

Ray* Camera::ScreenPointToRay(const Vector2& screen_point) const
{
    const float width = static_cast<float>(GetPixelWidth());
    const float height = static_cast<float>(GetPixelHeight());
    if (width <= 0.0f || height <= 0.0f)
    {
        return nullptr;
    }
    return ViewportPointToRay(Vector2(screen_point.x / width, screen_point.y / height));
}

Ray* Camera::ViewportPointToRay(const Vector2& viewport_point) const
{
    if (m_RenderCamera == nullptr)
    {
        return nullptr;
    }
    const Vector2 ndc(viewport_point.x * 2.0f - 1.0f, ScreenToNdcY(viewport_point.y, 1.0f));

    Vector3 origin;
    Vector3 direction;
    if (!RayFromNdc(ndc, origin, direction))
    {
        return nullptr;
    }

    // 射线对象按相机复用：`ScreenPointToRay` 每帧点一次的话，每次都 new 一个
    // 就是每帧一个对象。代价是两次调用拿到的是同一个 Ray —— 这和 Unity 不同
    // （Ray 是值类型），写在注释里让调用方知道要自己拷。
    auto& scratch = RayScratch();
    auto it = scratch.find(m_RenderCamera);
    if (it == scratch.end())
    {
        it = scratch.emplace(m_RenderCamera, std::make_unique<Ray>()).first;
    }
    it->second->SetOrigin(origin);
    it->second->SetDirection(direction);
    return it->second.get();
}
