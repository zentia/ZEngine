# `Transform` 补齐面：`SetPositionAndRotation` / `hasChanged` / `hierarchyCapacity` / `hierarchyCount` / `MoveAfterSibling`

对应 Unity：`UnityEngine.Transform.SetPositionAndRotation`、
`Transform.SetLocalPositionAndRotation`、`Transform.hasChanged`、
`Transform.hierarchyCapacity`、`Transform.hierarchyCount`、
`Transform.MoveAfterSibling`（2020.1+），加上 `localToWorldMatrix` /
`worldToLocalMatrix` 的**属性拼写**。

| 文件 | 内容 |
|---|---|
| `Runtime/Function/Framework/Component/Transform/Transform.h` | 声明 + `ZSCRIPTPROPERTY` + `m_HasChanged` |
| `Runtime/Function/Framework/Component/Transform/Transform.cpp` | 实现，`SetSiblingIndex` 拆出 `ReorderWithinParent` |

```ts
t.SetPositionAndRotation(new Vector3ish(1, 2, 3), q);        // 世界
t.SetLocalPositionAndRotation(new Vector3ish(4, 5, 6), q);   // 局部
if (t.hasChanged) { /* ... */ t.SetHasChanged(false); }      // 轮询 + 清零
const n: number = t.hierarchyCount;                          // 整份层级的节点数
t.hierarchyCapacity = 64;                                    // 只增不减
t.MoveAfterSibling(otherSibling);                            // "排到它后面"
const m: Matrix4x4 = t.localToWorldMatrix;                   // 属性拼写
```

## 1. ⚠️ `hasChanged` 不是 `IsDirty()`

`Component` 上本来就有一个 `m_IsDirty`，意思也差不多，而且**已经在所有改动
路径上被置位**了 —— 所以"顺手复用它"是最自然的错实现。它错在：

* `m_IsDirty` 是"世界矩阵待重算"，**帧末就被消费掉**；
* `m_HasChanged` 只有用户显式写 `false` 才清，帧循环不碰它。

用 `m_IsDirty` 顶替的话，轮询式用法（`if (t.hasChanged) { ...; t.SetHasChanged(false); }`）
会在某一帧之后永远读到 `false`，而且是间歇性的 —— 取决于那一帧有没有别的东西
把它置位。判据 TE17 直接把两者放在一起比：`t.GetHasChanged() == false && t.IsDirty() == true`。

置位统一放在 `MarkTransformDirty()`（所有改动的公共出口），而不是散在各个
setter 里 —— 漏在 `SetParent` / `SetSiblingIndex` / `MoveAfterSibling` 上的话，
表现是"位置变了但 `hasChanged` 没变"。

**只标记自己**：子节点动不影响父节点的 `hasChanged`（Unity 同）。判据 TE15/TE16。

**加载后是 `false`**：`PostLoadResource()` 末尾显式清零。加载是基准线，不是一次
改动；否则脚本一拿到刚加载的 transform 就看到 `true`，轮询的第一次必然误判。

## 2. `hierarchyCount` 是**整份层级**，不是自己的子树

```
root
 ├── A ── G
 ├── B
 └── C
```

* `root.hierarchyCount === 5`
* `A.hierarchyCount === 5`（**不是 2**）
* `G.hierarchyCount === 5`

`hierarchyCapacity` / `hierarchyCount` 在 Unity 里描述的是同一份层级数据结构，
同层级内任何 transform 读到的都一样。要自己子树的大小就自己累加 `childCount`。

实现上：层级已建立时读 `deep_child_count[0]`（根节点那一格就是"自己 + 全部
后代"）；还没进场景时退化成按 `m_Children` 递归，数出来是同一个数。

## 3. `hierarchyCapacity` 只增不减

容量小于当前节点数时**静默忽略**，不缩、不报错。原因是 `TransformHierarchy`
是一组定长数组（`local_transforms` / `parent_indices` / `next_indices` / ...），
缩小就得搬移活着的数据；而"用户想把容量压回去"换来的只是省一点内存 —— 容量
在 Unity 那边本来也只是一个提示（`EnsureCapacityIncrease` 会自己按 `*2` 扩）。

判据 TE27/TE28/TE29：要 1、要 -7 之后，节点数仍然是 6，容量仍然 `>= count`。

扩容之后**还能继续加节点**（TE25）—— 这才是"扩容没把层级搞坏"的判据。只断言
容量数字的话，`GrowTransformHierarchyCapacity` 深拷贝时漏搬一两个数组
（`parent_indices` / `next_indices`）完全看不出来。

## 4. `MoveAfterSibling` 的下标要按"移除自己之后"算

