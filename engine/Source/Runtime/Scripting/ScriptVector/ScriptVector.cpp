#include "Runtime/Scripting/ScriptVector/ScriptVector.h"

#include "Runtime/Scripting/ScriptMath/ScriptMath.h"
// `deltaTime <= 0` 时退回 Unity 的默认值 `Time.deltaTime`。
#include "Runtime/Scripting/ScriptTime/ScriptTime.h"

#include <algorithm>
#include <cmath>
#include <limits>

// 实现全部转发 `VectorMath` / `QuaternionMath` —— 这两份门面只负责 Unity 的拼写，
// 算法不在这里复制一份（复制的两份迟早漂移，上次就是 Lerp 夹不夹 t 的问题）。

namespace Runtime
{
    namespace
    {
        ::Vector3 Make(float x, float y, float z)
        {
            return ::Vector3(x, y, z);
        }
    }  // namespace

    // ---- Vector3：常量 ----

    ::Vector3 Vector3::GetZero()
    {
        return Make(0.0f, 0.0f, 0.0f);
    }

    ::Vector3 Vector3::GetOne()
    {
        return Make(1.0f, 1.0f, 1.0f);
    }

    // Z-up：上是 +Z，前是 -Y。见 ScriptVector.h 文件头。
    ::Vector3 Vector3::GetUp()
    {
        return Make(0.0f, 0.0f, 1.0f);
    }

    ::Vector3 Vector3::GetDown()
    {
        return Make(0.0f, 0.0f, -1.0f);
    }

    ::Vector3 Vector3::GetForward()
    {
        return Make(0.0f, -1.0f, 0.0f);
    }

    ::Vector3 Vector3::GetBack()
    {
        return Make(0.0f, 1.0f, 0.0f);
    }

    // ⚠️ right 是 **-X**，left 是 +X：本引擎 local +X 是"左"（right = forward × up
    // = (-Y) × (+Z) = -X），和 `Transform::GetRight()` 同一个约定
    // （UnityParitySmokeTest 的 S4/S5）。照 Unity 写成 right = +X，脚本里
    // `Vector3.right` 与 `transform.right` 就会指相反的两边，而且两边都"对"。
    ::Vector3 Vector3::GetRight()
    {
        return Make(-1.0f, 0.0f, 0.0f);
    }

    ::Vector3 Vector3::GetLeft()
    {
        return Make(1.0f, 0.0f, 0.0f);
    }

    ::Vector3 Vector3::GetPositiveInfinity()
    {
        const float inf = std::numeric_limits<float>::infinity();
        return Make(inf, inf, inf);
    }

    ::Vector3 Vector3::GetNegativeInfinity()
    {
        const float inf = -std::numeric_limits<float>::infinity();
        return Make(inf, inf, inf);
    }

    // ---- Vector3：运算 ----

    float Vector3::Distance(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::Distance(a, b);
    }

    float Vector3::SqrDistance(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::SqrDistance(a, b);
    }

    float Vector3::Dot(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::Dot(a, b);
    }

    ::Vector3 Vector3::Cross(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::Cross(a, b);
    }

    ::Vector3 Vector3::Lerp(const ::Vector3& a, const ::Vector3& b, float t)
    {
        // Unity 的 `Vector3.Lerp` 夹 t，`LerpUnclamped` 才不夹。`VectorMath.Lerp`
        // 不夹（历史原因），所以这里自己夹一层。
        float clamped = t;
        if (clamped < 0.0f)
        {
            clamped = 0.0f;
        }
        if (clamped > 1.0f)
        {
            clamped = 1.0f;
        }
        return LerpUnclamped(a, b, clamped);
    }

    ::Vector3 Vector3::LerpUnclamped(const ::Vector3& a, const ::Vector3& b, float t)
    {
        return VectorMath::Lerp(a, b, t);
    }

    float Vector3::Magnitude(const ::Vector3& v)
    {
        return VectorMath::Magnitude(v);
    }

    float Vector3::SqrMagnitude(const ::Vector3& v)
    {
        return VectorMath::SqrMagnitude(v);
    }

    ::Vector3 Vector3::Normalize(const ::Vector3& v)
    {
        return VectorMath::Normalize(v);
    }

