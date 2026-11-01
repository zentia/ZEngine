#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include "ScriptVector.generated.h"

// =============================================================================
// `Vector3` / `Quaternion` / `Vector2` / `Vector4` / `Matrix4x4` —— Unity 值类型的
// **静态面**。
//
// 为什么又是一对同名门面
// ----------------------
// 引擎的 `Vector3` / `Quaternion` 是纯值类型（POD，不派生 Object），进不了绑定表
// （ZCLASS 必须派生 Object）。它们作为值类型跨边界 —— `ZScript::Converter<T>`
// 把它们搬成 `{x,y,z}` / `{x,y,z,w}`，.d.ts 里是 `declare interface Vector3`。
//
// 于是 Unity 那些**静态成员**（`Vector3.zero`、`Vector3.up`、`Vector3.Distance`、
// `Quaternion.identity`、`Matrix4x4.TRS`）没有地方挂：`VectorMath` /
// `QuaternionMath` 是另一个名字，从 Unity 搬过来的代码每个调用点都得改。这几个门面
// 就是为了补上 Unity 的拼写。`Vector2` / `Vector4` / `Matrix4x4` 与 `Vector3` /
// `Quaternion` 同一套做法，只是晚做了一步。
//
// 做法与 `Runtime::Color`（`ScriptColor.h`）完全一样：门面放进 `namespace Runtime`，
// 值类型一律写全 `::Vector3` / `::Quaternion`（在命名空间内部，裸 `Vector3` 指的是
// 这个门面类，漏了 `::` 编译器会立刻报错，不会静默）。d.ts 侧
// `declare class Vector3` 与 `declare interface Vector3` **同名合并** —— 合起来
// 正是 Unity 的形状：既是值类型，又是常量与工具函数的命名空间。
//
// ⚠️ 轴向：Z-up、且 +X 是"左"，不是 Unity 的 Y-up / +X=右
// ------------------------------------------------------
// Unity：Y 轴朝上，`Vector3.up` = (0,1,0)、`Vector3.forward` = (0,0,1)、
// `Vector3.right` = (1,0,0)。
//
// ZEngine：**Z 轴朝上**，相机默认 forward 是 `NEGATIVE_UNIT_Y`
// （`CameraComponent.cpp`），而 right = forward × up = (-Y) × (+Z) = **-X** ——
// 也就是"local +X 是左"。所以：
//
//     up      = (0, 0, 1)        down  = (0, 0, -1)
//     forward = (0, -1, 0)       back  = (0, 1, 0)
//     right   = (-1, 0, 0)       left  = (1, 0, 0)
//
// 这里的名字对的是**语义轴**（"上"、"前"、"右"），不是坐标分量。从 Unity 搬
// 过来的 `transform.position = Vector3.up * 2` 因此仍然把物体举到引擎的天花板
// 方向 —— 这正是要的效果；照搬 Unity 的数字才会把物体平推到墙里。
//
// ⚠️ 与 `Transform::GetUp/Forward/Right` **必须一对一对上**：脚本里
// `Vector3.right` 和 `transform.right` 指的是同一边。UnityParitySmokeTest 的
// S4/S5 钉 transform 那半个约定，本文件的 `TestAxisConventionAgreement`
// （UnityParitySmokeTest）把两边放在一起比。`UnitX/Y/Z` 那种纯轴向的写法留给
// `VectorMath`。
//
// 与 `VectorMath` / `QuaternionMath` 的关系
// -----------------------------------------
// 实现全部转发过去 —— 两份实现迟早会漂移（上次漂移就是 `Color.Lerp` 夹不夹 t
// 的问题）。唯一**故意不同**的是 `Vector3.Lerp`：Unity 的 `Vector3.Lerp` **夹 t**，
// 不夹的版本叫 `LerpUnclamped`；而 `VectorMath.Lerp` 不夹（历史原因）。门面按
// Unity 的语义给，两者都留着。
// =============================================================================

namespace Runtime
{
    /// Unity 的 `Vector3` 静态面：常量 + 向量运算。不该被实例化（同 `Color` /
    /// `Time` / `Mathf`），之所以是个 Object 派生类只是因为 ZCLASS 要求。
    ZCLASS()
    class Vector3 : public Object
    {
        ZGENERATED_BODY(Vector3)