```ts
// 自己在目标前面 -> 移除后目标左移一位 -> 插到 sib
// 自己在目标后面 -> 目标不动         -> 插到 sib + 1
```

写成统一 `sibling_index + 1` 的话，"往前挪"（跨过目标）那一步是**空操作** ——
而这一步恰好是手写测试时最不常写的那个方向。夹具 `ABC` 三个儿子专门覆盖：

| 步骤 | 期望 |
|---|---|
| `A.MoveAfterSibling(B)` | `BAC` |
| `B.MoveAfterSibling(A)` | `ABC`（往前挪，off-by-one 的那一半） |
| `A.MoveAfterSibling(C)` | `BCA` |
| `C.MoveAfterSibling(B)`（已在后面） | `BCA`，不是交换 |

静默空操作的三种输入（Unity 是记一条错误日志然后什么都不做，不抛）：
`sibling` 为 null / 是自己、自己没有父节点、两者父节点不同。跨父级搬移要用
`SetParent` —— 它要决定 `worldPositionStays`，这里猜不出来。

### 为什么要从 `SetSiblingIndex` 里拆出 `ReorderWithinParent`

`SetSiblingIndex` 开头有一句"已经在那儿了就返回"的提前退出。这条对它是对的
（`SetSiblingIndex(i)` 是幂等的），但 `MoveAfterSibling` 不能走：

> 子节点 `[A, B, C]` 里 `A.MoveAfterSibling(B)` 算出的目标下标是 **0**，正好是
> A 现在的位置 —— 走提前退出就变成空操作了，而正确答案是把 A 挪到 B 后面
> （`BAC`）。

所以 `ReorderWithinParent(parent, new_index)` 只保留"真的改顺序 + 修层级 + 置脏"
那一段，两个入口各自做自己的参数校验。

## 5. `SetPositionAndRotation` 与 `SetLocalPositionAndRotation`

底层就是两次调用（没有更省的写法，将来内部做成单次更新时脚本不用改）。导出
它们的理由是 Unity 有，且搬代码时写起来一样。

⚠️ 判据必须让**父节点带位移**：`root` 在 `(10,0,0)` 时，世界 `(1,2,3)` 的局部
坐标是 `(-9,2,3)`。用根对象当夹具的话两个版本完全等价 —— 这就是"转发到 local
版本"这种错实现最常见的漏网点。

朝向的判据用 `forward`：ZEngine 是 Z-up、`forward = -Y`，绕 Z 转 90° 之后
`forward` 变成 `+X`。只看位置断言的话，"漏设朝向"那一行删掉也全绿。

## 6. 矩阵的属性拼写

`GetLocalToWorldMatrix()` / `GetWorldToLocalMatrix()` 早就导出为方法了；这一片
补的是 `ZSCRIPTPROPERTY`，让脚本能写 `t.localToWorldMatrix`（与 Unity 同形）。
两条路都留着。

只读：Unity 也不允许写 —— 矩阵是 position / rotation / scale 的派生量，非均匀
缩放 + 剪切分不开，写它没有唯一解。

## 7. 判据与变异

| 段 | 位置 | 覆盖 |
|---|---|---|
| TE0–TE29 | `UnityParitySmokeTest.cpp` | 世界/局部、矩阵、hasChanged、hierarchy* |
| MA0–MA13 | 同上（独立夹具） | `MoveAfterSibling` |
| TEW0–TEW15 | 同上 | 源码门（ZFUNCTION / ZSCRIPTPROPERTY / 成员独立性 / 复用） |
| Z6–Z7 | `ScriptComponentSmokeTest.cpp` + `Mover.ts` | TypeScript 端到端 |

`.mutate_te.py` 的变异体全是"看起来合理"的错实现：转发到 local 版本（根对象上
等价）、`hasChanged` 复用 `m_IsDirty`、`hierarchyCount` 数子树（根节点上等价）、
`MoveAfterSibling` 两分支写反。18/18 被抓到。

第一轮有两个漏了，都是**测试**的洞而不是实现的洞：

| 变异 | 为什么漏 | 修法 |
|---|---|---|
| TEM14 去掉 `sibling == this` 那道闸 | 唯一的"排到自己后面"断言发生时自己正好是**最后一个**，目标下标被夹回末尾，顺序碰巧不变 | 加 MA2b：在自已是**第一个**时判 |
| TEM15 去掉 `MoveAfterSibling` 的 `ZFUNCTION()` | 接线门只 grep 方法名，而声明还在 —— "字符串在、绑定不在" | TEW1 改成 grep `ZFUNCTION()` **连着声明那一行** |
