# 脚本侧场景与生命周期 API

`GameObject.Instantiate` / `Find` / `Destroy` / `SetActive`。这四个之前一个都没有 ——
脚本只能从 `this.gameObject` 出发，拿不到场景里别的对象，创建不了对象，也关不掉对象。

> 配套：向量 / 旋转 / 标量的运算在 `doc/SCRIPT_MATH_API.md` —— 脚本侧拿到的
> `Vector3` 是纯数据（一个方法都没有），要算东西得用那里的 `VectorMath` /
> `QuaternionMath` / `Mathf`。

```typescript
const bullet = GameObject.Instantiate("Assets/Prefabs/Bullet.prefab");
bullet.transform.position = { x: 0, y: 0, z: 0 };

const player = GameObject.Find("Player");       // O(n)，别每帧调
if (player != null) GameObject.Destroy(player);

const boss = GameObject.Find("Boss");
if (boss != null) boss.SetActive(false);        // 停 Tick + 停渲染 + 子树一起停
if (boss != null && boss.activeInHierarchy) { /* ... */ }
```

当前导出的 `GameObject`（`--class GameObject` 可以打印）：

```
GetID(): number
SetName(name: string): void
GetName(): string
HasComponent(component_type_name: string): boolean
GetComponent(component_type_name: string): Component
AddComponent(component_type_name: string): Component
GetTransform(): Transform
Instantiate(prefab_path: string): GameObject     [static]
Find(name: string): GameObject                   [static]
Destroy(gobject: GameObject): void               [static]
SetActive(active: boolean): void
GetActiveSelf(): boolean
IsActiveInHierarchy(): boolean
```

访问器属性（由 `ZSCRIPTPROPERTY` 发布，不是方法）：

```
activeSelf        : boolean   // 读写，等价于 SetActive / GetActiveSelf
activeInHierarchy : boolean   // 只读（祖先决定，不能直接写）
```

---

## 1. 为什么单开一张运行时对象表

`Level::m_RuntimeGObjects`（非拥有，只存裸指针），而不是把 `Instantiate` 的产物塞
进 `m_Gobjects`。两个理由，都踩不得：

| | `m_Gobjects`（关卡表） | `m_RuntimeGObjects`（运行时表） |
| --- | --- | --- |
| 所有权 | `shared_ptr`，是唯一持有者 | 不拥有，归 `ObjectManager` |
| 谁创建 | `Level::CreateObject`（`make_shared`） | `ObjectManager::Produce`（`new`） |
| 会被保存吗 | 会（`Level.cpp:394` 遍历它收集写 .level 文件） | 不会 |
| 销毁方式 | `erase` 掉 map 条目即析构 | 必须 `ObjectManager::Destroy` |

把 `Produce` 出来的裸指针包进 `shared_ptr` 就是双重释放 —— 这正是
`PrefabUtility.cpp:703` 警告过的同一类错配：`Produce()` 内部是 `new T()`，Apple 上
全局 `operator new` 故意不接管 mimalloc，喂给 `mi_free` 会在 `mi_usable_size` 里
EXC_BAD_ACCESS / SIGBUS；Windows/Linux 上 `new` 就是 mimalloc，所以这条错配一直是
隐形的。

持久化语义也一样重要：运行时生成的子弹不该被写进关卡文件。

`RegisterRuntimeGObject` 撞 id 时**不覆盖**：本表非拥有，覆盖会丢掉前一个对象的
唯一引用，等于它再也 Destroy 不掉 —— 泄漏和悬挂两个错误一起犯。

## 2. Instantiate 为什么重新发 GObjectID

`CloneSubtree` 走的是**序列化往返**（`TransferUtility::CloneObjectViaSerialization`），
`m_Id` 会跟着模板一起被拷出来。两次 `Instantiate` 同一个 prefab 就得到两个 ID 相同
的 GameObject，第二个进表时会被拒。