    public:
        // ---- 常量 ----

        /// (0,0,0)。Unity 的 `Vector3.zero`。
        ZFUNCTION()
        static ::Vector3 GetZero();
        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")

        /// (1,1,1)。Unity 的 `Vector3.one`。
        ZFUNCTION()
        static ::Vector3 GetOne();
        ZSCRIPTPROPERTY(Name = "one", Getter = "GetOne")

        // ---- 轴向常量（Z-up，见文件头）----

        /// 上 = +Z。Unity 的 `Vector3.up`（在 Unity 里是 +Y —— 轴向不同，语义同）。
        ZFUNCTION()
        static ::Vector3 GetUp();
        ZSCRIPTPROPERTY(Name = "up", Getter = "GetUp")

        /// 下 = -Z。
        ZFUNCTION()
        static ::Vector3 GetDown();
        ZSCRIPTPROPERTY(Name = "down", Getter = "GetDown")

        /// 前 = -Y（相机默认朝向）。
        ZFUNCTION()
        static ::Vector3 GetForward();
        ZSCRIPTPROPERTY(Name = "forward", Getter = "GetForward")

        /// 后 = +Y。
        ZFUNCTION()
        static ::Vector3 GetBack();
        ZSCRIPTPROPERTY(Name = "back", Getter = "GetBack")

        /// 右 = **-X**。⚠️ 本引擎 local +X 是"左"（right = forward × up =
        /// (-Y) × (+Z) = -X），与 `Transform::GetRight()` 同一个约定 —— 两边必须
        /// 指同一边，否则脚本里 `Vector3.right` 和 `transform.right` 会互相打架，
        /// 而且谁都不报错。Unity 的 `Vector3.right` 是 +X，因为它的 +X 就是右。
        ZFUNCTION()
        static ::Vector3 GetRight();
        ZSCRIPTPROPERTY(Name = "right", Getter = "GetRight")

        /// 左 = +X（同理，与 `Transform::GetLeft()` 一致）。
        ZFUNCTION()
        static ::Vector3 GetLeft();
        ZSCRIPTPROPERTY(Name = "left", Getter = "GetLeft")

        /// 三个分量都是 +∞。Unity 的 `Vector3.positiveInfinity`。
        ZFUNCTION()
        static ::Vector3 GetPositiveInfinity();
        ZSCRIPTPROPERTY(Name = "positiveInfinity", Getter = "GetPositiveInfinity")

        /// 三个分量都是 -∞。Unity 的 `Vector3.negativeInfinity`。
        ZFUNCTION()
        static ::Vector3 GetNegativeInfinity();
        ZSCRIPTPROPERTY(Name = "negativeInfinity", Getter = "GetNegativeInfinity")

        // ---- 运算（全部转发 VectorMath）----

        /// 两点距离。Unity 的 `Vector3.Distance`。
        ZFUNCTION()
        static float Distance(const ::Vector3& a, const ::Vector3& b);

        /// 距离的平方。比距离少一次开方 —— 只用来比较远近时用它。
        ZFUNCTION()
        static float SqrDistance(const ::Vector3& a, const ::Vector3& b);

        /// 点积。Unity 的 `Vector3.Dot`。
        ZFUNCTION()
        static float Dot(const ::Vector3& a, const ::Vector3& b);

        /// 叉积。**右手系**，顺序反了结果反向。Unity 的 `Vector3.Cross`。
        ZFUNCTION()
        static ::Vector3 Cross(const ::Vector3& a, const ::Vector3& b);

        /// 线性插值，`t` **被夹到 [0,1]**（Unity 的 `Vector3.Lerp` 同样夹）。
        /// 想外推用 `LerpUnclamped`。
        ZFUNCTION()
        static ::Vector3 Lerp(const ::Vector3& a, const ::Vector3& b, float t);

        /// 线性插值，`t` 不夹。
        ZFUNCTION()
        static ::Vector3 LerpUnclamped(const ::Vector3& a, const ::Vector3& b, float t);

        /// 长度。Unity 的 `Vector3.Magnitude`。
        ZFUNCTION()
        static float Magnitude(const ::Vector3& v);

