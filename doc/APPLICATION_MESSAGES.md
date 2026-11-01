# Unity 的应用层消息（`OnApplication*` / `OnTransformParentChanged` / `OnTransformChildrenChanged` / `OnValidate`）

`engine/Source/Runtime/Function/Framework/Component/Component.h`（`AppEventKind` + 五个虚函数）
`engine/Source/Runtime/BaseClasses/GameObject.{h,cpp}`（一趟广播）
`engine/Source/Runtime/Function/Framework/Level/Level.{h,cpp}`
`engine/Source/Runtime/Function/Framework/World/WorldManager.{h,cpp}`
`engine/Source/Runtime/Application/Application.{h,cpp}`（焦点入口 + 退出）
`engine/Source/Runtime/Function/Render/WindowSystem.cpp`（焦点回调的注册点）
`engine/Source/Runtime/Function/Framework/Component/Transform/Transform.cpp`（改父级，**两个**回调）
`engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.{h,cpp}`
`engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.{h,cpp}`（`InvokeInstanceMethodBool`）
`engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp`（`.d.ts`）

对照 Unity：`MonoBehaviour.OnApplicationFocus(bool)` / `OnApplicationPause(bool)` /
`OnApplicationQuit()` / `OnTransformParentChanged()` / `OnTransformChildrenChanged()` /
`OnValidate()`。

---

## 1. 与三趟 Tick 方向相反

| | Update / FixedUpdate / LateUpdate | 这五个消息 |
|---|---|---|
| 驱动者 | 帧（每帧 / 每步） | 整个应用发生了一件事 |
| 判据 | `activeInHierarchy && isEnabled &&` 编辑器白名单 | **什么都不判** |
| 一帧次数 | 0..n | 最多一次（且不由帧率驱动） |

不判启用是有意的，而且与 Unity 一致（Unity 的 `OnApplicationPause` 文档写的
就是 "sent to all GameObjects"）。理由：

* "应用要退出了"跟某个对象激不激活无关 —— 被停用的对象同样要收尾（存档、断连、
  写日志）。按激活态过滤掉它，得到的是"偶尔存不上档"这种没法复现的 bug。
* 它们一帧最多一次，不存在 Tick 那套"两套节奏错位"的问题。

⚠️ 这条语义由 **AM9 / AM11（行为）+ AW8 / AW9（函数体里不许出现那两行判据）**
双重守住。只留行为断言的话，谁把判据加回去，测试仍然绿（因为探针默认启用）。
`OnTransformChildrenChanged` 那一侧由 **CH30–CH34** 补上同样的两条。

## 2. 五个消息合成一个 `AppEventKind`

`Component` 上五个虚函数，但 `GameObject` 只有**一趟**遍历
（`BroadcastAppEvent`，按 kind switch）。拆成五个方法就等于把"遍历全部组件、
不判启用"这条语义抄五遍，抄漏一遍就是"某一条消息莫名其妙收不到"。

```
Application::NotifyApplicationFocus / Quit()    Transform::SetParent
        |                                              |
   WorldManager::BroadcastAppEvent          GameObject::BroadcastAppEvent
        |                                              |
   Level::BroadcastAppEvent  (两张表：m_Gobjects + m_RuntimeGObjects)
        |
   GameObject::BroadcastAppEvent  (全部组件，不判启用)
        |
   Component::OnApplicationXxx  ->  TypeScriptComponent -> JS
```

## 3. Pause 不是 Focus 的别名

```cpp
static bool DeriveApplicationPaused(bool has_focus, bool run_in_background)
{
    return !has_focus && !run_in_background;
}
```

* Focus 是"键盘给谁了" —— 失焦就一定发 `false`；
* Pause 是"还跑不跑" —— 只有 `runInBackground == false`（`Application::Run`
  那条分支会真的停掉世界）时才算暂停。

桌面上两者分叉：`runInBackground=true` 时窗口失焦，世界照跑，于是只有
`OnApplicationFocus(false)`，没有 `OnApplicationPause(true)`。移动端才同时发生。

这个函数是**纯**的、且**不去重**（没有"上一次的值"）：每次焦点变化都派发一条
`OnApplicationPause(推导值)`。用 `m_LastAppPaused` 去重能少发一次，但那是一块
谁也看不见的状态 —— 而"失焦但 runInBackground=true"时它会把一条真值（false）
吞掉。纯函数还顺带解决了可测性：单元测试里没有已加载的世界，派发结果观测不到，
只有这个纯函数能被断言（AM21–AM24）。

## 4. 触发点