    ::Vector3 Vector3::Scale(const ::Vector3& a, const ::Vector3& b)
    {
        // Unity 的 `Vector3.Scale` 是逐分量相乘 —— 转发 `VectorMath::Mul`（它的注释
        // 写的就是"Unity 的 Vector3.Scale"），不是标量的那个 `Scale`。
        return VectorMath::Mul(a, b);
    }

    ::Vector3 Vector3::ScaleUniform(const ::Vector3& v, float scalar)
    {
        return VectorMath::Scale(v, scalar);
    }

    ::Vector3 Vector3::MoveTowards(const ::Vector3& a, const ::Vector3& b, float max_distance)
    {
        return VectorMath::MoveTowards(a, b, max_distance);
    }

    ::Vector3 Vector3::ClampMagnitude(const ::Vector3& v, float max_length)
    {
        return VectorMath::ClampMagnitude(v, max_length);
    }

    Vector3SmoothDampResult Vector3::SmoothDamp(const ::Vector3& current, const ::Vector3& target,
                                                const ::Vector3& current_velocity,
                                                float smooth_time, float max_speed,
                                                float delta_time)
    {
        if (!(delta_time > 0.f))
        {
            delta_time = Time::GetDeltaTime();
        }
        Vector3SmoothDampResult out;
        // ⚠️ 三个分量**各自**跑标量版 —— Unity 的 `Vector3.SmoothDamp` 就是这么做
        // 的，于是 `maxSpeed` 是"每个分量的上限"，不是合成速度的上限（对角方向
        // 可以到 √3 倍）。别"修"成整向量限速，那和 Unity 不是同一条曲线。
        out.value.x = SmoothDampDetail::Step(current.x, target.x, current_velocity.x, smooth_time,
                                             max_speed, delta_time, out.velocity.x);
        out.value.y = SmoothDampDetail::Step(current.y, target.y, current_velocity.y, smooth_time,
                                             max_speed, delta_time, out.velocity.y);
        out.value.z = SmoothDampDetail::Step(current.z, target.z, current_velocity.z, smooth_time,
                                             max_speed, delta_time, out.velocity.z);
        return out;
    }

    ::Vector3 Vector3::Reflect(const ::Vector3& v, const ::Vector3& normal)
    {
        return VectorMath::Reflect(v, normal);
    }

    ::Vector3 Vector3::Project(const ::Vector3& v, const ::Vector3& on_normal)
    {
        return VectorMath::Project(v, on_normal);
    }

    float Vector3::Angle(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::Angle(a, b);
    }

    ::Vector3 Vector3::Min(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::Min(a, b);
    }

    ::Vector3 Vector3::Max(const ::Vector3& a, const ::Vector3& b)
    {
        return VectorMath::Max(a, b);
    }

    // ---- Quaternion ----

    ::Quaternion Quaternion::GetIdentity()
    {
        return ::Quaternion::IDENTITY;
    }

    ::Quaternion Quaternion::AngleAxis(float angle_degrees, const ::Vector3& axis)
    {
        return QuaternionMath::AngleAxis(angle_degrees, axis);
    }

    ::Quaternion Quaternion::LookRotation(const ::Vector3& forward, const ::Vector3& up)
    {
        return QuaternionMath::LookRotation(forward, up);
    }

    // ⚠️ Unity 的 `Quaternion.Slerp` / `Quaternion.Lerp` **夹住** t（要外插有
    // `SlerpUnclamped` / `LerpUnclamped`）。引擎的 SLerp / NLerp 不夹 —— 直接
    // 转发的话 `Slerp(a, b, 2)` 会走到 b 之外，而 Unity 里它等于 `Slerp(a, b, 1)`。
    // 这个差别完全是静默的：朝向"看起来在往那个方向转"，只是转过了头。
    namespace
    {
        float ClampT(float t) { return t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }
    }  // namespace

    ::Quaternion Quaternion::Slerp(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        return QuaternionMath::Slerp(a, b, ClampT(t));
    }

    ::Quaternion Quaternion::Lerp(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        return QuaternionMath::Nlerp(a, b, ClampT(t));
    }

    ::Quaternion Quaternion::Inverse(const ::Quaternion& q)
    {
        return QuaternionMath::Inverse(q);
    }

    float Quaternion::Dot(const ::Quaternion& a, const ::Quaternion& b)
    {
        return QuaternionMath::Dot(a, b);
    }