        /// 长度的平方。Unity 的 `Vector3.sqrMagnitude` 是实例属性，TS 没有实例
        /// 方法可挂（脚本侧的 Vector3 是 plain object），所以这里只能是函数。
        ZFUNCTION()
        static float SqrMagnitude(const ::Vector3& v);

        /// 归一化。零向量返回零向量（不是 NaN —— 见 `VectorMath.Normalize`）。
        ZFUNCTION()
        static ::Vector3 Normalize(const ::Vector3& v);

        /// 逐分量相乘。**不是**点积也不是叉积。Unity 的 `Vector3.Scale`。
        ///
        /// ⚠️ 历史这里曾是 `Scale(v, scalar)`（整体缩放），与 Unity 对不上；标量版本
        /// 挪到 `ScaleUniform` 了。改之前全工程没有调用点（`VectorMath::Scale` 那个
        /// 标量版本不受影响，测试 V2d 仍然钉着它）。
        ZFUNCTION()
        static ::Vector3 Scale(const ::Vector3& a, const ::Vector3& b);

        /// 整体乘一个标量。Unity 里写成 `v * s` —— TS 没有运算符重载，所以这里是
        /// 个函数（同 `Vector2.ScaleUniform`）。**不是** Unity 的 API。
        ZFUNCTION()
        static ::Vector3 ScaleUniform(const ::Vector3& v, float scalar);

        /// 朝 b 走，步长不超过 max_distance。Unity 的 `Vector3.MoveTowards`。
        ZFUNCTION()
        static ::Vector3 MoveTowards(const ::Vector3& a, const ::Vector3& b, float max_distance);

        /// 把长度夹到 max_length 以内。Unity 的 `Vector3.ClampMagnitude`。
        ZFUNCTION()
        static ::Vector3 ClampMagnitude(const ::Vector3& v, float max_length);

        /// 关于法线反射。Unity 的 `Vector3.Reflect`。
        ZFUNCTION()
        static ::Vector3 Reflect(const ::Vector3& v, const ::Vector3& normal);

        /// 投影到 on_normal 上。Unity 的 `Vector3.Project`。
        ZFUNCTION()
        static ::Vector3 Project(const ::Vector3& v, const ::Vector3& on_normal);

        /// 夹角，**度数**。Unity 的 `Vector3.Angle`。
        ZFUNCTION()
        static float Angle(const ::Vector3& a, const ::Vector3& b);

        /// 逐分量取小。Unity 的 `Vector3.Min`。
        ZFUNCTION()
        static ::Vector3 Min(const ::Vector3& a, const ::Vector3& b);

        /// 逐分量取大。Unity 的 `Vector3.Max`。
        ZFUNCTION()
        static ::Vector3 Max(const ::Vector3& a, const ::Vector3& b);
    };

    /// Unity 的 `Quaternion` 静态面：单位四元数 + 常用运算。
    ZCLASS()
    class Quaternion : public Object
    {
        ZGENERATED_BODY(Quaternion)

    public:
        /// 无旋转。Unity 的 `Quaternion.identity`。
        ///
        /// 这是 Quaternion 里最常用的一个值：`transform.rotation =
        /// Quaternion.identity` 就是"摆正"。
        ZFUNCTION()
        static ::Quaternion GetIdentity();
        ZSCRIPTPROPERTY(Name = "identity", Getter = "GetIdentity")

        /// 绕 axis 转 angle_degrees 度（**度数**，不是弧度）。
        /// Unity 的 `Quaternion.AngleAxis`。
        ZFUNCTION()
        static ::Quaternion AngleAxis(float angle_degrees, const ::Vector3& axis);

        /// 朝 forward 看、up 定滚转。Unity 的 `Quaternion.LookRotation`。
        ZFUNCTION()
        static ::Quaternion LookRotation(const ::Vector3& forward, const ::Vector3& up);

        /// 球面插值。Unity 的 `Quaternion.Slerp`。
        ZFUNCTION()
        static ::Quaternion Slerp(const ::Quaternion& a, const ::Quaternion& b, float t);

        /// 归一化线性插值（比 Slerp 便宜，角度大时角速度不均匀）。
        /// Unity 的 `Quaternion.Lerp`。
        ZFUNCTION()
        static ::Quaternion Lerp(const ::Quaternion& a, const ::Quaternion& b, float t);