| 消息 | 触发点 | 为什么在那里 |
|---|---|---|
| `OnApplicationFocus` / `Pause` | `WindowSystem::Initialize` 注册 `RegisterOnWindowFocusFunc` | Application 是 `Core` 阶段，**早于** WindowSystem。在 `Application::Initialize` 里注册拿到的是 nullptr，注册就静默丢了 |
| `OnApplicationQuit` | `Application::Quit()`，在 `RequestClose()` **之前** | 窗口系统一开始关，WorldManager 可能已随系统反初始化被摘掉，那时派发就是静默丢失（AW20 是这条顺序的门） |
| `OnTransformParentChanged` | `Transform::SetParent` 末尾 | 消息到达时层级必须已经是新的了。脚本在这个回调里读 `transform.parent` 是常规用法，早一步派发它读到的就是旧父级 |
| `OnTransformChildrenChanged` | 同上，**再往下两处** | 同一件事的另一半，见下节 |

转发统一走 `Application::NotifyApplicationFocus(bool)` 这个公开入口而不是各
回调内部直接广播 —— 它有三个调用方：WindowSystem、编辑器、测试。只留内部回调
的话整条链只能靠真的去点一下窗口来验证，而 CI 里没有窗口可点。

## 5. `OnTransformParentChanged` 与 `OnTransformChildrenChanged` 是一对

"把 C 挂到 A 下面"这件事有两个观察者，而且**不是同一个对象**：

| 回调 | 发给谁 | 语义 |
|---|---|---|
| `OnTransformParentChanged` | **被搬的 C** | "我的 `transform.parent` 变了" |
| `OnTransformChildrenChanged` | **旧父级和新父级**（A 与 B） | "我的孩子**个数**变了" |

```cpp
// Transform::SetParent 末尾
if (GameObject* const owner = GetParentObject())
{
    owner->BroadcastAppEvent(AppEventKind::kParentChanged, false);   // 给 C
}
if (old_father != nullptr)
{
    if (GameObject* const old_owner = old_father->GetParentObject())
    {
        old_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);
    }
}
if (new_parent != nullptr)
{
    if (GameObject* const new_owner = new_parent->GetParentObject())
    {
        new_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);
    }
}
```

三个容易写错的地方，各自都有门：

1. **只发一边。** 少了旧父级那一处，"挂上来"有人知道、"摘下去"没人知道。两处
   文本完全一样，所以源码门写的是 `>= 2`（CW9）。
2. **发给被搬的对象自己。** 两个回调名字只差一个词，而且被搬的对象上**已经有**
   一个同名相近的回调了，看起来非常合理。CH9 / CH16 / CH39 / CH42 是这条的判据。
3. **判空。** 脱离父级（`SetParent(nullptr)`）时 `new_parent` 就是 nullptr；
   从场景根挂上来时 `old_father` 是 nullptr。都不是防御性编程（CW12 / CW13）。

### 只认成员增减，不认重排

`SetSiblingIndex` **不**发这条消息。Unity 文档的说法是 "the list of children
has changed"，读成**成员增减**；若读成顺序变化，编辑器里拖一次排序就会给脚本
灌进几十次回调。这条由 CH29（行为）+ CW15（函数体反向扫描）守住。

⚠️ CH29 断言的是 `0`，而"从来不发"也是 `0` —— 所以紧跟着有 CH28（顺序真的变了）
和 CH28 之前的 CH23–CH25（B 真的有两个孩子）做反空转，再由互补的 CH28
（真的摘走一个，必须发）补上另一半。

### 搬整棵子树时，被搬的那个自己"孩子没变"

把 C（带着它的孩子 D）从 B 搬到 A：C 的孩子列表没动，所以 **C 不收**
`OnTransformChildrenChanged`，只收 `OnTransformParentChanged`（CH39 / CH40）。
同理，祖父母 A 那一层也收不到 —— 只有**直接父级**的孩子个数变了（CH41 / CH43）。

### `DetachChildren` 走的是 `SetParent`

`Transform::DetachChildren()` 内部就是逐个 `SetParent(nullptr, true)`，所以每摘
一个孩子都会发一次（CH46）。它没有自己的一份"直接改列表"的快路径 —— 有就会漏。

## 6. `OnValidate` 走的是已有的 `OnSerializedFieldsUpdated`

`Component::OnSerializedFieldsUpdated()` 的全部调用方都在 Editor 模块
（Inspector 编辑、撤销、场景放置、Prefab 应用），语义就是 Unity 的 `OnValidate`。
`TypeScriptComponent` 覆写它并额外判 `g_isEditor` —— Player 里这条路径唯一的
调用方是反序列化，那时脚本实例还没绑上，判不判都一样；显式判一次是为了把
"编辑器专用"这条语义留在代码里，而不是留在"恰好没有别的调用方"上。

P8 / P9 是互补的一对：Player 态**不发**，编辑态**发**。只测"发了"的话，
"忘了判 g_isEditor"这种实现是绿的。

## 7. bool 参数必须是真 bool

`InvokeInstanceMethodBool` 与 Number 版只差 `pesapi_create_boolean` 一行，但
不能复用：把 bool 塞成 `1.0 / 0.0`，`if (hasFocus)` 仍然对，而
`hasFocus === true` 是假 —— 一个只在严格比较时才炸的坑。