    float Quaternion::Angle(const ::Quaternion& a, const ::Quaternion& b)
    {
        return QuaternionMath::Angle(a, b);
    }

    ::Quaternion Quaternion::RotateTowards(const ::Quaternion& from, const ::Quaternion& to,
                                           float max_degrees_delta)
    {
        return QuaternionMath::RotateTowards(from, to, max_degrees_delta);
    }

    ::Quaternion Quaternion::FromToRotation(const ::Vector3& a, const ::Vector3& b)
    {
        return QuaternionMath::FromToRotation(a, b);
    }

    ::Vector3 Quaternion::Rotate(const ::Quaternion& q, const ::Vector3& v)
    {
        return QuaternionMath::Rotate(q, v);
    }

    ::Quaternion Quaternion::Euler(float x, float y, float z)
    {
        return ::Quaternion::FromEulerAngles(::Vector3(x, y, z));
    }

    ::Quaternion Quaternion::EulerVector(const ::Vector3& euler_degrees)
    {
        return ::Quaternion::FromEulerAngles(euler_degrees);
    }

    ::Vector3 Quaternion::GetEulerAngles(const ::Quaternion& q)
    {
        return ::Quaternion::ToEulerAngles(q);
    }

    ::Quaternion Quaternion::Normalize(const ::Quaternion& q)
    {
        return QuaternionMath::Normalize(q);
    }

    ::Quaternion Quaternion::Mul(const ::Quaternion& a, const ::Quaternion& b)
    {
        return QuaternionMath::Mul(a, b);
    }

    ::Quaternion Quaternion::Nlerp(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        return QuaternionMath::Nlerp(a, b, t);
    }

    ::Quaternion Quaternion::SlerpUnclamped(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        // ⚠️ shortest_path 关掉：开了之后 `t = 2` 会被当成"走过头再折回来"，
        // 外插就不再是外插了。
        return ::Quaternion::SLerp(t, a, b, /*shortest_path=*/false);
    }

    ::Quaternion Quaternion::LerpUnclamped(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        return ::Quaternion::NLerp(t, a, b, /*shortest_path=*/false);
    }

    // ---- Vector2 ----

    namespace
    {
        /// 夹到 [0,1]。三个 Lerp 都要用，写三遍迟早有一遍写错。
        float Clamp01(float t)
        {
            if (t < 0.0f)
            {
                return 0.0f;
            }
            if (t > 1.0f)
            {
                return 1.0f;
            }
            return t;
        }

        /// 夹到 [-1,1]。acos 的定义域 —— 浮点误差能让 1 变成 1.0000001，然后
        /// acos 返回 NaN，NaN 再一路传染到 transform。
        float ClampCos(float c)
        {
            if (c < -1.0f)
            {
                return -1.0f;
            }
            if (c > 1.0f)
            {
                return 1.0f;
            }
            return c;
        }
    }  // namespace

    ::Vector2 Vector2::GetZero()
    {
        return ::Vector2(0.0f, 0.0f);
    }

    ::Vector2 Vector2::GetOne()
    {
        return ::Vector2(1.0f, 1.0f);
    }

    // 屏幕平面：y 朝上、x 朝右。与世界那套 Z-up / +X=左 无关，见头文件注释。
    ::Vector2 Vector2::GetUp()
    {
        return ::Vector2(0.0f, 1.0f);
    }

    ::Vector2 Vector2::GetDown()
    {
        return ::Vector2(0.0f, -1.0f);
    }

    ::Vector2 Vector2::GetLeft()
    {
        return ::Vector2(-1.0f, 0.0f);
    }

    ::Vector2 Vector2::GetRight()
    {
        return ::Vector2(1.0f, 0.0f);
    }

    ::Vector2 Vector2::GetPositiveInfinity()
    {
        const float inf = std::numeric_limits<float>::infinity();
        return ::Vector2(inf, inf);
    }

    ::Vector2 Vector2::GetNegativeInfinity()
    {
        const float inf = -std::numeric_limits<float>::infinity();
        return ::Vector2(inf, inf);
    }

    float Vector2::GetEpsilon()
    {
        return 0.00001f;
    }

    float Vector2::Angle(const ::Vector2& from, const ::Vector2& to)
    {
        const ::Vector2 a = Normalize(from);
        const ::Vector2 b = Normalize(to);
        return Math::RadiansToDegrees(std::acos(ClampCos(a.x * b.x + a.y * b.y)));
    }