        /// 逆旋转。Unity 的 `Quaternion.Inverse`。
        ZFUNCTION()
        static ::Quaternion Inverse(const ::Quaternion& q);

        /// 点积。Unity 的 `Quaternion.Dot`。
        ZFUNCTION()
        static float Dot(const ::Quaternion& a, const ::Quaternion& b);

        /// 两个旋转之间的夹角，**度数**。Unity 的 `Quaternion.Angle`。
        ZFUNCTION()
        static float Angle(const ::Quaternion& a, const ::Quaternion& b);

        /// 朝 to 转，最多 max_degrees_delta 度。Unity 的 `Quaternion.RotateTowards`。
        ZFUNCTION()
        static ::Quaternion RotateTowards(const ::Quaternion& from, const ::Quaternion& to,
                                          float max_degrees_delta);

        /// 从 a 转到 b 的那个旋转。Unity 的 `Quaternion.FromToRotation`。
        ZFUNCTION()
        static ::Quaternion FromToRotation(const ::Vector3& a, const ::Vector3& b);

        /// 用四元数旋转向量（`q * v`）。Unity 里写成 `q * v` —— TS 没有运算符
        /// 重载，所以这里是个函数。
        ZFUNCTION()
        static ::Vector3 Rotate(const ::Quaternion& q, const ::Vector3& v);
    };

    /// Unity 的 `Vector2` 静态面：UI / 屏幕平面那套二维向量。
    ///
    /// ⚠️ 轴向与 `Vector3` 那套**不是一回事**，别把两边当同一个平面：
    ///   * `Vector2.up = (0,1)`、`Vector2.right = (1,0)` —— 屏幕平面，y 朝上、x 朝右。
    ///   * `Vector3.up = (0,0,1)`、`Vector3.right = (-1,0,0)` —— 世界，Z-up 且 local +X
    ///     是"左"（见文件头那张表）。
    /// 两边各自对着自己平面里"语义上的右/上"，所以都照 Unity 的数字给；`Vector2.right`
    /// 是 +X 而 `Vector3.right` 是 -X 不是笔误。`new Vector2(v3.x, v3.y)` 这种手抄
    /// 分量的写法要自己想清楚方向。
    ZCLASS()
    class Vector2 : public Object
    {
        ZGENERATED_BODY(Vector2)

    public:
        // ---- 常量 ----

        /// (0,0)。Unity 的 `Vector2.zero`。
        ZFUNCTION()
        static ::Vector2 GetZero();
        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")

        /// (1,1)。Unity 的 `Vector2.one`。
        ZFUNCTION()
        static ::Vector2 GetOne();
        ZSCRIPTPROPERTY(Name = "one", Getter = "GetOne")

        /// 上 = (0,1)。屏幕平面，不是世界的 +Z。
        ZFUNCTION()
        static ::Vector2 GetUp();
        ZSCRIPTPROPERTY(Name = "up", Getter = "GetUp")

        /// 下 = (0,-1)。
        ZFUNCTION()
        static ::Vector2 GetDown();
        ZSCRIPTPROPERTY(Name = "down", Getter = "GetDown")

        /// 左 = (-1,0)。
        ZFUNCTION()
        static ::Vector2 GetLeft();
        ZSCRIPTPROPERTY(Name = "left", Getter = "GetLeft")

        /// 右 = (1,0)。⚠️ 与 `Vector3.right`（-X）相反，理由见类注释。
        ZFUNCTION()
        static ::Vector2 GetRight();
        ZSCRIPTPROPERTY(Name = "right", Getter = "GetRight")

        /// 两个分量都是 +∞。Unity 的 `Vector2.positiveInfinity`。
        ZFUNCTION()
        static ::Vector2 GetPositiveInfinity();
        ZSCRIPTPROPERTY(Name = "positiveInfinity", Getter = "GetPositiveInfinity")

        /// 两个分量都是 -∞。Unity 的 `Vector2.negativeInfinity`。
        ZFUNCTION()
        static ::Vector2 GetNegativeInfinity();
        ZSCRIPTPROPERTY(Name = "negativeInfinity", Getter = "GetNegativeInfinity")

        /// 判等用的容差。Unity 的 `Vector2.kEpsilon`（1e-5）。
        ZFUNCTION()
        static float GetEpsilon();
        ZSCRIPTPROPERTY(Name = "kEpsilon", Getter = "GetEpsilon")