`Mover.ts` 里那两个 `=== true` 就是冲这个写的（P1 / P2 / P3 因此能抓住它）。

## 8. 测试

| 层 | 在哪 | 钉什么 |
|---|---|---|
| 行为（C++ 组件） | `UnityParitySmokeTest` AM0–AM24 | 四种消息都到、每个组件都到（不只是第一个）、**停用 / 失活照样收得到**、改父级只发宿主自己、同一父级不重发、pause 推导的四种组合 |
| 行为（C++ 组件） | `UnityParitySmokeTest` **CH0–CH46** | 父级收 / 子级**不**收、新旧两个父级各一次、脱离只有旧父级收、**重排不发**、停用与失活照样收、只发给直接父级（不冒泡到祖父）、搬子树时被搬的那个自己不收、`DetachChildren` 逐条发 |
| 行为（脚本） | `ScriptComponentSmokeTest` P0–P9、**P10–P18** | TS 侧真的收得到；两个方向都通；bool 是真 bool；`OnValidate` 的编辑器门；**父级才收**、`childCount` 在回调里已是新值、搬自己不给自己发 |
| 接线 | `UnityParitySmokeTest` AW0–AW39 | 枚举 + 四个虚函数、GameObject/Level/WorldManager 三跳、**函数体里没有 Tick 判据**、WorldManager 不早退、三个触发点、quit 派发早于 RequestClose、Bool 版 invoke、`.d.ts` 五个声明、JS 原型五个兜底 |
| 接线 | `UnityParitySmokeTest` **CW0–CW22** | 五个 kind 都在 switch 里、两个回调**都**声明（不是"至少一个"）、SetParent 里**两处**派发且都判空、派发在 `SetParent` 函数**体内**（不是没人调的 helper）、`SetSiblingIndex` 里**不许**出现它、`.d.ts` 与 JS 原型兜底都在 |

## 8.1 接线门自己的一个坑：切函数体不能写死行尾

上面那批"**函数体里**应该有 / 不该有什么"的门，都要先切出函数体：

```cpp
const std::string body = SliceFunctionBody(source, source.find("bool Transform::SetParent"));
```

而切函数体靠的是"找到下一个同样缩进的 `}`"。这里有两个独立的坑，都实测踩到过：

1. **行尾。** `ReadFile` 是 `std::ios::binary` 打开的，CRLF 原样进来；仓库里两种
   行尾同时存在（`Component.h` 是 CRLF、`GameObject.cpp` 是 LF），`git checkout`
   还会按 `core.autocrlf` 把文件翻一遍。CRLF 下 `"\n}\n"` 一律落空，`substr`
   退化成"从这里到文件末尾"，于是：
   * 正向门（"这段里应该有 X"）→ 整个文件里当然有 X → **永真**；
   * 反向门（"这段里不该有 X"）→ 永远找得到 X → **永假**。

   实测：`Transform.cpp` 被翻成 CRLF 之后 CW11 变永真、CW15 变永假，1985 条里
   只有那一条在动。修法是在 `ReadFile` 里统一把 `\r` 折掉 —— **读到的内容不该
   取决于 git 怎么捡出这个文件**。

2. **缩进。** Runtime 那批文件函数顶格（`void Transform::SetParent` 在第 0 列），
   而 `UnityParitySmokeTest.cpp` 自己的函数在匿名 namespace 里、缩进 4 格。写死
   `"\n}\n"` 的话后者切出来的是"半个文件"。所以 `FunctionEndMarker` 按**签名行
   的前导空白**推导，两种风格都对。

两层门：

* `BodyIsBounded(...)` —— 证明"切出来的真的是一个函数"（CW5b / CW10b / CW14b /
  AW7b / AW12b / CW21c）；
* CW22 —— 数 `ReadFile` **函数体内**有没有 `NormalizeNewlines`。范围必须限定在
  函数体内：数整个文件的话，会被 CW22 自己的字面量命中（自引用），变异体把真正
  的调用删掉之后计数仍是 1，门恒真。

`TCM18`（把归一化那一步拿掉）是这条链的变异体。删掉归一化之后 CW10b / CW14b /
CW15 / CW22 一起红。

## 9. 还没做

* `OnBecameVisible` / `OnBecameInvisible` —— 需要渲染侧的可见性判定（视锥剔除
  结果回传），不是一条消息转发能解决的。
* `OnDrawGizmos` / `OnDrawGizmosSelected` —— Unity 里由 Scene 视图每帧驱动，
  这里没有对应的"编辑器视图帧"，得先定它什么时候算一帧。
* `OnGUI` / `OnRenderObject` / `OnPostRender` —— 需要 IMGUI / 渲染回调锚点。
* `OnApplicationFocus` 在移动端（没有引擎窗口的一支）不会来：那一支
  `WindowSystem::Initialize` 在 `CreatePlatformApplication()` 返回空时提前
  return，压根注册不了回调。要接得由宿主自己调
  `Application::NotifyApplicationFocus`。
