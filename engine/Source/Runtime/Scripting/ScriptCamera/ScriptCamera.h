#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <vector>

#include "ScriptCamera.generated.h"

// RenderCamera 只当指针用，前向声明即可（生成代码不会为它实例化 Converter）。
class RenderCamera;

// 脚本侧的相机（Unity 的 `UnityEngine.Camera`）与射线（`UnityEngine.Ray`）。
//
// 为什么包的是 RenderCamera 而不是 CameraComponent
// ------------------------------------------------
// 投影所需的一切 —— fov、aspect、near/far、正交与否、view/projection 矩阵 ——
// 都活在 `RenderCamera` 上。`CameraComponent` 只是"场景里那个标记了 main 的组
// 件"，它自己也指向一份 CameraParameter，但真正被渲染管线用的状态是
// RenderCamera 的。包错一层的话 `camera.fieldOfView = 60` 改不到渲染用的那个
// 值，而画面毫无变化 —— 静默的错行为，不是报错。
//
// `Camera.main` 是 `Camera.GetMain()`
// -----------------------------------
// Unity 里 `Camera.main` 是**静态属性**。ZSCRIPTPROPERTY 的 getter 不允许
// static，静态属性做不出来（和 `Screen` / `Time` / `Random` 同一处取舍），所以
// 落成静态方法。
//
// 屏幕坐标：原点在**左下角**
// ---------------------------
// Unity 的 `Input.mousePosition` 和 `WorldToScreenPoint` 都以左下角为原点、y 向
// 上。操作系统的鼠标事件（以及 Win32 / Cocoa 的原生坐标）以**左上**角为原点。
// 这里跟 Unity：屏幕坐标一律左下原点。拿原生鼠标坐标直接喂进
// `ScreenPointToRay` 会得到上下翻转的结果，而且不报错 —— 这是移植代码时最常
// 踩的一处。
//
// `WorldToScreenPoint` 的 z 是"沿视线方向的深度"
// ----------------------------------------------
// 不是到相机的欧氏距离。取沿 forward 的投影，`ScreenToWorldPoint` 才和它互为
// 逆运算（见 .cpp 里的推导）。Unity 同。

ZCLASS()
class Ray : public Object
{
    ZGENERATED_BODY(Ray)

public:
    Ray() = default;

    ZFUNCTION()
    Vector3 GetOrigin() const { return m_Origin; }

    ZFUNCTION()
    void SetOrigin(const Vector3& origin) { m_Origin = origin; }

    /// 方向**不**在 SetDirection 里归一化（Unity 的 Ray 也不）：脚本可以自己
    /// 表达"长度 5 的方向"。`Camera.ScreenPointToRay` 产出的方向是归一化的。
    ZFUNCTION()
    Vector3 GetDirection() const { return m_Direction; }

    ZFUNCTION()
    void SetDirection(const Vector3& direction) { m_Direction = direction; }

    /// 射线上距离 origin 为 `distance` 的点。
    ZFUNCTION()
    Vector3 GetPoint(float distance) const { return m_Origin + m_Direction * distance; }

    ZSCRIPTPROPERTY(Name="origin", Getter="GetOrigin", Setter="SetOrigin")
    ZSCRIPTPROPERTY(Name="direction", Getter="GetDirection", Setter="SetDirection")

private:
    Vector3 m_Origin;
    Vector3 m_Direction{Vector3::NEGATIVE_UNIT_Y};
};

ZCLASS()
class Camera : public Object
{
    ZGENERATED_BODY(Camera)

public:
    Camera() = default;

    /// 绑定到一台 RenderCamera。C++ 侧的装配入口，**不**导出给脚本：脚本只能从
    /// `GetMain()` / `GetAllCameras()` 拿 Camera。
    void Bind(RenderCamera* render_camera) { m_RenderCamera = render_camera; }
    RenderCamera* GetRenderCamera() const { return m_RenderCamera; }

    /// 这台 Camera 是否指向一台真实存在的 RenderCamera。
    ZFUNCTION()
    bool IsValid() const { return m_RenderCamera != nullptr; }

    // ---- 静态：找到相机 -------------------------------------------------

    /// 游戏视口正在用的那台相机。没有 RenderSystem 时返回 null。
    ///
    /// 与 Unity 的差别：Unity 的 `Camera.main` 是"打了 MainCamera 标签的那台"，
    /// 每次调用都要遍历一遍（所以官方建议缓存）。ZEngine 的游戏视口就一台相
    /// 机，`GetMain()` 直接取它 —— 同样的结果，不用每帧遍历。
    ZFUNCTION()
    static Camera* GetMain();

    /// 相机台数（游戏 + 编辑器视口，通常 2）。
    ZFUNCTION()
    static int GetCount();