        // ---- 运算 ----

        /// 无符号夹角，**度数**，范围 [0,180]。Unity 的 `Vector2.Angle`。
        ZFUNCTION()
        static float Angle(const ::Vector2& from, const ::Vector2& to);

        /// 有符号夹角，**度数**，范围 (-180,180]。正值 = 从 from 逆时针转到 to。
        /// Unity 的 `Vector2.SignedAngle`。
        ZFUNCTION()
        static float SignedAngle(const ::Vector2& from, const ::Vector2& to);

        /// 把长度夹到 max_length 以内。Unity 的 `Vector2.ClampMagnitude`。
        ZFUNCTION()
        static ::Vector2 ClampMagnitude(const ::Vector2& v, float max_length);

        /// 距离。Unity 的 `Vector2.Distance`。
        ZFUNCTION()
        static float Distance(const ::Vector2& a, const ::Vector2& b);

        /// 点积。Unity 的 `Vector2.Dot`。
        ZFUNCTION()
        static float Dot(const ::Vector2& a, const ::Vector2& b);

        /// 线性插值，`t` **被夹到 [0,1]**（Unity 的 `Vector2.Lerp` 同样夹）。
        ZFUNCTION()
        static ::Vector2 Lerp(const ::Vector2& a, const ::Vector2& b, float t);

        /// 线性插值，`t` 不夹。Unity 的 `Vector2.LerpUnclamped`。
        ZFUNCTION()
        static ::Vector2 LerpUnclamped(const ::Vector2& a, const ::Vector2& b, float t);

        /// 长度。Unity 的 `Vector2.magnitude` 是实例属性，TS 的 Vector2 是 plain
        /// object，挂不了实例方法，所以这里只能是函数。
        ZFUNCTION()
        static float Magnitude(const ::Vector2& v);

        /// 长度的平方。
        ZFUNCTION()
        static float SqrMagnitude(const ::Vector2& v);

        /// 逐分量取大。Unity 的 `Vector2.Max`。
        ZFUNCTION()
        static ::Vector2 Max(const ::Vector2& a, const ::Vector2& b);

        /// 逐分量取小。Unity 的 `Vector2.Min`。
        ZFUNCTION()
        static ::Vector2 Min(const ::Vector2& a, const ::Vector2& b);

        /// 朝 b 走，步长不超过 max_distance。Unity 的 `Vector2.MoveTowards`。
        ZFUNCTION()
        static ::Vector2 MoveTowards(const ::Vector2& a, const ::Vector2& b, float max_distance);

        /// 归一化。零向量返回零向量（不是 NaN）。
        ZFUNCTION()
        static ::Vector2 Normalize(const ::Vector2& v);

        /// 逆时针转 90°：(-y, x)。Unity 的 `Vector2.Perpendicular`。
        ZFUNCTION()
        static ::Vector2 Perpendicular(const ::Vector2& v);

        /// 关于法线反射。Unity 的 `Vector2.Reflect`。
        ZFUNCTION()
        static ::Vector2 Reflect(const ::Vector2& v, const ::Vector2& normal);

        /// 逐分量相乘。Unity 的 `Vector2.Scale`。
        ZFUNCTION()
        static ::Vector2 Scale(const ::Vector2& a, const ::Vector2& b);

        /// 整体乘一个标量。Unity 里写成 `v * s` —— TS 没有运算符重载，所以这里是
        /// 个函数（同 `Quaternion.Rotate`）。**不是** Unity 的 API。
        ZFUNCTION()
        static ::Vector2 ScaleUniform(const ::Vector2& v, float scalar);
    };

    /// Unity 的 `Vector4` 静态面。
    ///
    /// ⚠️ `Magnitude` / `Normalize` 是**四维**的（含 w）。引擎值类型自带的
    /// `Vector4::normalise()` 只归一化 xyz（Ogre 的平面语义：xyz 是法线、w 是距离），
    /// 与 Unity 的 `Vector4.Normalize`（`v / v.magnitude`，四维）不是一回事，所以
    /// 这里自己算，不转发过去。
    ZCLASS()
    class Vector4 : public Object
    {
        ZGENERATED_BODY(Vector4)

