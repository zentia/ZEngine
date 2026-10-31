#include "Runtime/Scripting/ScriptVector/ScriptVector.h"

#include "Runtime/Scripting/ScriptMath/ScriptMath.h"

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

    ::Vector3 Vector3::GetRight()
    {
        return Make(1.0f, 0.0f, 0.0f);
    }

    ::Vector3 Vector3::GetLeft()
    {
        return Make(-1.0f, 0.0f, 0.0f);
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

    ::Vector3 Vector3::Scale(const ::Vector3& v, float scalar)
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

    ::Quaternion Quaternion::Slerp(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        return QuaternionMath::Slerp(a, b, t);
    }

    ::Quaternion Quaternion::Lerp(const ::Quaternion& a, const ::Quaternion& b, float t)
    {
        return QuaternionMath::Nlerp(a, b, t);
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
}  // namespace Runtime