    /// 按索引取相机，越界返回 null（**不**夹紧到第 0 台：脚本里
    /// `for (i = 0; i <= count; ++i)` 写错时，夹紧会让它静默地又拿到第 0 台）。
    ZFUNCTION()
    static Camera* GetCameraAt(int index);

    /// 全部相机。顺序来自 RenderSystem，不保证稳定。
    ZFUNCTION()
    static std::vector<Camera*> GetAllCameras();

    // ---- 投影参数 -------------------------------------------------------

    /// 垂直视场角，**度**（Unity 的 `fieldOfView` 是垂直的，不是水平的）。
    ZFUNCTION()
    float GetFieldOfView() const;

    /// 设置垂直视场角。受引擎自身 fovx 范围 [4, 120] 的夹取：极端宽高比下
    /// （aspect > 约 3.2）想表达的垂直 fov 需要的水平 fov 会超出上限，此时回来
    /// 的垂直 fov 比设进去的小。这是已有的引擎夹取，不是门面的 bug。
    ZFUNCTION()
    void SetFieldOfView(float fov);

    ZFUNCTION()
    float GetNearClipPlane() const;

    ZFUNCTION()
    void SetNearClipPlane(float near_clip);

    ZFUNCTION()
    float GetFarClipPlane() const;

    ZFUNCTION()
    void SetFarClipPlane(float far_clip);

    ZFUNCTION()
    float GetAspect() const;

    ZFUNCTION()
    void SetAspect(float aspect);

    ZFUNCTION()
    bool GetOrthographic() const;

    ZFUNCTION()
    void SetOrthographic(bool orthographic);

    /// 正交模式下**半高**（世界单位）。Unity 的 `orthographicSize` 同样是半高。
    ZFUNCTION()
    float GetOrthographicSize() const;

    ZFUNCTION()
    void SetOrthographicSize(float size);

    /// 渲染目标的像素宽（没有 RenderTexture 时是屏幕宽）。
    ZFUNCTION()
    int GetPixelWidth() const;

    ZFUNCTION()
    int GetPixelHeight() const;

    // ---- 坐标转换 -------------------------------------------------------

    /// 世界坐标 → 屏幕坐标（左下原点，像素）。z 是沿视线方向的深度。
    ZFUNCTION()
    Vector3 WorldToScreenPoint(const Vector3& world_point) const;

    /// 屏幕坐标（左下原点，像素，z = 沿视线方向的深度）→ 世界坐标。
    /// 与 `WorldToScreenPoint` 互为逆运算。
    ZFUNCTION()
    Vector3 ScreenToWorldPoint(const Vector3& screen_point) const;

    /// 世界坐标 → 视口坐标（左下原点，**0..1**）。
    ZFUNCTION()
    Vector3 WorldToViewportPoint(const Vector3& world_point) const;

    /// 视口坐标（0..1，z = 沿视线方向的深度）→ 世界坐标。
    ZFUNCTION()
    Vector3 ViewportToWorldPoint(const Vector3& viewport_point) const;

    /// 屏幕点 → 射线。方向**已归一化**。
    ZFUNCTION()
    Ray* ScreenPointToRay(const Vector2& screen_point) const;

    /// 视口点（0..1）→ 射线。方向**已归一化**。
    ZFUNCTION()
    Ray* ViewportPointToRay(const Vector2& viewport_point) const;

    // ---- 屏幕 <-> 视口 ---------------------------------------------------
    //
    // Unity 有四个互转（World/Screen/Viewport 各两个方向），之前只有穿过相机的
    // 那四条，屏幕与视口之间得先绕道世界 —— 一次额外的反投影，白花一次矩阵求
    // 逆，还把误差带进去。

    /// 屏幕（像素，左下原点）→ 视口（0..1）。z 原样带过去（它是深度，不是坐标）。
    ZFUNCTION()
    Vector3 ScreenToViewportPoint(const Vector3& screen_point) const;

    /// 视口（0..1）→ 屏幕（像素，左下原点）。与上式互为逆运算。
    ZFUNCTION()
    Vector3 ViewportToScreenPoint(const Vector3& viewport_point) const;

    // ---- 矩阵（Unity 的 `worldToCameraMatrix` / `cameraToWorldMatrix` /
    //      `projectionMatrix`）---------------------------------------------

    /// 世界 → 相机空间。Unity 的 `camera.worldToCameraMatrix`。
    ZFUNCTION()
    Matrix4x4 GetWorldToCameraMatrix() const;

    /// 相机空间 → 世界，即上式的逆。Unity 的 `camera.cameraToWorldMatrix`。
    ///
    /// 手写一个"由 position + rotation 组装"的版本是**错**的：LWC 下 view 矩阵
    /// 里还带一块相对当前 tile 的平移，组装出来的矩阵和渲染用的不是同一张，
    /// 脚本拿它做坐标变换会和画面对不上。所以直接求逆。
    ZFUNCTION()
    Matrix4x4 GetCameraToWorldMatrix() const;