所以整棵子树（沿 `Transform::GetChildren`）都重新 `ObjectIDAllocator::Alloc()` 一个。
用显式栈做 DFS 而不是递归：子树深度由 prefab 决定，深 prefab 有爆栈风险。

## 3. Destroy 为什么延迟到帧末

`Level::DeleteGObjectByID` 只从表 `erase`（`Level.cpp:583`），注释明说不管 transform
父子、`TransformSceneRoots`、render state。直接拿它当 Destroy 用，场景里会留一个野的
render entity，以及一群 `m_Father` 指向已释放内存的子对象。

**延迟**：脚本可以在任何地方调 Destroy，包括在某个对象的 `OnUpdate` 里销毁另一个
对象 —— 而 `Level::Tick` 手里正拿着两张表的迭代器，立即销毁会让它们当场失效。
`QueueDestroy` 只记 id，真正销毁在 Tick 末尾的 `FlushPendingDestroys`（Unity 同语义）。

同一帧重复 Destroy 同一个对象会去重：不去重的话第二次会去碰一个已经析构的对象，而
销毁是延迟的，第二次请求到达时对象可能还没死 —— 于是"看起来能跑"，直到某次顺序变了
才崩。`FlushPendingDestroys` 先把队列 swap 出来再处理：销毁过程中有人再 QueueDestroy
（例如某个组件的 OnDestroy 又销毁别的对象）属于下一帧，不该在这轮被顺手做掉。

**三处清理**：

1. 子对象提升到场景根（`SetParent(nullptr)`）。
2. 自己也 `SetParent(nullptr)` —— `SetParent` 内部走 `DetachFromParentList` +
   `UpdateSceneRootRegistration`，所以 scene roots 的注册一并更新了。
3. 渲染侧发一条 `AddDeleteGameObject`。`Level::FlushRenderDeletes` 是卸载时批量发的，
   单个对象要自己发。

**两条销毁路径不能混**：主表对象 erase 即析构（再 Destroy 是双重释放）；运行时对象
必须 `ObjectManager::Destroy`（只 erase 会泄漏，组件全留在 ObjectManager 表里）。

### 与 Unity 的一处刻意差异

Unity 的 `Destroy(gameObject)` 会连整棵子树一起销毁。这里**只销毁指定的那一个**，
子对象提升到场景根。改动前先想清楚谁会依赖它。

## 4. SetActive / activeSelf / activeInHierarchy

### 存储复用 `Transform::m_Enabled`，不新建字段

三个既有消费方已经把它当 activeSelf 用了：`GameObject::Tick`、
`BaseRenderer::SubmitRenderState`、Timeline 的 activation clip
（`runtime.saved_active = transform->isEnabled()`）。再建一个 `m_ActiveSelf`
会立刻变成两个真值源 —— 一边开一边没开，谁也说不清。所以 `GameObject::SetActive`
只是"写这个 flag + 触发副作用"的**唯一入口**。

### 两个概念（与 Unity 同）

| 概念 | 含义 | 谁的开关 |
| --- | --- | --- |
| `activeSelf` | 自己这一个开关，不管祖先 | 可读写（`SetActive` / `GetActiveSelf`） |
| `activeInHierarchy` | 自己**且**所有祖先都开着 | 只读，由祖先决定 |

父级被停用时，子对象的 `activeSelf` **不变**，只有 `activeInHierarchy` 变 false。
这样父级恢复后，本来就关着的子对象不会跟着被打开。

### 为什么必须由 GameObject 主动推给组件

`GameObject::Tick` 在对象未激活时**直接 return**，组件的 `Tick` 再也跑不到。也就是说
"停用"这件事在时序上没有任何一次 Tick 能替组件收尾 —— `BaseRenderer` 里那条
"提交空 parts 撤掉渲染实体"的分支一直是**死代码**：对象停用了，画面上还留着。

所以在 `Component` 上加了虚钩子 `OnOwnerActiveChanged(bool)`，`SetActive` 推一次。
不排队到帧末：要的就是同一帧内画面跟着变。

### 子树

