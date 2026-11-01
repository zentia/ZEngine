#include "Runtime/Scripting/ScriptGizmos/ScriptGizmos.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Core/Math/Vector4.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawGroup.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"

#include <EASTL/string.h>

#include <cmath>

namespace
{
    /// 所有 gizmo 进同一个 group。
    ///
    /// 不按调用方分：分组的用途是后端按 no-depth-test 分桶渲染，而脚本画的东西
    /// 全是同一类（深度测试的、一帧的）。多开一个 group 只会让"我画的东西在
    /// 哪"多一层要查的地方。
    constexpr const char* kGizmoGroupName = "gizmos";

    /// `AddBox` 的 rotate 参数是 (axis.xyz, angle_radians)，不是四元数 ——
    /// LevelDebugger 传的是 `Vector4(1,0,0,0)`（绕 X 转 0 弧度），写成四元数
    /// 的 (0,0,0,1) 会得到一个非零的旋转轴反过来用。
    const ::Vector4 kNoRotation {1.0f, 0.0f, 0.0f, 0.0f};

    /// Unity 的 gizmo 是**深度测试**的（被物体挡住就看不见），所以这里显式传
    /// false —— 与 `no_depth_test` 的默认值相反，照默认值写会得到"永远浮在最
    /// 前面"的效果，那不是 Gizmos 而更像 Unity 的 Handles。
    constexpr bool kDepthTested = false;

    ::DebugDrawGroup* GetGizmoGroup()
    {
        auto* manager = GET_SYSTEM(DebugDrawManager);
        if (manager == nullptr)
        {
            // 引擎没跑渲染（纯逻辑测试 / 专用服务器）：画不了，但不是错误。
            return nullptr;
        }
        // 非 Vulkan 构建下这个调用恒返回 nullptr（见 ScriptGizmos.h 的说明）。
        return manager->TryGetOrCreateDebugDrawGroup(kGizmoGroupName);
    }

    ::Vector4 ToVec4(const ::Color& c)
    {
        return ::Vector4(c.r, c.g, c.b, c.a);
    }

    /// ⚠️ 这两个状态**必须**是文件级静态，不能写成函数内的 `static` 局部变量：
    /// 那样 `GetColor` 和 `SetColor` 会各自持有一个互不相干的对象，写进去的值
    /// 读不出来 —— 编译能过、运行不崩、`Gizmos.color` 看起来只是"不生效"。
    ::Color s_Color {1.0f, 1.0f, 1.0f, 1.0f};
    ::Matrix4x4 s_Matrix {::Matrix4x4::IDENTITY};
}  // namespace

namespace Runtime
{
    // ---- 状态 -------------------------------------------------------------
    //
    // 两个都是进程级的静态：Unity 的 `Gizmos.color` / `Gizmos.matrix` 也是全局
    // 的（不是 per-behaviour）。没有加锁 —— 和引擎其它静态面（Time / Input）
    // 一致，脚本只在主线程跑。

    ::Color Gizmos::GetColor()
    {
        return s_Color;
    }

    void Gizmos::SetColor(const ::Color& color)
    {
        s_Color = color;
    }

    ::Matrix4x4 Gizmos::GetMatrix()
    {
        return s_Matrix;
    }

    void Gizmos::SetMatrix(const ::Matrix4x4& matrix)
    {
        s_Matrix = matrix;
    }

    // ---- 线 ---------------------------------------------------------------

    void Gizmos::DrawLine(const ::Vector3& from, const ::Vector3& to)
    {
        auto* group = GetGizmoGroup();
        if (group == nullptr)
        {
            return;
        }

        const ::Matrix4x4 matrix = GetMatrix();
        const ::Vector4 color = ToVec4(GetColor());

        group->AddLine(matrix * from, matrix * to, color, color,
                       k_debug_draw_one_frame, kDepthTested);
    }

