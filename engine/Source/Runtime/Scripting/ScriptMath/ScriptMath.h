#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"

#include "ScriptMath.generated.h"

// 脚本侧的数学运算。
//
// 为什么是"装静态方法的空类"而不是给 Vector3 加方法
// --------------------------------------------------
// Vector3 / Quaternion 是纯值类型，不派生 Object，而 ZCLASS 有硬约束
// （zht/parser.py：`ZCLASS '%s' must derive from an Object-based class`），
// 所以它们进不了绑定表。脚本侧拿到的 Vector3 只是 Converter 转出来的
// `{x, y, z}` plain object（d.ts 里是 `declare interface Vector3`），**一个方法
// 都没有**。
//
// 后果很实际：脚本能读写 `transform.position`，却没法做任何运算 —— 没有加减、
// 没有 normalized、没有 Distance / Lerp。也就是说"朝目标走一步"这种最基本的
// 玩法代码写不出来，Instantiate / Find / Destroy / SetActive 加在一起也只能做
// "开关对象"。这三个类就是把运算补上。
//
// 代价是拼写：Unity 的 `Vector3.Distance(a, b)` 在这里是 `VectorMath.Distance(a, b)`。
// `Vector3` 这个名字已经被 d.ts 的 interface 占了，同名 class 会撞。
//
// 为什么没有 Euler / Forward / Right
// ----------------------------------
// 这两类 API 的**轴向约定无法从代码可靠确定**，猜错就是一个静默算错的 API：
//   * Euler：引擎没有现成的"欧拉角 → 四元数"实现；而 Quaternion::GetRoll /
//     GetPitch / GetYaw 的注释是从 Ogre（Y-up）照搬的，三段注释互相矛盾
//     （GetPitch 说 "local Z onto X and Y"，GetYaw 说 "local Z onto X and Z"），
//     在 Z-up 引擎里对应哪根轴读不出来。所以只提供 ToEuler（读引擎自己的值），
//     不提供反向构造。
//   * Forward / Right：相机默认 forward 是 `NEGATIVE_UNIT_Y`
//     （CameraComponent.cpp:188/195），和 UE 的 X-forward、Unity 的 Z-forward 都
//     不同，且"对象前方"与"相机前方"未必同一约定。所以只给纯轴向 UnitX/Y/Z，
//     方向由脚本自己用 AngleAxis / LookRotation 造。
//
// 这三个类都不该被实例化 —— 它们只是脚本侧的命名空间。header-only（同
// BasicShape.h 的 Box / Sphere / Capsule，无对应 .cpp）。

// ---- 向量 ----------------------------------------------------------------

ZCLASS()
class VectorMath : public Object
{
    ZGENERATED_BODY(VectorMath)

public:
    // ---- 构造与轴向常量 ----
    // Z-up：UnitZ 是竖直轴。不给 Forward / Right，理由见文件头。
    ZFUNCTION()
    static Vector3 Create(float x, float y, float z) { return Vector3(x, y, z); }

    ZFUNCTION()
    static Vector3 Zero() { return Vector3::ZERO; }

    ZFUNCTION()
    static Vector3 One() { return Vector3(1.f, 1.f, 1.f); }

    ZFUNCTION()
    static Vector3 UnitX() { return Vector3::UNIT_X; }

    ZFUNCTION()
    static Vector3 UnitY() { return Vector3::UNIT_Y; }

    ZFUNCTION()
    static Vector3 UnitZ() { return Vector3::UNIT_Z; }

    // ---- 算术 ----
    ZFUNCTION()
    static Vector3 Add(const Vector3& a, const Vector3& b) { return a + b; }

    ZFUNCTION()
    static Vector3 Sub(const Vector3& a, const Vector3& b) { return a - b; }

    /// 分量逐个相乘（Unity 的 Vector3.Scale）。不是点积也不是叉积 —— 三个名字放
    /// 一起最容易搞混，所以这里叫 Mul 但注释写死是分量乘。
    ZFUNCTION()
    static Vector3 Mul(const Vector3& a, const Vector3& b) { return Vector3(a.x * b.x, a.y * b.y, a.z * b.z); }

    ZFUNCTION()
    static Vector3 Scale(const Vector3& v, float scalar) { return v * scalar; }

    ZFUNCTION()
    static Vector3 Negate(const Vector3& v) { return -v; }

    // ---- 度量 ----
    ZFUNCTION()
    static float Magnitude(const Vector3& v) { return v.length(); }

    /// 平方长度。比较长度时用它省掉一次开方。
    ZFUNCTION()
    static float SqrMagnitude(const Vector3& v) { return v.squaredLength(); }