    public:
        /// (0,0,0,0)。Unity 的 `Vector4.zero`。
        ZFUNCTION()
        static ::Vector4 GetZero();
        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")

        /// (1,1,1,1)。Unity 的 `Vector4.one`。
        ZFUNCTION()
        static ::Vector4 GetOne();
        ZSCRIPTPROPERTY(Name = "one", Getter = "GetOne")

        /// 四个分量都是 +∞。Unity 的 `Vector4.positiveInfinity`。
        ZFUNCTION()
        static ::Vector4 GetPositiveInfinity();
        ZSCRIPTPROPERTY(Name = "positiveInfinity", Getter = "GetPositiveInfinity")

        /// 四个分量都是 -∞。Unity 的 `Vector4.negativeInfinity`。
        ZFUNCTION()
        static ::Vector4 GetNegativeInfinity();
        ZSCRIPTPROPERTY(Name = "negativeInfinity", Getter = "GetNegativeInfinity")

        /// 距离（四维）。Unity 的 `Vector4.Distance`。
        ZFUNCTION()
        static float Distance(const ::Vector4& a, const ::Vector4& b);

        /// 点积（四维）。Unity 的 `Vector4.Dot`。
        ZFUNCTION()
        static float Dot(const ::Vector4& a, const ::Vector4& b);

        /// 线性插值，`t` **被夹到 [0,1]**。Unity 的 `Vector4.Lerp`。
        ZFUNCTION()
        static ::Vector4 Lerp(const ::Vector4& a, const ::Vector4& b, float t);

        /// 线性插值，`t` 不夹。Unity 的 `Vector4.LerpUnclamped`。
        ZFUNCTION()
        static ::Vector4 LerpUnclamped(const ::Vector4& a, const ::Vector4& b, float t);

        /// 长度（四维）。
        ZFUNCTION()
        static float Magnitude(const ::Vector4& v);

        /// 长度的平方（四维）。
        ZFUNCTION()
        static float SqrMagnitude(const ::Vector4& v);

        /// 逐分量取大。Unity 的 `Vector4.Max`。
        ZFUNCTION()
        static ::Vector4 Max(const ::Vector4& a, const ::Vector4& b);

        /// 逐分量取小。Unity 的 `Vector4.Min`。
        ZFUNCTION()
        static ::Vector4 Min(const ::Vector4& a, const ::Vector4& b);

        /// 朝 b 走，步长不超过 max_distance。Unity 的 `Vector4.MoveTowards`。
        ZFUNCTION()
        static ::Vector4 MoveTowards(const ::Vector4& a, const ::Vector4& b, float max_distance);

        /// 归一化（四维）。零向量返回零向量。
        ZFUNCTION()
        static ::Vector4 Normalize(const ::Vector4& v);

        /// 投影到 b 上。Unity 的 `Vector4.Project`。
        ZFUNCTION()
        static ::Vector4 Project(const ::Vector4& a, const ::Vector4& b);

        /// 逐分量相乘。Unity 的 `Vector4.Scale`。
        ZFUNCTION()
        static ::Vector4 Scale(const ::Vector4& a, const ::Vector4& b);

        /// 整体乘一个标量（同 `Vector2.ScaleUniform`，非 Unity API）。
        ZFUNCTION()
        static ::Vector4 ScaleUniform(const ::Vector4& v, float scalar);
    };

    /// Unity 的 `Matrix4x4` 静态面。
    ///
    /// 构造类函数（`Perspective` / `Ortho` / `LookAt`）走引擎 `Math::Make*`，与
    /// Unity 一样是 **OpenGL 约定**：右手系、裁剪空间 z ∈ [-1,1]（`Math.cpp` 里
    /// `MakePerspectiveMatrix` 的 `ret[2][2] = zfar / (znear - zfar)`、`ret[3][2] = -1`
    /// 就是这个意思）。⚠️ 所以 `Perspective` 的 fov 是**竖直** fov、**度**。
    ZCLASS()
    class Matrix4x4 : public Object
    {
        ZGENERATED_BODY(Matrix4x4)

    public:
        /// 单位矩阵。Unity 的 `Matrix4x4.identity`。
        ZFUNCTION()
        static ::Matrix4x4 GetIdentity();
        ZSCRIPTPROPERTY(Name = "identity", Getter = "GetIdentity")