    float Vector2::SignedAngle(const ::Vector2& from, const ::Vector2& to)
    {
        // atan2(cross, dot)：一次拿到符号和大小，比 "acos 求角再乘 sign(cross)" 少
        // 一次开方，也不会在 180° 附近被 acos 的精度放大。
        const float cross = from.x * to.y - from.y * to.x;
        const float dot = from.x * to.x + from.y * to.y;
        return Math::RadiansToDegrees(std::atan2(cross, dot));
    }

    ::Vector2 Vector2::ClampMagnitude(const ::Vector2& v, float max_length)
    {
        const float sqr = v.x * v.x + v.y * v.y;
        if (sqr <= max_length * max_length)
        {
            return v;
        }
        const ::Vector2 n = Normalize(v);
        return ::Vector2(n.x * max_length, n.y * max_length);
    }

    float Vector2::Distance(const ::Vector2& a, const ::Vector2& b)
    {
        return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    }

    float Vector2::Dot(const ::Vector2& a, const ::Vector2& b)
    {
        return a.x * b.x + a.y * b.y;
    }

    ::Vector2 Vector2::Lerp(const ::Vector2& a, const ::Vector2& b, float t)
    {
        return LerpUnclamped(a, b, Clamp01(t));
    }

    ::Vector2 Vector2::LerpUnclamped(const ::Vector2& a, const ::Vector2& b, float t)
    {
        return ::Vector2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
    }