    ZFUNCTION()
    static float Distance(const Vector3& a, const Vector3& b) { return a.distance(b); }

    ZFUNCTION()
    static float SqrDistance(const Vector3& a, const Vector3& b) { return a.squaredDistance(b); }

    ZFUNCTION()
    static float Dot(const Vector3& a, const Vector3& b) { return a.dotProduct(b); }

    ZFUNCTION()
    static Vector3 Cross(const Vector3& a, const Vector3& b) { return a.crossProduct(b); }

    /// 零向量返回零向量，不会返回 NaN：Vector3::normalise() 内部对 length == 0
    /// 直接 return（Vector3.h:229），不是 0/0。
    ZFUNCTION()
    static Vector3 Normalize(const Vector3& v) { return v.normalisedCopy(); }

    /// 夹角，**度**。零向量参与时 Vector3::angleBetween 返回 0。
    ZFUNCTION()
    static float Angle(const Vector3& a, const Vector3& b) { return a.angleBetween(b).valueDegrees(); }

    /// 夹角，弧度。引擎内部大量用弧度，给一个不用再换算的入口。
    ZFUNCTION()
    static float AngleRad(const Vector3& a, const Vector3& b) { return a.angleBetween(b).valueRadians(); }

    // ---- 插值与限幅 ----
    /// alpha 不夹取（Unity 的 LerpUnclamped 同）：想夹取自己先 Clamp01。
    ZFUNCTION()
    static Vector3 Lerp(const Vector3& a, const Vector3& b, float alpha) { return Vector3::Lerp(a, b, alpha); }

    /// 朝 b 移动，步长不超过 max_distance（Unity 的 MoveTowards 同）。
    ///
    /// max_distance 为负时朝**反**方向走 —— 与 Unity 一致，不是笔误。
    /// 已经足够近（或 a == b）时直接返回 b，不会过冲。
    ZFUNCTION()
    static Vector3 MoveTowards(const Vector3& a, const Vector3& b, float max_distance)
    {
        const Vector3 delta = b - a;
        const float   dist  = delta.length();
        if (dist <= max_distance || dist < 1e-6f)
        {
            return b;
        }
        return a + delta * (max_distance / dist);
    }

    /// 把长度压到 max_length 以内，方向不变。零向量返回零向量。
    ZFUNCTION()
    static Vector3 ClampMagnitude(const Vector3& v, float max_length)
    {
        const float sqr = v.squaredLength();
        if (sqr > max_length * max_length)
        {
            return v.normalisedCopy() * max_length;
        }
        return v;
    }

    ZFUNCTION()
    static Vector3 Clamp(const Vector3& v, const Vector3& min, const Vector3& max)
    {
        return Vector3::Clamp(v, min, max);
    }

    ZFUNCTION()
    static Vector3 Min(const Vector3& a, const Vector3& b)
    {
        return Vector3(Math::min(a.x, b.x), Math::min(a.y, b.y), Math::min(a.z, b.z));
    }

    ZFUNCTION()
    static Vector3 Max(const Vector3& a, const Vector3& b)
    {
        return Vector3(Math::max(a.x, b.x), Math::max(a.y, b.y), Math::max(a.z, b.z));
    }

    // ---- 其它 ----
    ZFUNCTION()
    static bool IsZero(const Vector3& v) { return v.isZero(); }

    /// 以 normal 为法线的平面反射。要求 v **背离**平面（Vector3::reflect 的前提），
    /// 指向平面内的话先自己取反。
    ZFUNCTION()
    static Vector3 Reflect(const Vector3& v, const Vector3& normal) { return v.reflect(normal); }

    /// 投影到以 normal 为法线的平面上。
    ZFUNCTION()
    static Vector3 Project(const Vector3& v, const Vector3& on_normal) { return v.project(on_normal); }

    /// 从 a 转到 b 的最短旋转（Unity 的 Quaternion.FromToRotation）。
    ZFUNCTION()
    static Quaternion FromToRotation(const Vector3& a, const Vector3& b) { return a.getRotationTo(b); }
};

// ---- 旋转 ----------------------------------------------------------------

ZCLASS()
class QuaternionMath : public Object
{
    ZGENERATED_BODY(QuaternionMath)

public:
    ZFUNCTION()
    static Quaternion Identity() { return Quaternion::IDENTITY; }

    /// 绕 axis 转 angle_degrees 度。axis 不必是单位向量（内部会归一化）。
    /// 不给 Euler 构造，理由见文件头。
    ZFUNCTION()
    static Quaternion AngleAxis(float angle_degrees, const Vector3& axis)
    {
        // Radian 和 Degree 的互相转换构造都是 explicit，所以不能直接把 Degree
        // 传给吃 `const Radian&` 的形参 —— 手动换算。
        return Quaternion::GetQuaternionFromAngleAxis(Radian(angle_degrees * Math_fDeg2Rad), axis);
    }