    void Gizmos::DrawRay(const ::Vector3& from, const ::Vector3& direction)
    {
        auto* group = GetGizmoGroup();
        if (group == nullptr)
        {
            return;
        }

        const ::Matrix4x4 matrix = GetMatrix();
        const ::Vector4 color = ToVec4(GetColor());

        // 终点是 from + direction（`direction` 是方向）。矩阵对两个点用同一条
        // 变换，所以旋转/缩放会作用在这条射线上，平移不会把"方向"改成别的东西。
        const ::Vector3 end = from + direction;
        group->AddLine(matrix * from, matrix * end, color, color,
                       k_debug_draw_one_frame, kDepthTested);
    }

    // ---- 球 ---------------------------------------------------------------

    void Gizmos::DrawWireSphere(const ::Vector3& center, float radius)
    {
        auto* group = GetGizmoGroup();
        if (group == nullptr)
        {
            return;
        }

        const ::Matrix4x4 matrix = GetMatrix();
        group->AddSphere(matrix * center, radius, ToVec4(GetColor()),
                         k_debug_draw_one_frame, kDepthTested);
    }

    void Gizmos::DrawSphere(const ::Vector3& center, float radius)
    {
        // 后端没有实心球体图元（见头文件说明）：目前与 DrawWireSphere 走同一条
        // 路径。分开保留 API，等后端有实心光栅化再接。
        DrawWireSphere(center, radius);
    }

    // ---- 盒 ---------------------------------------------------------------

    void Gizmos::DrawWireCube(const ::Vector3& center, const ::Vector3& size)
    {
        auto* group = GetGizmoGroup();
        if (group == nullptr)
        {
            return;
        }

        const ::Matrix4x4 matrix = GetMatrix();
        // Unity 的 `size` 是总尺寸；AddBox 要的是半长。
        const ::Vector3 half = size * 0.5f;

        group->AddBox(matrix * center, half, kNoRotation, ToVec4(GetColor()),
                      k_debug_draw_one_frame, kDepthTested);
    }

    void Gizmos::DrawCube(const ::Vector3& center, const ::Vector3& size)
    {
        auto* group = GetGizmoGroup();
        if (group == nullptr)
        {
            return;
        }

        const ::Matrix4x4 matrix = GetMatrix();
        const ::Vector4 color = ToVec4(GetColor());
        const ::Vector3 half = size * 0.5f;

        // 八个角。顺序固定成 [(-x,-y,-z), (+x,-y,-z), (+x,+y,-z), (-x,+y,-z),
        //                     (-x,-y,+z), (+x,-y,+z), (+x,+y,+z), (-x,+y,+z)]，
        // 下面的面索引才对得上。
        ::Vector3 corner[8] = {
            {center.x - half.x, center.y - half.y, center.z - half.z},
            {center.x + half.x, center.y - half.y, center.z - half.z},
            {center.x + half.x, center.y + half.y, center.z - half.z},
            {center.x - half.x, center.y + half.y, center.z - half.z},
            {center.x - half.x, center.y - half.y, center.z + half.z},
            {center.x + half.x, center.y - half.y, center.z + half.z},
            {center.x + half.x, center.y + half.y, center.z + half.z},
            {center.x - half.x, center.y + half.y, center.z + half.z},
        };
        for (::Vector3& c : corner)
        {
            c = matrix * c;
        }

        // 六个面，每个面按外法线看是逆时针。绕序错了在背面剔除下会整面消失，
        // 而且是"某些角度看不见"这种最难复现的症状。
        static const int faces[6][4] = {
            {0, 3, 2, 1},  // -Z
            {4, 5, 6, 7},  // +Z
            {0, 4, 7, 3},  // -X
            {1, 2, 6, 5},  // +X
            {0, 1, 5, 4},  // -Y
            {3, 7, 6, 2},  // +Y
        };

        // ⚠️ 不能用 `AddQuad(..., _FillMode_solid)`：后端只在
        // `WriteTriangleData` 里遍历 `m_Triangles`，`m_Quads` 的实心图元**从来
        // 不会被写出来**（GetTriangleCount 也只数 m_Triangles）。那样写编译能过、
        // 计数为 0、屏幕上什么都没有 —— 一个完全静默的失败。所以一个面拆两个
        // 三角形，共 12 个。
        for (const auto& face : faces)
        {
            group->AddTriangle(corner[face[0]],
                               corner[face[1]],
                               corner[face[2]],
                               color,
                               color,
                               color,
                               k_debug_draw_one_frame,
                               kDepthTested,
                               _FillMode_solid);
            group->AddTriangle(corner[face[0]],
                               corner[face[2]],
                               corner[face[3]],
                               color,
                               color,
                               color,
                               k_debug_draw_one_frame,
                               kDepthTested,
                               _FillMode_solid);
        }
    }

