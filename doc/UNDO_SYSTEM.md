# ZEngine 编辑器 Undo / Redo 事务系统

> 状态：已实现（2026-10-20）
> 代码位置：`engine/Source/Editor/Undo/`
> 设计参考：UE `FTransBuffer` / `FScopedTransaction` + Unity `Undo` 类

---

## 1. 背景

在此之前 ZEngine 编辑器**完全没有撤销能力**：gizmo 拖歪一下、Inspector 里改错一个数值、
误删一个物体，都只能手工改回去或者重新加载关卡。原因是场景编辑路径上是"直接写字段"，
没有任何地方留存"改之前是什么"。

本系统补上这一层，并把所有已有的编辑入口（gizmo / Inspector / Hierarchy / 菜单 / 删除）
统一接到它上面。

---

## 2. 架构

分两层，刻意让**栈逻辑与引擎无关**，这样它可以脱离编辑器做无头单元测试。

| 层 | 文件 | 依赖 | 职责 |
| --- | --- | --- | --- |
| 核心栈 | `Undo/UndoSystem.{h,cpp}` | 仅标准库 | 事务栈、记录、合并、限额 |
| 编辑器适配 | `Undo/EditorUndo.{h,cpp}` | 引擎 + 编辑器 | 拥有 `UndoSystem`，负责选区、脏标记、关卡绑定、播放模式 |
| 具体记录 | `Undo/EditorUndoRecords.{h,cpp}` | 引擎 | 组件快照、Transform 状态、GameObject 生命周期 |
| 测试 | `Undo/Test/UndoSystemSmokeTest.cpp` | 仅 `UndoSystem.cpp` | 70 项无头断言 |

```
Ctrl+Z / Edit 菜单 / console  ──►  EditorUndoSystem::PerformUndo()
                                          │
                                          ▼
                                    UndoSystem::Undo()
                                          │  逆序 Apply()
                                          ▼
                    IUndoRecord（ComponentSnapshot / TransformState / Lifetime）
                                          │
                                          ▼
                              Component / Transform / Level
```

---

## 3. 核心模型

### 3.1 记录是"交换式"的（Swap-based）

```cpp
class IUndoRecord {
    virtual void Apply() = 0;   // 把自己存的状态与现场状态交换
    virtual bool Finalize();    // 事务关闭时调用，返回 false 表示"没变"，记录被丢弃
    virtual uint64_t GetCoalesceKey() const;
    virtual size_t GetMemoryBytes() const;
};
```

`Apply()` 必须**自反**（`Apply(); Apply();` 回到原样），因此同一份代码同时服务撤销与重做，
记录不需要知道自己是在往哪个方向走。这也是 UE `FTransaction` 的模型。

记录**在修改发生之前**创建，保存"旧状态"；事务关闭时 `Finalize()` 对比新旧，没变就丢掉——
所以"点了一下但值没变"不会污染历史。

### 3.2 事务（Transaction）

* 一次 Ctrl+Z == 一个事务。
* 事务可嵌套，只有最外层 `Begin/End` 产生一个撤销步（UE `FScopedTransaction` 语义）。
* **不开事务也可以记录**（Unity `Undo.RecordObject` 语义）：记录落进"隐式事务"，
  由 `EditorUndoSystem::Tick()` 每帧 `FlushImplicitTransaction()` 收口。
  于是"一个事件处理函数里改了三个物体"天然是同一个撤销步，调用方不必记得开事务。
* `CancelTransaction()`：把已记录的内容全部回滚并丢弃（拖拽中按 Esc 用）。
* `GetCoalesceKey()`：同一事务里相同 key 只保留第一个快照（最旧的"改之前"才是对的）。

### 3.3 上下文（IUndoContext）

不是记录、但必须随事务一起回滚的旁路状态——编辑器用它保存/恢复**选区**
（UE 同样把选中项作为事务的一部分恢复）。

### 3.4 限额与分支

* 默认 200 步 / 256 MB，超出从最旧的开始丢弃。
* 撤销后再做新编辑 → 清空 redo 分支（所有编辑器行为一致）。
* 正在 Apply / 播放模式下 → 拒绝记录（`CanRecord()`），避免"应用一个记录时又记录一个"。

---

## 4. 三种记录

| 记录 | 用于 | 实现要点 |
| --- | --- | --- |
| `ComponentSnapshotRecord` | 任意组件字段（材质、网格、动画、开关…） | 走正常 `Transfer()` 路径写一份 **YAML DOM**；float 位精确还原；快照前压入 `ScopedPPtrResolver(nullptr)`，让 PPtr / ImmediatePtr 落成裸 InstanceID |
| `TransformStateRecord` | 位置 / 旋转 / 缩放 / 父节点 / 兄弟序号 / 激活 | 不能用裸快照覆盖层级链接，改为走 `SetParent(..., kLocalPositionStays)` + `SetSiblingIndex()` 等层级 API |
| `GameObjectLifetimeRecord` | 创建 / 删除 | **自己持有被删掉的整棵子树**（UE 事务缓冲模型），撤销时把同一批对象、同一批 InstanceID 放回关卡，而不是克隆一份新的 |