    /// 投影矩阵（含 TAA 抖动，如果开了的话）。
    ZFUNCTION()
    Matrix4x4 GetProjectionMatrix() const;

    /// 去掉 TAA 抖动的那张投影矩阵。Unity 里对应的是
    /// `nonJitteredProjectionMatrix`：拾取、UI 跟随这类"必须和画面严格对齐"的
    /// 计算要用它，否则每帧差半个像素。
    ZFUNCTION()
    Matrix4x4 GetNonJitteredProjectionMatrix() const;

    /// Unity 的 `ResetAspect()`：aspect 回到当前渲染目标的宽高比（之前
    /// `SetAspect()` 设进去的自定义值作废）。
    ZFUNCTION()
    void ResetAspect();

    /// Unity 的 `pixelRect`：这台相机在渲染目标上占的那块像素区域。
    ///
    /// ⚠️ Unity 返回 `Rect`，ZEngine 没有 `Rect` 值类型可以过脚本边界，所以按
    /// `(x, y, width, height)` 塞进 `Vector4`（左下原点，像素）。这是与 Unity 的
    /// 形状差异，不是语义差异。
    ZFUNCTION()
    Vector4 GetPixelRect() const;

    // ---- 朝向 ------------------------------------------------------------
    //
    // Unity 里这些是 `camera.transform.position/forward/up/right`。Camera 门面
    // 本身不是组件、拿不到 Transform，所以按 RenderCamera 自己的状态给。

    ZFUNCTION()
    Vector3 GetPosition() const;
    ZFUNCTION()
    Vector3 GetForward() const;
    ZFUNCTION()
    Vector3 GetUp() const;
    ZFUNCTION()
    Vector3 GetRight() const;

    ZSCRIPTPROPERTY(Name="fieldOfView", Getter="GetFieldOfView", Setter="SetFieldOfView")
    ZSCRIPTPROPERTY(Name="nearClipPlane", Getter="GetNearClipPlane", Setter="SetNearClipPlane")
    ZSCRIPTPROPERTY(Name="farClipPlane", Getter="GetFarClipPlane", Setter="SetFarClipPlane")
    ZSCRIPTPROPERTY(Name="aspect", Getter="GetAspect", Setter="SetAspect")
    ZSCRIPTPROPERTY(Name="orthographic", Getter="GetOrthographic", Setter="SetOrthographic")
    ZSCRIPTPROPERTY(Name="orthographicSize", Getter="GetOrthographicSize", Setter="SetOrthographicSize")
    ZSCRIPTPROPERTY(Name="pixelWidth", Getter="GetPixelWidth")
    ZSCRIPTPROPERTY(Name="pixelHeight", Getter="GetPixelHeight")
    // 只读：写入要动 RenderCamera 的 implicitWorldToCameraMatrix /
    // implicitAspect 那套"谁最后写谁算"的状态机，照 Unity 开放一个 setter 却只
    // 改到一半比不给 setter 更糟。
    ZSCRIPTPROPERTY(Name="worldToCameraMatrix", Getter="GetWorldToCameraMatrix")
    ZSCRIPTPROPERTY(Name="cameraToWorldMatrix", Getter="GetCameraToWorldMatrix")
    ZSCRIPTPROPERTY(Name="projectionMatrix", Getter="GetProjectionMatrix")
    ZSCRIPTPROPERTY(Name="pixelRect", Getter="GetPixelRect")
    ZSCRIPTPROPERTY(Name="position", Getter="GetPosition")
    ZSCRIPTPROPERTY(Name="forward", Getter="GetForward")
    ZSCRIPTPROPERTY(Name="up", Getter="GetUp")
    ZSCRIPTPROPERTY(Name="right", Getter="GetRight")

private:
    /// 世界 → 裁剪空间（齐次），未除以 w。
    Vector4 WorldToClip(const Vector3& world_point) const;
    /// 裁剪空间（齐次）→ 世界。
    Vector3 ClipToWorld(const Vector4& clip) const;
    /// 沿视线方向的深度 —— 不是到相机的欧氏距离。取它，`ScreenToWorldPoint`
    /// 才和 `WorldToScreenPoint` 互为逆运算（Unity 同）。
    float DepthOf(const Vector3& world_point) const;
    /// NDC(x, y) + 沿视线的深度 → 世界点。
    Vector3 WorldFromNdcDepth(const Vector2& ndc, float depth) const;
    /// NDC(x, y) → 射线起点 + 单位方向。退化（除零 / 零长度）时返回 false。
    bool RayFromNdc(const Vector2& ndc, Vector3& out_origin, Vector3& out_direction) const;

    RenderCamera* m_RenderCamera{nullptr};
};
