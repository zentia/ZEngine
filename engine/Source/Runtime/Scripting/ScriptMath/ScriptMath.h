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

    ZFUNCTION()
    static float HalfPi() { return Math_HALF_PI; }

    ZFUNCTION()
    static float TwoPi() { return Math_TWO_PI; }

    // =========================================================================
    // 下面这一整块是"Unity 的 Mathf 里本引擎缺的那些"
    // =========================================================================
    //
    // 补全前 Mathf 只有 8 个函数（Clamp / Clamp01 / Lerp / MoveTowards / Sign /
    // Deg2Rad / Rad2Deg / Pi），而标量数学是脚本里最容易被顺手用到的东西 ——
    // 缺一个 `SmoothStep` 或 `Repeat`，玩法代码就只能用裸的 `Math.sin` 之类现
    // 拼，而拼出来的那一份和引擎里的其它部分不是同一个口径。

    // ---- 常量 ----------------------------------------------------------
    //
    // ⚠️ Unity 里 `Epsilon` / `Infinity` / `NegativeInfinity` 是**静态只读字段**，
    // 这里只能是**方法**。ZSCRIPTPROPERTY 的 getter 不允许 static（zht/parser.py
    // 直接报错），所以静态属性在 ZFUNCTION 体系里做不出来 —— 脚本得写
    // `Mathf.Infinity()`。这个偏差是有意的：宁可多一对括号，也不给一个"看起来
    // 是字段其实是函数"的东西。

    /// 1 附近的浮点分辨极限（`FLT_EPSILON`，≈1.19e-7）。
    ///
    /// ⚠️ 与 Unity 的 `Mathf.Epsilon` **不是同一个值**：Unity 给的是
    /// `float.Epsilon` ≈ 1.4e-45（最小非正规数）。这里给 FLT_EPSILON，因为
    /// `Approximately` 拿它当下限 —— 用 1.4e-45 的话那一项恒等于 0，
    /// Approximately 会退化成纯相对比较，在 0 附近直接失效。
    ZFUNCTION()
    static float Epsilon() { return std::numeric_limits<float>::epsilon(); }

    ZFUNCTION()
    static float Infinity() { return std::numeric_limits<float>::infinity(); }

    ZFUNCTION()
    static float NegativeInfinity() { return -std::numeric_limits<float>::infinity(); }

    // ---- 取整数 --------------------------------------------------------
    //
    // ⚠️ `Round` 的平局规则：**四舍六入五取偶**（Unity 的 Mathf.Round 同）。
    // 不是"四舍五入" —— `Round(2.5)` 是 **2** 不是 3。用 std::rint 而不是
    // std::round 就是为了这个：后者是"远离零取整"。
    ZFUNCTION()
    static float Abs(float value) { return Math::abs(value); }

    ZFUNCTION()
    static float Ceil(float value) { return std::ceil(value); }

    ZFUNCTION()
    static float Floor(float value) { return std::floor(value); }

    ZFUNCTION()
    static float Round(float value) { return std::rint(value); }

    ZFUNCTION()
    static int32_t CeilToInt(float value) { return static_cast<int32_t>(std::ceil(value)); }

    ZFUNCTION()
    static int32_t FloorToInt(float value) { return static_cast<int32_t>(std::floor(value)); }

    /// 平局取偶，与 `Round` 同一规则（`(int)Mathf.Round(f)`，不是 lround）。
    ZFUNCTION()
    static int32_t RoundToInt(float value) { return static_cast<int32_t>(std::rint(value)); }

    // ---- 三角 / 幂 -----------------------------------------------------
    //
    // 全部吃**弧度**、返回**弧度**，与 C 的 <cmath> 一致，也与引擎 Math 一致。
    // 要角度就用 Deg2Rad / Rad2Deg 换，别猜。
    ZFUNCTION()
    static float Sin(float radians) { return std::sin(radians); }

    ZFUNCTION()
    static float Cos(float radians) { return std::cos(radians); }

    ZFUNCTION()
    static float Tan(float radians) { return std::tan(radians); }

    ZFUNCTION()
    static float Asin(float ratio) { return Math::asin(ratio).valueRadians(); }

    ZFUNCTION()
    static float Acos(float ratio) { return Math::acos(ratio).valueRadians(); }

    ZFUNCTION()
    static float Atan(float ratio) { return std::atan(ratio); }

    /// `Atan2(y, x)` —— **y 在前**，与 C 的 `atan2` 和 Unity 同序。
    /// 参数顺序写反是最常见的数学 bug 之一，而且它在两个象限里"看起来是对的"。
    ZFUNCTION()
    static float Atan2(float y, float x) { return std::atan2(y, x); }

    ZFUNCTION()
    static float Sqrt(float value) { return std::sqrt(value); }

    ZFUNCTION()
    static float Pow(float base, float exponent) { return std::pow(base, exponent); }

    ZFUNCTION()
    static float Exp(float power) { return std::exp(power); }

    /// 自然对数（Unity 的 `Mathf.Log(f)`）。
    ///
    /// **没有 `Log(f, p)` 这个双参重载**：ZFUNCTION 体系里同名重载会被当成
    /// 两个不同名字的绑定，脚本侧只能看到一个。要换底自己除：`Log(v)/Log(10)`。
    ZFUNCTION()
    static float Log(float value) { return std::log(value); }

    ZFUNCTION()
    static float Log10(float value) { return std::log10(value); }

    // ---- 极值 ----
    // 手写三元而不是 `Math::max`：这两个名字在 MSVC 的工程里可能已经被
    // <windows.h> 的 min/max 宏占掉，而宏不看命名空间。
    ZFUNCTION()
    static float Max(float a, float b) { return (a > b) ? a : b; }

    ZFUNCTION()
    static float Min(float a, float b) { return (a < b) ? a : b; }

    // ---- 环绕 / 摆动 ---------------------------------------------------

    /// `t` 对 `length` 取模，但结果恒在 `[0, length)`（**负数也归正**）。
    /// `fmod` 不行：`fmod(-1, 3)` 是 -1，而 Repeat 要给 2。角度插值少了这个
    /// 差别，跨 0° 时会往反方向转一大圈。
    ZFUNCTION()
    static float Repeat(float t, float length)
    {
        return Clamp(t - std::floor(t / length) * length, 0.f, length);
    }

    /// 在 `[0, length]` 之间来回。t 是"时间"，length 是单程长度。
    ZFUNCTION()
    static float PingPong(float t, float length)
    {
        t = Repeat(t, length * 2.f);
        return length - Math::abs(t - length);
    }

    /// 两个**角度**（度）之间的最短差，落在 `(-180, 180]`。
    /// 直接 `target - current` 在 350° → 10° 时给 -340，而正确答案是 +20。
    ZFUNCTION()
    static float DeltaAngle(float current, float target)
    {
        float delta = Repeat(target - current, 360.f);
        if (delta > 180.f)
        {
            delta -= 360.f;
        }
        return delta;
    }

    /// 角度插值（度），走**最短弧**。与 `Lerp` 的区别只在"先算最短差"。
    /// t 夹取在 [0,1]，与 Unity 同。
    ZFUNCTION()
    static float LerpAngle(float a, float b, float t)
    {
        const float delta = DeltaAngle(a, b);
        return a + delta * Clamp01(t);
    }

    /// 朝目标角走，单步不超过 max_delta（度），走最短弧。
    ZFUNCTION()
    static float MoveTowardsAngle(float current, float target, float max_delta)
    {
        const float delta_angle = DeltaAngle(current, target);
        if (-max_delta < delta_angle && delta_angle < max_delta)
        {
            return target;
        }
        return MoveTowards(current, current + delta_angle, max_delta);
    }

    // ---- 插值 ----------------------------------------------------------

    /// 与 `Lerp` 完全相同 —— 本引擎的 `Lerp` 本来就不夹取 alpha。
    /// 提供它是为了**把这件事说清楚**：脚本里写 `Lerp(a, b, 1.5)` 得到外插值，
    /// 而不是因为 Unity 有两个名字就以为这里有两种语义。
    ZFUNCTION()
    static float LerpUnclamped(float a, float b, float alpha) { return Lerp(a, b, alpha); }

    /// `Lerp` 的反函数：`value` 在 [a, b] 里的归一化位置。
    ///
    /// ⚠️ a == b 时返回 0 —— 不是 NaN。除零会产出一个 NaN，而 NaN 会一路静默
    /// 传下去（NaN 参与的任何比较都是 false），最后表现为"这个值莫名不动了"。
    ZFUNCTION()
    static float InverseLerp(float a, float b, float value)
    {
        if (a == b)
        {
            return 0.f;
        }
        return Clamp01((value - a) / (b - a));
    }

    /// 平滑阶跃：`t` 夹取到 [0,1]，再过一条 3t²-2t³ 的 S 曲线。
    /// 与 `Lerp` 的区别是**两端导数为 0**（进出都平滑），直接 Lerp 在端点会
    /// 有一个速度突变。
    ZFUNCTION()
    static float SmoothStep(float from, float to, float t)
    {
        t = Clamp01(t);
        t = t * t * (3.f - 2.f * t);
        return to * t + from * (1.f - t);
    }

    // ---- 杂项 ----------------------------------------------------------

    /// 浮点"约等于"。相对 + 绝对双判据（Unity 同）：
    /// 纯相对比较在 0 附近失效（任何两个极小数都"相等"），纯绝对比较在大数上
    /// 失效（1e9 和 1e9+1 差得远却"相等"）。
    ZFUNCTION()
    static bool Approximately(float a, float b)
    {
        return Math::abs(b - a) < Max(1e-6f * Max(Math::abs(a), Math::abs(b)), Epsilon() * 8.f);
    }

    ZFUNCTION()
    static bool IsPowerOfTwo(int32_t value) { return (value > 0) && ((value & (value - 1)) == 0); }

    /// >= value 的最小的 2 的幂。位扩散法（`value--` 然后右移按位或）。
    ZFUNCTION()
    static int32_t NextPowerOfTwo(int32_t value)
    {
        value -= 1;
        value |= value >> 16;
        value |= value >> 8;
        value |= value >> 4;
        value |= value >> 2;
        value |= value >> 1;
        return value + 1;
    }

    /// 最近的 2 的幂（距离相等时取大的那个，与 Unity 同）。
    ZFUNCTION()
    static int32_t ClosestPowerOfTwo(int32_t value)
    {
        const int32_t next = NextPowerOfTwo(value);
        const int32_t prev = next >> 1;
        return ((next - value) > (value - prev)) ? prev : next;
    }

    /// 2D Perlin 噪声，返回 `[0, 1]`。
    ///
    /// ⚠️ **不是 Unity 的逐位实现**：Unity 的 `Mathf.PerlinNoise` 用它自己的
    /// 排列表和渐变取法，这里是标准的 2D 梯度噪声。值域、确定性、连续性一致，
    /// 但同一个 (x, y) 的**具体数值不同** —— 从 Unity 工程里搬一张"看起来一样"
    /// 的噪声图是搬不过来的，只能搬"用噪声做这件事"的用法。
    ZFUNCTION()
    static float PerlinNoise(float x, float y)
    {
        // 整数哈希而不是置换表：常量表放在 header-only 的静态方法里每个 TU 一份。
        auto hash = [](int32_t ix, int32_t iy) -> int32_t {
            uint32_t h = static_cast<uint32_t>(ix * 374761393 + iy * 668265263);
            h = (h ^ (h >> 13)) * 1274126177u;
            return static_cast<int32_t>((h ^ (h >> 16)) % 8u);
        };
        // 8 个方向上的单位梯度（4 个轴向 + 4 个对角）。
        static const float k_gradients[8][2] = {
            {1.f, 0.f},  {-1.f, 0.f},  {0.f, 1.f},          {0.f, -1.f},
            {0.70710678f, 0.70710678f}, {-0.70710678f, 0.70710678f},
            {0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f},
        };
        auto fade = [](float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); };
        auto grad = [&](int32_t ix, int32_t iy, float dx, float dy) -> float {
            const int32_t g = hash(ix, iy);
            return k_gradients[g][0] * dx + k_gradients[g][1] * dy;
        };

        const int32_t x0 = static_cast<int32_t>(std::floor(x));
        const int32_t y0 = static_cast<int32_t>(std::floor(y));
        const float   fx = x - static_cast<float>(x0);
        const float   fy = y - static_cast<float>(y0);
        const float   u = fade(fx);
        const float   v = fade(fy);

        const float n00 = grad(x0, y0, fx, fy);
        const float n10 = grad(x0 + 1, y0, fx - 1.f, fy);
        const float n01 = grad(x0, y0 + 1, fx, fy - 1.f);
        const float n11 = grad(x0 + 1, y0 + 1, fx - 1.f, fy - 1.f);
        const float nx0 = n00 + u * (n10 - n00);
        const float nx1 = n01 + u * (n11 - n01);
        const float value = nx0 + v * (nx1 - nx0);

        // 2D 梯度噪声的理论幅值约 ±0.7071，乘 1/0.7071 归一到 ±1 再映射到 [0,1]。
        return 0.5f + 0.5f * Clamp(value * 1.41421356f, -1.f, 1.f);
    }

    // ---- 色彩空间 ------------------------------------------------------
    //
    // sRGB 的分段近似（低段是直线、高段是幂），不是纯 pow(x, 2.2)：
    // 纯幂在 0 附近导数发散，暗部会被压成一团黑。

    /// gamma 空间 -> 线性空间。
    ZFUNCTION()
    static float GammaToLinearSpace(float value)
    {
        if (value <= 0.04045f)
        {
            return value / 12.92f;
        }
        if (value < 1.f)
        {
            return std::pow((value + 0.055f) / 1.055f, 2.4f);
        }
        return std::pow(value, 2.2f);
    }

    /// 线性空间 -> gamma 空间。与上面互为逆运算（0..1 内往返）。
    ZFUNCTION()
    static float LinearToGammaSpace(float value)
    {
        if (value <= 0.f)
        {
            return 0.f;
        }
        if (value <= 0.0031308f)
        {
            return value * 12.92f;
        }
        if (value < 1.f)
        {
            return 1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
        }
        return std::pow(value, 1.f / 2.2f);
    }

    // =========================================================================
    // 故意没有的
    // =========================================================================
    //
    // * `SmoothDamp` / `SmoothDampAngle` —— Unity 的签名是
    //   `SmoothDamp(current, target, ref currentVelocity, smoothTime)`，靠
    //   `ref` 把速度带回给调用方。TS 没有 out/ref 参数（见 RaycastHit 为什么
    //   自带 `valid` 字段），这个 API 搬不过来。要做阻尼就用
    //   `MoveTowards` 或自己在脚本里存速度。
    // * `Log(f, p)` / `CorrelatedColorTemperatureToRGB` —— 前者是重载（见上），
    //   后者返回 Color 而本引擎没有 Color 值类型。
};
