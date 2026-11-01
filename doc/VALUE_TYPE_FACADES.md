# 值类型的静态门面（`Vector2` / `Vector4` / `Matrix4x4`）

`engine/Source/Runtime/Scripting/ScriptVector/ScriptVector.{h,cpp}`（五个门面）
`engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp`（值类型的 `declare interface`）
`engine/Source/Runtime/Core/Math/Matrix3.h`（`Matrix3x3(const Quaternion&)`，本切片修掉的转置）

对照 Unity：`UnityEngine.Vector2` / `Vector4` / `Matrix4x4` 的**静态**成员。

---

## 1. 为什么又是三个"同名门面"

引擎的 `Vector2` / `Vector4` / `Matrix4x4` 是纯值类型（不派生 `Object`），进不了
绑定表 —— `ZCLASS()` 要求派生 `Object`。它们作为值跨边界：`ZScript::Converter<T>`
把它们搬成 plain object，`.d.ts` 里是 `declare interface Vector2`。

于是 Unity 那些静态成员（`Vector2.zero`、`Vector2.SignedAngle`、`Matrix4x4.TRS`）
没有地方挂。做法与既有的 `Vector3` / `Quaternion` / `Runtime::Color` 完全一样：

```
namespace Runtime {
    ZCLASS() class Vector2 : public Object { ... };   // 门面（只是命名空间）
}

declare interface Vector2 { x: number; y: number; }   // 值类型形状
declare class Vector2 { static readonly zero: Vector2; ... }   // 静态面
```

`.d.ts` 里 `interface` 与 `class` **同名合并**，合起来正是 Unity 的形状：既是值，
又是常量与工具函数的命名空间。

⚠️ 门面在 `namespace Runtime` 内，所以**值类型一律写全 `::Vector2`**。漏了 `::`
编译器会立刻报错（不会静默）—— 这正是选这个写法的原因。接线门 VW3 / VW4 钉这一条。

## 2. 三条容易踩的语义差异（都是**故意**的）

### 2.1 `Vector2.right = +X`，而 `Vector3.right = -X`

| | 平面 | up | right |
|---|---|---|---|
| `Vector2` | 屏幕 / UI | `(0,1)` | `(1,0)` |
| `Vector3` | 世界（Z-up） | `(0,0,1)` | `(-1,0,0)` |

两者都对着**各自平面**里语义上的"右"。本引擎 local +X 是"左"（`right = forward ×
up = (-Y) × (+Z) = -X`），所以世界的 `Vector3.right` 是 -X；屏幕平面没有这回事，
`Vector2.right` 就是 +X。把它们"统一"成一个符号是错的。

由 **VT5（行为）+ VW6（源码）+ Q2（TypeScript）** 三层钉住。

### 2.2 `Vector4` 的长度/归一化是**四维**的

引擎值类型自带的那份归一化只动 `xyz`（Ogre 的平面语义：`xyz` 是法线、`w` 是
距离）。Unity 的 `Vector4.magnitude` 是 `sqrt(Dot(v, v))` —— 含 `w`。

夹具 `(1,2,2,4)`：三维 hypot 是 **3**，四维是 **5**。转发错的那份会得到 `w = 4/3`，
而 Unity 给 `0.8`。

由 **VT28 / VT30 / VT31 + VW7 / VW8** 钉住。VW8 是源码级：门面的 `.cpp` 里**不许**
出现那个调用（出现过一次，被注释里的一句话误伤过，注释已改写法）。

### 2.3 `Matrix4x4.Perspective` / `Ortho` 是 OpenGL 约定

`Math::MakePerspectiveMatrix` 给的是 右手系 + 裁剪空间 `z ∈ [-1,1]`
（`m[2][2] = zfar / (znear - zfar)`、`m[3][2] = -1`），与 Unity 一致（不是 D3D 的
`0..1`）。`fov` 是**竖直**视野角，单位是**度**。

由 **VT51 / VT52 / VT53 + VW11** 钉住。

## 3. 顺手修掉的一个引擎内部分歧：`Matrix3x3(const Quaternion&)`