    float Vector2::Magnitude(const ::Vector2& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y);
    }

    float Vector2::SqrMagnitude(const ::Vector2& v)
    {
        return v.x * v.x + v.y * v.y;
    }

    ::Vector2 Vector2::Max(const ::Vector2& a, const ::Vector2& b)
    {
        return ::Vector2(std::max(a.x, b.x), std::max(a.y, b.y));
    }

    ::Vector2 Vector2::Min(const ::Vector2& a, const ::Vector2& b)
    {
        return ::Vector2(std::min(a.x, b.x), std::min(a.y, b.y));
    }

    ::Vector2 Vector2::MoveTowards(const ::Vector2& a, const ::Vector2& b, float max_distance)
    {
        const ::Vector2 delta(b.x - a.x, b.y - a.y);
        const float sqr = delta.x * delta.x + delta.y * delta.y;
        // 已经到了（或 max_distance 够走到）就直接返回 b —— 不这么写会在终点附近
        // 抖：每帧算一个"差一点点"的新点，永远不相等。
        if (sqr == 0.0f || (max_distance >= 0.0f && sqr <= max_distance * max_distance))
        {
            return b;
        }
        const float dist = std::sqrt(sqr);
        return ::Vector2(a.x + delta.x / dist * max_distance, a.y + delta.y / dist * max_distance);
    }

    ::Vector2 Vector2::Normalize(const ::Vector2& v)
    {
        const float sqr = v.x * v.x + v.y * v.y;
        if (sqr <= 0.0f)
        {
            return ::Vector2(0.0f, 0.0f);
        }
        const float inv = 1.0f / std::sqrt(sqr);
        return ::Vector2(v.x * inv, v.y * inv);
    }

    ::Vector2 Vector2::Perpendicular(const ::Vector2& v)
    {
        return ::Vector2(-v.y, v.x);
    }

    ::Vector2 Vector2::Reflect(const ::Vector2& v, const ::Vector2& normal)
    {
        const ::Vector2 n = Normalize(normal);
        const float d = 2.0f * (v.x * n.x + v.y * n.y);
        return ::Vector2(v.x - d * n.x, v.y - d * n.y);
    }

    ::Vector2 Vector2::Scale(const ::Vector2& a, const ::Vector2& b)
    {
        return ::Vector2(a.x * b.x, a.y * b.y);
    }

    ::Vector2 Vector2::ScaleUniform(const ::Vector2& v, float scalar)
    {
        return ::Vector2(v.x * scalar, v.y * scalar);
    }

    // ---- Vector4 ----
    //
    // ⚠️ 长度/归一化一律**四维**（含 w）。值类型自带的那份 Ogre 归一化只动 xyz
    // （平面语义：xyz 是法线、w 是距离），与 Unity 的 `Vector4.magnitude` 对不上，
    // 所以这里自己算，不转发它（接线门 VW8 就是钉这一条：源码里不许出现那个调用）。

    ::Vector4 Vector4::GetZero()
    {
        return ::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    ::Vector4 Vector4::GetOne()
    {
        return ::Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    ::Vector4 Vector4::GetPositiveInfinity()
    {
        const float inf = std::numeric_limits<float>::infinity();
        return ::Vector4(inf, inf, inf, inf);
    }

    ::Vector4 Vector4::GetNegativeInfinity()
    {
        const float inf = -std::numeric_limits<float>::infinity();
        return ::Vector4(inf, inf, inf, inf);
    }

    float Vector4::Distance(const ::Vector4& a, const ::Vector4& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        const float dw = a.w - b.w;
        return std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
    }

    float Vector4::Dot(const ::Vector4& a, const ::Vector4& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    ::Vector4 Vector4::Lerp(const ::Vector4& a, const ::Vector4& b, float t)
    {
        return LerpUnclamped(a, b, Clamp01(t));
    }

    ::Vector4 Vector4::LerpUnclamped(const ::Vector4& a, const ::Vector4& b, float t)
    {
        return ::Vector4(a.x + (b.x - a.x) * t,
                         a.y + (b.y - a.y) * t,
                         a.z + (b.z - a.z) * t,
                         a.w + (b.w - a.w) * t);
    }

    float Vector4::Magnitude(const ::Vector4& v)
    {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
    }

    float Vector4::SqrMagnitude(const ::Vector4& v)
    {
        return v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w;
    }

    ::Vector4 Vector4::Max(const ::Vector4& a, const ::Vector4& b)
    {
        return ::Vector4(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z),
                         std::max(a.w, b.w));
    }

    ::Vector4 Vector4::Min(const ::Vector4& a, const ::Vector4& b)
    {
        return ::Vector4(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z),
                         std::min(a.w, b.w));
    }

    ::Vector4 Vector4::MoveTowards(const ::Vector4& a, const ::Vector4& b, float max_distance)
    {
        const ::Vector4 delta(b.x - a.x, b.y - a.y, b.z - a.z, b.w - a.w);
        const float sqr = SqrMagnitude(delta);
        if (sqr == 0.0f || (max_distance >= 0.0f && sqr <= max_distance * max_distance))
        {
            return b;
        }
        const float dist = std::sqrt(sqr);
        return ::Vector4(a.x + delta.x / dist * max_distance,
                         a.y + delta.y / dist * max_distance,
                         a.z + delta.z / dist * max_distance,
                         a.w + delta.w / dist * max_distance);
    }

    ::Vector4 Vector4::Normalize(const ::Vector4& v)
    {
        const float sqr = SqrMagnitude(v);
        if (sqr <= 0.0f)
        {
            return ::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        const float inv = 1.0f / std::sqrt(sqr);
        return ::Vector4(v.x * inv, v.y * inv, v.z * inv, v.w * inv);
    }

    ::Vector4 Vector4::Project(const ::Vector4& a, const ::Vector4& b)
    {
        const float bb = Dot(b, b);
        if (bb <= 0.0f)
        {
            return ::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        const float k = Dot(a, b) / bb;
        return ::Vector4(b.x * k, b.y * k, b.z * k, b.w * k);
    }

    ::Vector4 Vector4::Scale(const ::Vector4& a, const ::Vector4& b)
    {
        return ::Vector4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
    }

    ::Vector4 Vector4::ScaleUniform(const ::Vector4& v, float scalar)
    {
        return ::Vector4(v.x * scalar, v.y * scalar, v.z * scalar, v.w * scalar);
    }

    // ---- Matrix4x4 ----

    ::Matrix4x4 Matrix4x4::GetIdentity()
    {
        return ::Matrix4x4::IDENTITY;
    }

    ::Matrix4x4 Matrix4x4::GetZero()
    {
        return ::Matrix4x4::ZERO;
    }

    float Matrix4x4::Determinant(const ::Matrix4x4& m)
    {
        return m.determinant();
    }

    ::Matrix4x4 Matrix4x4::Inverse(const ::Matrix4x4& m)
    {
        // 奇异矩阵不抛异常也不产生 NaN —— 一律返回零矩阵。Unity 对奇异矩阵的结果
        // 没有承诺，这里挑了个"不会把 NaN 传染到后续 transform"的确定值；调用方
        // 想区分就先自己看 `Determinant`。
        if (std::fabs(m.determinant()) <= 1.0e-12f)
        {
            return ::Matrix4x4::ZERO;
        }
        return m.inverse();
    }

    ::Matrix4x4 Matrix4x4::Transpose(const ::Matrix4x4& m)
    {
        return m.transpose();
    }

    ::Matrix4x4 Matrix4x4::TRS(const ::Vector3& pos, const ::Quaternion& q, const ::Vector3& s)
    {
        // MakeTransform 的顺序是 T * R * S，与 Unity 的 TRS 一致（先缩放、再旋转、
        // 最后平移）。
        ::Matrix4x4 m;
        m.MakeTransform(pos, s, q);
        return m;
    }

    ::Matrix4x4 Matrix4x4::Scale(const ::Vector3& v)
    {
        return ::Matrix4x4::GetScale(v);
    }

    ::Matrix4x4 Matrix4x4::Translate(const ::Vector3& v)
    {
        return ::Matrix4x4::GetTrans(v);
    }

    ::Matrix4x4 Matrix4x4::Rotate(const ::Quaternion& q)
    {
        ::Matrix4x4 m = ::Matrix4x4::IDENTITY;
        m.setMatrix3x3(Matrix3x3(q));
        return m;
    }

    ::Matrix4x4 Matrix4x4::Ortho(float left, float right, float bottom, float top, float zNear,
                                 float zFar)
    {
        return Math::MakeOrthographicProjectionMatrix(left, right, bottom, top, zNear, zFar);
    }

    ::Matrix4x4 Matrix4x4::Perspective(float fov, float aspect, float zNear, float zFar)
    {
        // fov 是**度**（Unity 同），MakePerspectiveMatrix 要弧度。
        return Math::MakePerspectiveMatrix(Radian(Math::DegreesToRadians(fov)), aspect, zNear, zFar);
    }

    ::Matrix4x4 Matrix4x4::LookAt(const ::Vector3& from, const ::Vector3& to, const ::Vector3& up)
    {
        return Math::MakeLookAtMatrix(from, to, up);
    }

    ::Matrix4x4 Matrix4x4::Multiply(const ::Matrix4x4& a, const ::Matrix4x4& b)
    {
        return a.concatenate(b);
    }

    ::Vector3 Matrix4x4::MultiplyPoint(const ::Matrix4x4& m, const ::Vector3& v)
    {
        return m.transformCoord(v);
    }

    ::Vector3 Matrix4x4::MultiplyPoint3x4(const ::Matrix4x4& m, const ::Vector3& v)
    {
        // 只用左上 3×4：不做透视除法（Unity 的 "fast" 版本同）。
        const ::Vector4 r = m * ::Vector4(v, 1.0f);
        return ::Vector3(r.x, r.y, r.z);
    }

    ::Vector3 Matrix4x4::MultiplyVector(const ::Matrix4x4& m, const ::Vector3& v)
    {
        // 方向：平移列（m03/m13/m23）不参与。
        return ::Vector3(m.m_Mat[0][0] * v.x + m.m_Mat[0][1] * v.y + m.m_Mat[0][2] * v.z,
                         m.m_Mat[1][0] * v.x + m.m_Mat[1][1] * v.y + m.m_Mat[1][2] * v.z,
                         m.m_Mat[2][0] * v.x + m.m_Mat[2][1] * v.y + m.m_Mat[2][2] * v.z);
    }

    ::Vector4 Matrix4x4::GetColumn(const ::Matrix4x4& m, int i)
    {
        if (i < 0 || i > 3)
        {
            return ::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return ::Vector4(m.m_Mat[0][i], m.m_Mat[1][i], m.m_Mat[2][i], m.m_Mat[3][i]);
    }

    ::Vector4 Matrix4x4::GetRow(const ::Matrix4x4& m, int i)
    {
        if (i < 0 || i > 3)
        {
            return ::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return ::Vector4(m.m_Mat[i][0], m.m_Mat[i][1], m.m_Mat[i][2], m.m_Mat[i][3]);
    }

    bool Matrix4x4::IsIdentity(const ::Matrix4x4& m)
    {
        return m == ::Matrix4x4::IDENTITY;
    }
}  // namespace Runtime