        /// 全零矩阵。Unity 的 `Matrix4x4.zero`。
        ZFUNCTION()
        static ::Matrix4x4 GetZero();
        ZSCRIPTPROPERTY(Name = "zero", Getter = "GetZero")

        /// 行列式。Unity 的 `Matrix4x4.Determinant`。
        ZFUNCTION()
        static float Determinant(const ::Matrix4x4& m);

        /// 逆矩阵。奇异矩阵（行列式 0）返回**原矩阵**，不产生 NaN。
        /// Unity 的 `Matrix4x4.Inverse`。
        ZFUNCTION()
        static ::Matrix4x4 Inverse(const ::Matrix4x4& m);

        /// 转置。Unity 的 `Matrix4x4.Transpose`。
        ZFUNCTION()
        static ::Matrix4x4 Transpose(const ::Matrix4x4& m);

        /// 平移 × 旋转 × 缩放，Unity 的 `Matrix4x4.TRS`（顺序与 `Transform` 的
        /// 局部矩阵一致：`T * R * S`）。
        ZFUNCTION()
        static ::Matrix4x4 TRS(const ::Vector3& pos, const ::Quaternion& q, const ::Vector3& s);

        /// 纯缩放矩阵。Unity 的 `Matrix4x4.Scale`。
        ZFUNCTION()
        static ::Matrix4x4 Scale(const ::Vector3& v);

        /// 纯平移矩阵。Unity 的 `Matrix4x4.Translate`。
        ZFUNCTION()
        static ::Matrix4x4 Translate(const ::Vector3& v);

        /// 纯旋转矩阵。Unity 的 `Matrix4x4.Rotate`。
        ZFUNCTION()
        static ::Matrix4x4 Rotate(const ::Quaternion& q);

        /// 正交投影。Unity 的 `Matrix4x4.Ortho`。
        ZFUNCTION()
        static ::Matrix4x4 Ortho(float left, float right, float bottom, float top, float zNear,
                                 float zFar);

        /// 透视投影。`fov` 是**竖直**视野角、**度**（Unity 同）。
        /// Unity 的 `Matrix4x4.Perspective`。
        ZFUNCTION()
        static ::Matrix4x4 Perspective(float fov, float aspect, float zNear, float zFar);

        /// 视图矩阵（从 from 看向 to）。Unity 的 `Matrix4x4.LookAt`。
        ZFUNCTION()
        static ::Matrix4x4 LookAt(const ::Vector3& from, const ::Vector3& to, const ::Vector3& up);

        /// 矩阵相乘 `a * b`。Unity 里写成 `a * b` —— TS 没有运算符重载。
        ZFUNCTION()
        static ::Matrix4x4 Multiply(const ::Matrix4x4& a, const ::Matrix4x4& b);

        /// 变换一个**点**：受平移影响，且做透视除法。Unity 的
        /// `Matrix4x4.MultiplyPoint`。
        ZFUNCTION()
        static ::Vector3 MultiplyPoint(const ::Matrix4x4& m, const ::Vector3& v);

        /// 变换一个点，但**只用左上 3×4**（丢掉透视除法）。Unity 的
        /// `Matrix4x4.MultiplyPoint3x4`。
        ZFUNCTION()
        static ::Vector3 MultiplyPoint3x4(const ::Matrix4x4& m, const ::Vector3& v);

        /// 变换一个**方向**：不受平移影响。Unity 的 `Matrix4x4.MultiplyVector`。
        ZFUNCTION()
        static ::Vector3 MultiplyVector(const ::Matrix4x4& m, const ::Vector3& v);

        /// 取第 i 列。Unity 里是实例方法 `m.GetColumn(i)` —— 值类型在脚本侧是
        /// plain object，挂不了实例方法，所以改成静态。
        ZFUNCTION()
        static ::Vector4 GetColumn(const ::Matrix4x4& m, int i);

        /// 取第 i 行。同上（Unity 的 `m.GetRow(i)`）。
        ZFUNCTION()
        static ::Vector4 GetRow(const ::Matrix4x4& m, int i);

        /// 是不是单位矩阵。Unity 的 `Matrix4x4.isIdentity` 是实例属性。
        ZFUNCTION()
        static bool IsIdentity(const ::Matrix4x4& m);
    };
}  // namespace Runtime