`Matrix4x4::Rotate(q)` 第一版用 `setMatrix3x3(Matrix3x3(q))`，结果 `+X` 被转到
`-Y` —— 与 `Matrix4x4::TRS(0, q, 1)`（走 `MakeTransform`）**相反**。

根因：

| 实现 | `m[0][1]` | 含义 |
|---|---|---|
| `Quaternion::ToRotationMatrix`（`MakeTransform` 用的那份） | `2xy - 2wz` | 右手系，转 `+A` |
| `Matrix3x3(const Quaternion&)`（原来手抄的那份） | `2xy + 2zw` | **转置**，转 `-A` |

`Matrix3.h` 文件头自己就写死了右手系（`RZ = [[cos,-sin,0],[sin,cos,0],[0,0,1]]`，
即 `(1,0,0) → (0,1,0)`），所以手抄那份是与**同一文件顶部的约定**相矛盾。

没人发现是因为这个构造**全工程零调用点**（唯一一个是本次新加的门面，加的那天就
红了）。修法是把它改成委托给唯一真源：

```cpp
Matrix3x3(const Quaternion& q) { q.ToRotationMatrix(*this); }
```

"四元数 → 矩阵"现在只有一份实现。

## 4. `Vector3.Scale` 的语义纠正

历史上 `Vector3.Scale(v, scalar)` 是**整体缩放**，而 Unity 的 `Vector3.Scale(a, b)`
是**逐分量相乘**。本切片按 Unity 纠正：

* `Vector3.Scale(a, b)` → `VectorMath::Mul`（逐分量）
* `Vector3.ScaleUniform(v, s)` → `VectorMath::Scale`（整体）

改之前全工程没有调用点（`VectorMath::Scale` 本身不受影响，`ScriptMathSmokeTest`
的 V2d 仍然钉着它）。

## 5. TS 没有运算符重载，所以多出几个非 Unity 的函数

| Unity 写法 | ZEngine |
|---|---|
| `v * s` | `Vector2.ScaleUniform(v, s)` / `Vector3.ScaleUniform` / `Vector4.ScaleUniform` |
| `a * b`（矩阵） | `Matrix4x4.Multiply(a, b)` |
| `m * v`（点） | `Matrix4x4.MultiplyPoint(m, v)` |
| `m.GetColumn(i)` | `Matrix4x4.GetColumn(m, i)`（静态，因为值类型挂不了实例方法） |

同理 `Vector2.magnitude` 在 Unity 是实例属性，这里只能是 `Vector2.Magnitude(v)`。

## 6. 没做的（及原因）

* `Vector2.SmoothDamp` / `Vector3.SmoothDamp`：需要 `ref currentVelocity`（出参），
  TS 没有。与 `Physics.Raycast` 出参同款问题（那里靠 `RaycastHit.valid` 绕开）。
* `Matrix4x4.SetColumn` / `SetRow`：值类型不可变（跨边界是拷贝），写了也改不动
  调用方手上的那份。需要"改一列"就用 `GetColumn` 取出来再 `Multiply` 拼回去。
* `Vector2.kEpsilonNormalSqrt`：Unity 有，但本引擎没有对应的归一化容差用途，
  先不给（给了就是一个没人用、也没人对得上的数字）。

## 7. 验证

| 层 | 位置 | 覆盖 |
|---|---|---|
| 行为 | `UnityParitySmokeTest` **VT0..VT59** | 常量、算术、四维 vs 三维、矩阵顺序、OpenGL 约定 |
| 接线 | `UnityParitySmokeTest` **VW0..VW17** | 门面存在、全称限定、不转发 Ogre 归一化、d.ts 合并 |
| 脚本链 | `Mover.ts` `ProbeValueTypes()` + `ScriptComponentSmokeTest` **Q0..Q2** | tsc 类型检查 + 运行时取值 |
| 变异 | `.mutate_vt.py` | 见下 |

变异测试：`.mutate_vt.py`（`.mutate_am.py` 同款框架）。每个变异体必须被**至少一条**
断言抓到，抓不到的要么补断言要么说明是等价变异。
