#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"

#include "ScriptVector.generated.h"

// =============================================================================
// `Vector3` / `Quaternion` —— Unity 那两个值类型的**静态面**。
//
// 为什么又是一对同名门面
// ----------------------
// 引擎的 `Vector3` / `Quaternion` 是纯值类型（POD，不派生 Object），进不了绑定表
// （ZCLASS 必须派生 Object）。它们作为值类型跨边界 —— `ZScript::Converter<T>`
// 把它们搬成 `{x,y,z}` / `{x,y,z,w}`，.d.ts 里是 `declare interface Vector3`。
//
// 于是 Unity 那些**静态成员**（`Vector3.zero`、`Vector3.up`、`Vector3.Distance`、
// `Quaternion.identity`）没有地方挂：`VectorMath` / `QuaternionMath` 是另一个名字，
// 从 Unity 搬过来的代码每个调用点都得改。这两个门面就是为了补上 Unity 的拼写。
//
// 做法与 `Runtime::Color`（`ScriptColor.h`）完全一样：门面放进 `namespace Runtime`，
// 值类型一律写全 `::Vector3` / `::Quaternion`（在命名空间内部，裸 `Vector3` 指的是
// 这个门面类，漏了 `::` 编译器会立刻报错，不会静默）。d.ts 侧
// `declare class Vector3` 与 `declare interface Vector3` **同名合并** —— 合起来
// 正是 Unity 的形状：既是值类型，又是常量与工具函数的命名空间。
//
// ⚠️ 轴向：Z-up，不是 Unity 的 Y-up
// ---------------------------------
// Unity：Y 轴朝上，`Vector3.up` = (0,1,0)、`Vector3.forward` = (0,0,1)。
// ZEngine：**Z 轴朝上**，相机默认 forward 是 `NEGATIVE_UNIT_Y`
// （`CameraComponent.cpp`）。所以：
//
//     up      = (0, 0, 1)        down  = (0, 0, -1)
//     forward = (0, -1, 0)       back  = (0, 1, 0)
//     right   = (1, 0, 0)        left  = (-1, 0, 0)
//
// 这里的名字对的是**语义轴**（"上"、"前"），不是坐标分量。从 Unity 搬过来的
// `transform.position = Vector3.up * 2` 因此仍然把物体举到引擎的天花板方向 ——
// 这正是要的效果；照搬 Unity 的数字才会把物体平推到墙里。`UnitX/Y/Z` 那种纯
// 轴向的写法留给 `VectorMath`。
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

        /// 右 = +X。Unity 的 `Vector3.right` 也是 +X。
        ZFUNCTION()
        static ::Vector3 GetRight();
        ZSCRIPTPROPERTY(Name = "right", Getter = "GetRight")

        /// 左 = -X。
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

        /// 逐分量缩放。Unity 的 `Vector3.Scale`。
        ZFUNCTION()
        static ::Vector3 Scale(const ::Vector3& v, float scalar);

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
}  // namespace Runtime