父级开关一变，整棵子树的 `activeInHierarchy` 都变了，所以 `SetActive` 沿
`Transform::GetChildren()` 递归重推。每个后代推的是**自己算出来的**值，不是父级传下来的
值 —— 本来就关着的那些，父级恢复后仍然该是关的。

### 覆盖到的消费方

| 消费方 | 位置 | 改了什么 |
| --- | --- | --- |
| Tick | `GameObject::Tick` | 从"只查自己的 Transform"改成 `IsActiveInHierarchy()` |
| 网格渲染 | `BaseRenderer::SubmitRenderState` | `object_active` 改成 `IsActiveInHierarchy()`；新增 `OnOwnerActiveChanged` 重发 |
| GPU-Spine | `SpineRenderer::PushTransform` / `OnOwnerActiveChanged` | `active` 改成 `IsActiveInHierarchy()`；停用后补推一次 `visible` |
| 灯光 | `RenderScene::SyncPointLightsFromLevel` | 新增 `IsActiveInHierarchy()` 门禁（原来只查 `LightComponent::isEnabled()`） |
| 相机 | `Level::GetMainCameraComponent` | 停用对象上的相机不再当主相机 |

**没覆盖**：物理（Rigidbody 仍参与模拟）、已在播放的音频（停 Tick 不会停 voice）。
这两个要接得改各自的注册/解注册，不在这一轮。

### 入口收敛：别再直接写 Transform 的 flag

| 位置 | 原来 | 现在 |
| --- | --- | --- |
| Timeline activation clip | `transform->SetEnabled(...)` | `target->SetActive(...)` |
| Inspector 激活勾选框 | `xf->SetEnabled(active)` | `go->SetActive(active)` |
| 撤销恢复激活状态 | `transform->SetEnabled(state.enabled)` | `owner->SetActive(state.enabled)` |

三处都只写 flag、不通知组件，于是父级开关一变子树就留在旧状态上。

这条不变量有自检钉着：

```bash
python3 scripts/check_active_api.py              # 检查
python3 scripts/check_active_api.py --selftest   # 负向验证：确认检查不是摆设
```

`--selftest` 会把上面三处的旧写法注入一份临时副本，确认检查**会**失败 —— 一个正则
写错或路径写错的检查会一直"通过"，比没有检查更糟。

## 5. 两套类型表是独立的

加一个新类型到脚本签名里要改**两处**，只改一处会在各自阶段报错：

| 表 | 位置 | 管什么 | 漏了会怎样 |
| --- | --- | --- | --- |
| `ZScript::Converter<T>` | `Runtime/Scripting/Binding/ScriptBinding.h` | 运行时 C++ ↔ JS 的值转换 | 绑定代码编译不过（模板找不到特化） |
| `_TS_TYPES` | `engine/tools/ZHeaderTool/zht/parser.py` | .d.ts 的类型拼写 | generate 阶段报 `cannot be mapped to a TypeScript type` |

`std::filesystem::path` 的 `ToJs` 走 `generic_string()`：否则同一个资源在 Linux 上是
`a/b.prefab`、Windows 上是 `a\b.prefab`，任何比较或拼接路径的脚本都会只在其中一个
平台上静默失效。

## 6. 量化

```bash
python3 scripts/quantify_script_binding.py                # 全量
python3 scripts/quantify_script_binding.py --class GameObject
```

直接跑 `ZHeaderTool --dump-ir` 拿当前状态，不需要 cmake / 编译。

为什么需要它：`bin/Debug/Intermediate/Typings/zengine.d.ts` 和
`build/Generated/ZHT/**/*.gen.cpp` 都只是构建产物，没有 cmake 的机器上重新生成不了，
于是它们会停在某一时刻。照着一份过期的 d.ts 判断"某个 API 有没有导出"会得出完全
错误的结论 —— 我就据此下过一次错误判断：以为 `GameObject` 没有 `AddComponent`，
实际它有，只是产物是 7 天前的。

当前：`类 67 | 方法 205 | 静态方法 4 | 缺 Converter 的类型 0 种`。