记录一律**按 InstanceID** 通过 `ObjectManager::IDToPointer` 解析目标，绝不保存裸指针——
撤销/重做之间的对象早就不在同一个地址上了。

---

## 5. 调用方式

```cpp
// 推荐：显式命名一个步骤（可跨帧，如 gizmo 拖拽）
EditorUndo::ScopedTransaction transaction("Set Material");
EditorUndo::RecordComponent(material_component);
material_component->SetMaterial(path);

// 不命名也行：本帧所有记录自动合成一步
EditorUndo::RecordTransform(transform);
transform->SetLocalPosition(p);

// 创建对象之后
auto* go = GET_SYSTEM(ObjectManager)->NewObject<GameObject>();
level->AddGObject(go);
EditorUndo::RegisterCreatedGameObject(go->GetID(), "Create Empty");

// 删除必须走这里，才会产生撤销步
EditorUndo::DestroyGameObjects(selected_ids, "Delete");
```

`ScopedTransaction` 还有 `Cancel()`：回滚并撤销整个作用域（不想提交时用）。

> ⚠️ 约定：**先 Record，后改**。记录的是"改之前"的状态。

---

## 6. 编辑器接入点

| 位置 | 撤销步标题 |
| --- | --- |
| `EditorSceneManager::MoveEntity`（gizmo 拖拽） | `Move` / `Rotate` / `Scale`（拖拽期间合并成一步） |
| `EditorSceneManager::OnDeleteSelectedGObject` | `Delete` |
| `ZSlateInspectorWindow`（所有字段提交） | `Set Mesh` / `Set Material` / `Modify Transform` / … |
| `ZSlateHierarchyWindow::CreateEmpty` | `Create Empty` |
| `EditorHierarchyReparent` | `Reparent` |
| `EditorScenePlacement`（拖 prefab / mesh 进场景） | `Instantiate Prefab` / `Instantiate Mesh` |

快捷键：`EditorInputManager::OnKey` —— Ctrl/Cmd+Z、Ctrl+Y、Ctrl+Shift+Z。
菜单：`MenuController` —— Edit ▸ `Undo <title>` / `Redo <title>` / `Undo History`（20 条，可一次回退 N 步）/ `Clear Undo History`。
控制台：`undo [steps]`、`redo [steps]`、`undo.history [max rows]`、`undo.clear`。

---

## 7. 为它做的运行时改动

| 改动 | 为什么 |
| --- | --- |
| `Component::PostEditUndo()` | 撤销后给组件一次刷新机会；`AnimationComponent` 覆写它以重新解析骨架 / 控制器（只在 `PostLoadResource` 里绑定） |
| `Level::GetLoadSerial()` | load / unload 时自增，Undo 系统据此判断"关卡被重载过"，历史作废 |
| `Level::RestoreGObject()` | 让被摘掉的对象带着**原 ID** 回到关卡，PPtr 引用继续有效 |
| `RenderSwapContext::CancelDeleteGameObject()` | swap 是先 upsert 后 delete，所以同帧内撤销"复活"一个对象必须把它从删除队列里撤回来 |
| `TransformSceneRoots` 两处修复 | 原来 `OnTransformBecameRoot` 会重复挂载 last root、`OnTransformLeftRoot` 对未挂载的 transform 会清空整条根链表（每次 `PostLoadResource` 都触发）；撤销/重做频繁挂载卸载后必然踩到 |

---

## 8. 测试

```bash
cmake -S . -B build -DZENGINE_BUILD_UNDO_SMOKE_TEST=ON
cmake --build build --target UndoSystemSmokeTest
```

70 项断言 / 0 失败，覆盖：交换语义、嵌套、逆序/顺序、空操作丢弃、合并、redo 分支清空、
隐式分组、事务未关闭时拒绝撤销、Cancel、Apply 期间的重入保护、限额、上下文、监听器与 state id、标题。

---

## 9. 已知边界（后续可补）

* **资产编辑不在覆盖范围内**：材质 / 纹理 / 数据表由各自的 Inspector 直接写盘，不走 `UndoSystem`。
* **组件的增删没有编辑器 UI**，因此也没有对应的撤销入口。
* **播放模式下的修改不记录**（与 Unity/UE 一致）。
* 内存预算目前只统计记录自报的 `GetMemoryBytes()`，是估算值。
