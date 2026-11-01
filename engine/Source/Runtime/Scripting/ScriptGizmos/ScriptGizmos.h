#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Color/Color.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Vector3.h"

#include "ScriptGizmos.generated.h"

// =============================================================================
// `Gizmos` —— Unity 的 `UnityEngine.Gizmos` 静态面。
//
//   Gizmos.color = Color.red;
//   Gizmos.DrawLine(here, there);
//   Gizmos.DrawWireSphere(center, 1.5);
//
// 为什么是另一个门面类
// --------------------
// `Gizmos` 在 Unity 里是一个只有静态成员的静态类。引擎里没有"静态类"这回事：
// 要能被脚本看见就必须 ZCLASS（也就是必须派生 Object），于是和 `Runtime::Color`
// / `Runtime::Time` / `Runtime::Mathf` 一样，做成"不可实例化的命名空间类"。
//
// 后端
// ----
// 所有绘制都进 `DebugDrawManager` 的同一个 group（"gizmos"），生命周期是一帧
// （`k_debug_draw_one_frame`）—— 和 Unity 一样：gizmo 不会自己留下来，必须每帧
// 重画。这条不是顺手选的：脚本几乎总是在 OnUpdate / OnDrawGizmos 里画，如果
// 忘了清，画面上会堆满上一秒的残影，而"我没画它但它还在"比"我画了它却没出现"
// 难查得多。
//
// ⚠️ 非 Vulkan 构建（`__APPLE__` 或 `!Z_HAS_VULKAN`）下
// `DebugDrawManager::TryGetOrCreateDebugDrawGroup` **恒返回 nullptr**
// （DebugDrawManager.cpp 顶部有一整段空实现）。所以每个入口都必须判空降级成
// no-op，而不是崩 —— 一个只在 macOS 上炸的 Gizmos.DrawLine 是最糟的那种 bug。
//
// 还没做的 Unity 成员
// --------------------
//   * `DrawIcon` —— 没有图标资源管线。
//   * `DrawMesh` / `DrawGUITexture` —— 需要一次性的网格 / 贴图句柄，绑定层搬不动。
//     （`DrawFrustum` 不需要：它是 8 个角点 + 12 条线，纯几何，不碰资源。）
//   * `DrawSphere` 与 `DrawWireSphere` 目前**画出来是一样的**：后端的球体图元
//     没有 FillMode（`DebugDrawSphere` 结构体没有该字段，`DrawWireFrameObject`
//     一律按线框渲染）。API 分开保留，等后端有实心光栅化路径时再接上；在那之前
//     两者行为一致的这件事写在 `.md` 里，不藏在实现里。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `Gizmos` 静态面。不该被实例化（ZCLASS 要求它派生 Object）。
    ZCLASS()
    class Gizmos : public Object
    {
        ZGENERATED_BODY(Gizmos)

    public:
        // ---- 状态 ------------------------------------------------------

        /// 后续所有绘制用的颜色。Unity 的默认值是白色 (1,1,1,1)。
        ZFUNCTION()
        static ::Color GetColor();
        ZFUNCTION()
        static void SetColor(const ::Color& color);
        ZSCRIPTPROPERTY(Name = "color", Getter = "GetColor", Setter = "SetColor")

        /// 后续所有绘制要过的变换。Unity 的默认值是单位矩阵。
        ///
        /// 这里做的是**点变换**（w=1），不是方向变换：Unity 的 `Gizmos.matrix`
        /// 会把 gizmo 的位置整体搬走，而 `DrawRay` 的方向同样要跟着旋转 —— 因为
        /// 矩阵里的旋转分量对方向有效、平移分量对方向无效，`Matrix4x4 * Vector3`
        /// 的齐次实现恰好就是这个语义。
        ZFUNCTION()
        static ::Matrix4x4 GetMatrix();
        ZFUNCTION()
        static void SetMatrix(const ::Matrix4x4& matrix);
        ZSCRIPTPROPERTY(Name = "matrix", Getter = "GetMatrix", Setter = "SetMatrix")

        // ---- 线 --------------------------------------------------------

        /// 从 `from` 到 `to` 的一条线段。
        ZFUNCTION()
        static void DrawLine(const ::Vector3& from, const ::Vector3& to);

        /// 从 `from` 出发、沿 `direction` 的一条射线。
        ///
        /// ⚠️ `direction` 是**方向**不是终点：终点是 `from + direction`。Unity
        /// 的 `DrawRay(Ray)` 重载也是这个语义（Ray.direction 不是长度终点）。
        /// 传终点进来会画出一条两倍长的线，而且从 `from` 指到完全不相干的地方。
        ZFUNCTION()
        static void DrawRay(const ::Vector3& from, const ::Vector3& direction);

        // ---- 球 --------------------------------------------------------

        ZFUNCTION()
        static void DrawWireSphere(const ::Vector3& center, float radius);

        /// 实心球。见文件头：后端目前没有实心路径，画出来与 `DrawWireSphere` 相同。
        ZFUNCTION()
        static void DrawSphere(const ::Vector3& center, float radius);

        // ---- 盒 --------------------------------------------------------

        /// 线框盒。`size` 是**总尺寸**（Unity 语义），不是半长 —— 内部再折半。
        ZFUNCTION()
        static void DrawWireCube(const ::Vector3& center, const ::Vector3& size);

        /// 实心盒。12 个 `_FillMode_solid` 三角形（每面两个）—— 与
        /// `DrawWireCube` 走的是**不同的图元类型**（triangle vs box），不是同一
        /// 份数据换个参数，所以计数上能区分，测试也就能区分。
        ZFUNCTION()
        static void DrawCube(const ::Vector3& center, const ::Vector3& size);

        // ---- 视锥 ------------------------------------------------------

        /// 相机视锥的**线框**：近平面 4 条 + 远平面 4 条 + 4 条侧棱，共 12 条线。
        /// Unity 的 `Gizmos.DrawFrustum`。
        ///
        /// ⚠️ 两条与 Unity **数字不同但语义相同**的约定：
        ///   * 张开方向是 **forward = -Y**（本引擎的语义前，与 `Vector3.forward`
        ///     / `Transform::GetForward()` 一致），不是 Unity 的 +Z。照抄 +Z 会得到
        ///     一个朝天上指的视锥 —— 而且它仍然是 12 条线、仍然是个台体，只数图元
        ///     数完全看不出来。
        ///   * up = +Z、right = forward × up = **-X**，同样与 `Vector3.up` /
        ///     `Vector3.right` 对齐（`TestAxisConventionAgreement` 那套）。
        ///
        /// `fov` 是**竖直**视野角、**度**（Unity 同），`aspect` = 宽/高。
        /// 参数退化（fov 不在 (0,180)、near<=0、far<=near、aspect<=0）时**一条线
        /// 都不画**：与其把 inf / NaN 写进顶点缓冲污染整条 draw call，不如什么都不
        /// 画 —— "某个 gizmo 没出现"比"整屏乱掉"好查得多。
        ZFUNCTION()
        static void DrawFrustum(const ::Vector3& center,
                                float fov,
                                float far_clip_plane,
                                float near_clip_plane,
                                float aspect);
    };
}  // namespace Runtime
