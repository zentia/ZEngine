# Transform 脚本 API（方向向量 / 欧拉角 / Translate / Rotate / LookAt）

对应 Unity：`UnityEngine.Transform` 的方向属性、`eulerAngles`、`Translate` /
`Rotate` / `RotateAround` / `LookAt` / `IsChildOf` / `DetachChildren`。

代码：`Source/Runtime/Function/Framework/Component/Transform/Transform.{h,cpp}`
门禁：`UnityParitySmokeTest` 的 **S 组**（行为）与 **T 组**（接线）。

---

## 1. ⚠️ 轴向约定：ZEngine 是 Z-up、forward = -Y

这是本文最重要的一节。两套基都是右手系（`X × Y = Z` 都成立），只是"哪个轴朝前"不同：

|              | X    | Y      | Z    |
|--------------|------|--------|------|
| **ZEngine**  | 左   | **-前** | 上   |
| Unity        | 右   | 上      | 前   |

```
GetForward() = TransformDirection(-Y)      // 与 MotorComponent 的 rotation * NEGATIVE_UNIT_Y 一致
GetBack()    = TransformDirection(+Y)
GetUp()      = TransformDirection(+Z)
GetDown()    = TransformDirection(-Z)
GetRight()   = TransformDirection(-X)      // local +X 是左，所以右是 -X
GetLeft()    = TransformDirection(+X)
```

**为什么不能按 Unity 的 +Z 写**：`transform.GetForward() * speed * dt` 会让角色朝侧面走，
而且渲染看不出错（模型朝向本来就对），只有物理和移动会偏。

右手系的可验证恒等式（S15c 断言的就是这两条）：

```
up × right   = forward
forward × up = right
```

写成左手系的话"朝向看着对"但整体镜像，位移和旋转方向全反。

---

## 2. 脚本面：属性 vs 方法

Unity 里 `forward` / `up` / `right` / `eulerAngles` 是**属性**，不是方法。
ZHeaderTool 的 `ZSCRIPTPROPERTY` 把已有的 `ZFUNCTION` getter/setter 对发布成
pesapi 访问器属性，所以两种拼写都能用：

```ts
t.forward = Vector3(0, 1, 0);     // 属性
t.SetForward(Vector3(0, 1, 0));   // 方法
```

| 属性 | getter / setter | 只读 |
|------|-----------------|------|
| `forward` | `GetForward` / `SetForward` | |
| `up` | `GetUp` / `SetUp` | |
| `right` | `GetRight` / `SetRight` | |
| `eulerAngles` | `GetEulerAngles` / `SetEulerAngles` | |
| `localEulerAngles` | `GetLocalEulerAngles` / `SetLocalEulerAngles` | |
| `childCount` | `GetChildrenCount` | ✓ |
| `root` | `GetRoot` | ✓ |
| `parent` | `GetParent` | ✓ |

唯一的**不对称**是 `parent`：Unity 可写（可以 `t.parent = other`），这里只能读。

> 原因：属性 setter 只接受一个参数，而 `SetParent` 的脚本拼写是
> `SetParent(parent: Transform, worldPositionStays: boolean)`（两个）。
> ZHeaderTool 里一个脚本名只能被一个重载占用，不可能同时有
> `SetParent(t)` 和 `SetParent(t, bool)`。要改父级就调 `SetParent`。

---

## 3. 欧拉角

单位**度**，合成顺序 **X → Y → Z**（等价于矩阵 `Rz·Ry·Rx`，四元数 `qz*qy*qx`）。

> Unity 的合成顺序是 `Z·X·Y`（Y-up 那套）。两者**不可互换**：
> 同一组 `(x,y,z)` 在两个引擎里是不同朝向。

万向锁：`GetEulerAngles(SetEulerAngles(v))` 只在非退化区域等于 `v`；
Y 分量接近 ±90° 时 x/z 会互相吞并（与 Unity 同）。需要精确存朝向就用
`GetRotation()` / `SetRotation()` 传四元数。

`EulerFromQuaternion` 从矩阵的 `r[2][0]` 反解 `sin(b)`，**必须 clamp 到 [-1,1]**
—— 浮点误差推出去一点，`asin` 就返回 NaN。

---

## 4. 平移 / 旋转