    /// 朝 forward 看、up 为上方（Unity 的 Quaternion.LookRotation）。
    /// forward 为零向量时引擎内部退化，调用方应自己保证非零。
    ZFUNCTION()
    static Quaternion LookRotation(const Vector3& forward, const Vector3& up)
    {
        return Quaternion::GetQuaternionFromDirection(forward, up);
    }

    /// 旋转复合：先转 b，再转 a（与 Unity 的 `a * b` 同序，也和引擎
    /// `operator*` 一致 —— CameraComponent 里就是 `q_yaw * q_pitch * forward`）。
    ZFUNCTION()
    static Quaternion Mul(const Quaternion& a, const Quaternion& b) { return a * b; }

    /// 用 q 旋转向量 v。
    ZFUNCTION()
    static Vector3 Rotate(const Quaternion& q, const Vector3& v) { return q * v; }

    ZFUNCTION()
    static Quaternion Inverse(const Quaternion& q) { return q.inverse(); }

    /// 球面插值。shortest path 恒开：两个朝向之间走短弧，不会出现"绕远路转一圈"。
    /// 参数是 (a, b, t) 而不是引擎 SLerp 的 (t, kp, kq) —— 对齐 Unity 的拼写。
    ZFUNCTION()
    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t)
    {
        return Quaternion::SLerp(t, a, b, /*shortest_path=*/true);
    }

    /// 归一化线性插值：比 Slerp 快，但角速度不均匀。
    ZFUNCTION()
    static Quaternion Nlerp(const Quaternion& a, const Quaternion& b, float t)
    {
        return Quaternion::NLerp(t, a, b, /*shortest_path=*/true);
    }

    /// 两个旋转之间的夹角，**度**。
    ZFUNCTION()
    static float Angle(const Quaternion& a, const Quaternion& b)
    {
        Radian   angle;
        Vector3  axis;
        (a.inverse() * b).ToAngleAxis(angle, axis);
        return Math::abs(angle.valueDegrees());
    }

    /// 拆成三个欧拉角（度），读的是引擎自己的 GetPitch / GetYaw / GetRoll。
    ///
    /// **分量 -> 轴的映射已由 ScriptVMSmokeTest 的 C1 钉住**（绕 X 转 90 落在
    /// x 分量、绕 Y 转 90 落在 z 分量、绕 Z 转 90 落在 y 分量）：
    ///
    ///     x = pitch = 绕 X
    ///     y = yaw   = 绕 Z   ← Z-up 该有的样子，竖直轴是 Z
    ///     z = roll  = 绕 Y
    ///
    /// 仍然**没有反向的 Euler(x, y, z)**：反向要先定死旋转顺序（YXZ 还是 ZYX），
    /// 定错一样是静默算错，等真有需求再定。
    ZFUNCTION()
    static Vector3 ToEuler(const Quaternion& q)
    {
        return Vector3(q.GetPitch().valueDegrees(), q.GetYaw().valueDegrees(), q.GetRoll().valueDegrees());
    }
};

// ---- 标量 ----------------------------------------------------------------

ZCLASS()
class Mathf : public Object
{
    ZGENERATED_BODY(Mathf)

public:
    ZFUNCTION()
    static float Clamp(float value, float min, float max) { return Math::Clamp(value, min, max); }

    ZFUNCTION()
    static float Clamp01(float value) { return Math::Clamp(value, 0.f, 1.f); }

    /// alpha 不夹取（Unity 的 LerpUnclamped 同）。
    ZFUNCTION()
    static float Lerp(float a, float b, float alpha) { return a + alpha * (b - a); }

    /// 朝 b 走，步长不超过 max_delta。max_delta 为负则朝反方向（同 VectorMath）。
    ZFUNCTION()
    static float MoveTowards(float a, float b, float max_delta)
    {
        const float delta = b - a;
        if (Math::abs(delta) <= max_delta)
        {
            return b;
        }
        return a + ((delta > 0.f) ? max_delta : -max_delta);
    }

    /// -1 / 0 / 1。Math 里没有 Sign（只有 abs），所以自己写。
    ZFUNCTION()
    static float Sign(float value) { return (value > 0.f) ? 1.f : ((value < 0.f) ? -1.f : 0.f); }

    ZFUNCTION()
    static float Deg2Rad(float degrees) { return degrees * Math_fDeg2Rad; }

    ZFUNCTION()
    static float Rad2Deg(float radians) { return radians * Math_fRad2Deg; }

    ZFUNCTION()
    static float Pi() { return Math_PI; }
};