    // ---- 视锥 ---------------------------------------------------------------

    void Gizmos::DrawFrustum(const ::Vector3& center,
                             float fov,
                             float far_clip_plane,
                             float near_clip_plane,
                             float aspect)
    {
        auto* group = GetGizmoGroup();
        if (group == nullptr)
        {
            return;
        }

        // 退化参数：一条线都不画，而不是把 inf / NaN 交给后端。
        // `fov >= 180` 时 tan(fov/2) 发散，`near <= 0` 时近平面跑到眼睛后面，
        // `far <= near` 时台体反过来 —— 三种都只会产出一个没人看得懂的形状。
        if (!(fov > 0.0f) || fov >= 180.0f || !(near_clip_plane > 0.0f) ||
            !(far_clip_plane > near_clip_plane) || !(aspect > 0.0f))
        {
            return;
        }

        const ::Matrix4x4 matrix = GetMatrix();
        const ::Vector4 color = ToVec4(GetColor());

        // ⚠️ 语义轴，不是 Unity 的分量：前是 -Y、上是 +Z、右是 -X。这三个常量与
        // `Vector3.forward` / `up` / `right` 必须是同一套（FR7 就是钉这一条的）。
        const ::Vector3& forward = ::Vector3::NEGATIVE_UNIT_Y;
        const ::Vector3& up = ::Vector3::UNIT_Z;
        const ::Vector3 right = forward.crossProduct(up);

        const float tan_half_fov = std::tan(Math::DegreesToRadians(fov) * 0.5f);

        // 四个角的顺序固定 (-r,-u) (+r,-u) (+r,+u) (-r,+u)：下面用 (i, i+1)%4
        // 连边，绕序错了会变成两个交叉的蝴蝶面而不是一个矩形。
        ::Vector3 near_c[4];
        ::Vector3 far_c[4];
        for (int i = 0; i < 4; ++i)
        {
            const float sx = ((i == 1) || (i == 2)) ? 1.0f : -1.0f;
            const float sy = (i >= 2) ? 1.0f : -1.0f;

            const ::Vector3 n_off = right * (sx * near_clip_plane * tan_half_fov * aspect) +
                                    up * (sy * near_clip_plane * tan_half_fov);
            const ::Vector3 f_off = right * (sx * far_clip_plane * tan_half_fov * aspect) +
                                    up * (sy * far_clip_plane * tan_half_fov);

            near_c[i] = matrix * (center + forward * near_clip_plane + n_off);
            far_c[i] = matrix * (center + forward * far_clip_plane + f_off);
        }

        auto add_edge = [group, &color](const ::Vector3& a, const ::Vector3& b) {
            group->AddLine(a, b, color, color, k_debug_draw_one_frame, kDepthTested);
        };

        for (int i = 0; i < 4; ++i)
        {
            const int j = (i + 1) % 4;
            add_edge(near_c[i], near_c[j]);  // 近平面
        }
        for (int i = 0; i < 4; ++i)
        {
            const int j = (i + 1) % 4;
            add_edge(far_c[i], far_c[j]);  // 远平面
        }
        for (int i = 0; i < 4; ++i)
        {
            add_edge(near_c[i], far_c[i]);  // 侧棱
        }
    }
}  // namespace Runtime