| 方法 | 语义 |
|------|------|
| `Translate(v)` | 沿**自身**轴（Unity `Translate(v, Space.Self)`） |
| `TranslateWorld(v)` | 沿**世界**轴 |
| `Rotate(euler)` | 绕自身轴（**右乘**：`GetLocalRotation() * Δ`） |
| `RotateAxis(axis, angle)` | 绕**世界**轴（**左乘**：`Δ * GetRotation()`） |
| `RotateAround(point, axis, angle)` | 绕世界空间一个点转：朝向和位置都要转 |
| `LookAt(pos)` / `LookAtWithUp(pos, up)` | 朝向对准某点，up 默认世界 +Z |

Unity 用 `Space` 枚举区分两个 `Translate`；这里因为"一个脚本名只属于一个重载"，
拆成两个名字。

`RotateAround` 只转朝向不改位置的话，会变成"绕着点滑动但始终面朝同一边"——
两行都要有（S22 / S23 分别守住这两条）。

**退化输入一律空操作，不写 NaN**：零轴、零长度朝向、LookAt 自己的位置。
NaN 会一路污染到渲染矩阵，而且极难回溯来源。

---

## 5. 层级

| 方法 | 语义 |
|------|------|
| `IsChildOf(other)` | 自己或**任一祖先**是 `other`（不只看直接父级） |
| `DetachChildren()` | 解除**直接**子节点（挂到场景根），只解一层 |

### `DetachChildren` 必须遍历子节点列表的**拷贝**

```cpp
std::vector<PPtr<Transform>> children = m_Children;   // 拷贝
for (const PPtr<Transform>& child : children) { child->SetParent(nullptr, true); }
```

`SetParent(nullptr)` 会从 `m_Children` 里 erase 掉自己，而 **range-for 的 `end()`
只在循环开始时求一次值**，`erase` 之后它仍指向旧的第 N 个槽位。

⚠️ **两个儿子时这个 bug 看不出来**：erase 只把后面的元素前移、不清空尾部字节，
于是第二轮恰好又读到了那个还没解开的儿子，结果"完全正确"。**三个儿子**才必然
跳过中间那个 —— 所以门禁的夹具用的是 3 个子节点（S31）。

---

## 6. 门禁（`UnityParitySmokeTest`）

**S 组（行为）** 用真实 Transform 实例：

- S2–S8：identity 下六个方向向量，且和引擎自己用的常量一致
- S9–S10：`RotateAxis(Z, 90)`
- S11–S15：`SetForward` / `SetUp`，含"forward 与 up 平行"的退化输入
- S15a–S15d：`SetRight`，含右手系恒等式与**镜像叉乘的反向控制**
- S16–S18：欧拉角往返（含混合角）
- S19–S20：`Translate` 自身轴 vs `TranslateWorld` 世界轴
- S21–S23：`Rotate` / `RotateAround`
- S24–S25b：`LookAt`，含"目标是自己"必须 no-op 且**不能悄悄重置成单位朝向**
- S26–S36：三层层级 + `IsChildOf` + 三个子节点的 `DetachChildren`

**T 组（接线）**：

- T1–T2：轴向约定必须写在头文件里（最容易"A 组按 -Y 写、B 组按 +Z 用"的一处，
  而且两边都编译得过）
- T10：`DetachChildren` 遍历拷贝
- T11–T12：世界轴左乘 / 自身轴右乘
- T13：退化保护
- T14–T19：属性形态（`ZSCRIPTPROPERTY`）确实存在

### 夹具的两个坑（改测试时别踩回去）

1. **不能用 `ObjectManager::Produce(TypeOf<Transform>(), 0)`**。Transform 的工厂
   是 `IMPLEMENT_REGISTER_CLASS(Transform)` 填进 `Type` 的，而那行在 ZHeaderTool
   生成的 `Transform.gen.cpp` 里；本 exe 按规矩不编任何 `.gen.cpp`（编了会因
   `/WHOLEARCHIVE` 拿到第二份 `TypeContainer<T>::m_Type`），所以这里的
   `TypeOf<Transform>()` 是 `factory == nullptr` 的占位 Type，`Produce` 一律返回
   nullptr。走 `new Transform()` + `AllocateAndAssignInstanceID()`。
2. **不能只 `new Transform()`**。InstanceID 为 0 对 PPtr 等于"不存在"
   （`PPtr::operator T*()` 在 `m_InstanceID == 0` 时短路返回 nullptr），而
   `m_Children` 是 `PPtr<Transform>` —— 记下来的儿子下一行就读不回来，
   层级操作全线静默失效。
3. `addComponent()` 只塞 `m_Components`，**owner 反向指针是 `PostLoadResource()`
   设的**，漏了它 `m_ParentObject` 是 nullptr。
