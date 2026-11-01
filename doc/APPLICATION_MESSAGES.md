# Unity 的应用层消息（`OnApplication*` / `OnTransformParentChanged` / `OnValidate`）

`engine/Source/Runtime/Function/Framework/Component/Component.h`（`AppEventKind` + 四个虚函数）
`engine/Source/Runtime/BaseClasses/GameObject.{h,cpp}`（一趟广播）
`engine/Source/Runtime/Function/Framework/Level/Level.{h,cpp}`
`engine/Source/Runtime/Function/Framework/World/WorldManager.{h,cpp}`
`engine/Source/Runtime/Application/Application.{h,cpp}`（焦点入口 + 退出）
`engine/Source/Runtime/Function/Render/WindowSystem.cpp`（焦点回调的注册点）
`engine/Source/Runtime/Function/Framework/Component/Transform/Transform.cpp`（改父级）
`engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.{h,cpp}`
`engine/Source/Runtime/Scripting/Wrapper/ScriptEnv.{h,cpp}`（`InvokeInstanceMethodBool`）
`engine/Source/Runtime/Scripting/Binding/ScriptBinding.cpp`（`.d.ts`）

对照 Unity：`MonoBehaviour.OnApplicationFocus(bool)` / `OnApplicationPause(bool)` /
`OnApplicationQuit()` / `OnTransformParentChanged()` / `OnValidate()`。

---

## 1. 与三趟 Tick 方向相反

| | Update / FixedUpdate / LateUpdate | 这四个消息 |
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

## 2. 四个消息合成一个 `AppEventKind`

`Component` 上四个虚函数，但 `GameObject` 只有**一趟**遍历
（`BroadcastAppEvent`，按 kind switch）。拆成四个方法就等于把"遍历全部组件、
不判启用"这条语义抄四遍，抄漏一遍就是"某一条消息莫名其妙收不到"。

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

## 4. 三个触发点

| 消息 | 触发点 | 为什么在那里 |
|---|---|---|
| `OnApplicationFocus` / `Pause` | `WindowSystem::Initialize` 注册 `RegisterOnWindowFocusFunc` | Application 是 `Core` 阶段，**早于** WindowSystem。在 `Application::Initialize` 里注册拿到的是 nullptr，注册就静默丢了 |
| `OnApplicationQuit` | `Application::Quit()`，在 `RequestClose()` **之前** | 窗口系统一开始关，WorldManager 可能已随系统反初始化被摘掉，那时派发就是静默丢失（AW20 是这条顺序的门） |
| `OnTransformParentChanged` | `Transform::SetParent` 末尾 | 消息到达时层级必须已经是新的了。脚本在这个回调里读 `transform.parent` 是常规用法，早一步派发它读到的就是旧父级 |

转发统一走 `Application::NotifyApplicationFocus(bool)` 这个公开入口而不是各
回调内部直接广播 —— 它有三个调用方：WindowSystem、编辑器、测试。只留内部回调
的话整条链只能靠真的去点一下窗口来验证，而 CI 里没有窗口可点。

## 5. `OnValidate` 走的是已有的 `OnSerializedFieldsUpdated`

`Component::OnSerializedFieldsUpdated()` 的全部调用方都在 Editor 模块
（Inspector 编辑、撤销、场景放置、Prefab 应用），语义就是 Unity 的 `OnValidate`。
`TypeScriptComponent` 覆写它并额外判 `g_isEditor` —— Player 里这条路径唯一的
调用方是反序列化，那时脚本实例还没绑上，判不判都一样；显式判一次是为了把
"编辑器专用"这条语义留在代码里，而不是留在"恰好没有别的调用方"上。

P8 / P9 是互补的一对：Player 态**不发**，编辑态**发**。只测"发了"的话，
"忘了判 g_isEditor"这种实现是绿的。

## 6. bool 参数必须是真 bool

`InvokeInstanceMethodBool` 与 Number 版只差 `pesapi_create_boolean` 一行，但
不能复用：把 bool 塞成 `1.0 / 0.0`，`if (hasFocus)` 仍然对，而
`hasFocus === true` 是假 —— 一个只在严格比较时才炸的坑。

`Mover.ts` 里那两个 `=== true` 就是冲这个写的（P1 / P2 / P3 因此能抓住它）。

## 7. 测试

| 层 | 在哪 | 钉什么 |
|---|---|---|
| 行为（C++ 组件） | `UnityParitySmokeTest` AM0–AM24 | 四种消息都到、每个组件都到（不只是第一个）、**停用 / 失活照样收得到**、改父级只发宿主自己、同一父级不重发、pause 推导的四种组合 |
| 行为（脚本） | `ScriptComponentSmokeTest` P0–P9 | TS 侧真的收得到；两个方向都通；bool 是真 bool；`OnValidate` 的编辑器门 |
| 接线 | `UnityParitySmokeTest` AW0–AW39 | 枚举 + 四个虚函数、GameObject/Level/WorldManager 三跳、**函数体里没有 Tick 判据**、WorldManager 不早退、三个触发点、quit 派发早于 RequestClose、Bool 版 invoke、`.d.ts` 五个声明、JS 原型五个兜底 |

## 8. 还没做

* `OnBecameVisible` / `OnBecameInvisible` —— 需要渲染侧的可见性判定（视锥剔除
  结果回传），不是一条消息转发能解决的。
* `OnDrawGizmos` / `OnDrawGizmosSelected` —— Unity 里由 Scene 视图每帧驱动，
  这里没有对应的"编辑器视图帧"，得先定它什么时候算一帧。
* `OnGUI` / `OnRenderObject` / `OnPostRender` —— 需要 IMGUI / 渲染回调锚点。
* `OnApplicationFocus` 在移动端（没有引擎窗口的一支）不会来：那一支
  `WindowSystem::Initialize` 在 `CreatePlatformApplication()` 返回空时提前
  return，压根注册不了回调。要接得由宿主自己调
  `Application::NotifyApplicationFocus`。
