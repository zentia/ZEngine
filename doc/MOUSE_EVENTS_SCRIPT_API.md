# 鼠标消息 —— `OnMouseEnter` / `OnMouseOver` / `OnMouseExit` / `OnMouseDown` / `OnMouseDrag` / `OnMouseUp` / `OnMouseUpAsButton`

```ts
class Button3D extends Behaviour {
    hovered: boolean = false;

    OnMouseEnter(): void { this.hovered = true; }
    OnMouseExit(): void { this.hovered = false; }
    OnMouseDown(): void { Debug.Log("pressed"); }
    OnMouseDrag(): void { /* 按住的每一帧 */ }
    OnMouseUp(): void { Debug.Log("released"); }
    OnMouseUpAsButton(): void { Debug.Log("clicked"); }
}
```

七个都是 `Behaviour` 上的**空默认实现**，脚本里定义了才会被调用；没定义就是
no-op（和 Unity 一样，不需要 `override` 关键字，也没有"必须实现"的接口）。

| TypeScript | C++ 钩子 | 何时来 |
|---|---|---|
| `OnMouseEnter()` | `Component::OnMouseEnter` | 鼠标**进入**该对象的那一帧，一次 |
| `OnMouseOver()` | `Component::OnMouseOver` | 鼠标**停在该对象上**的每一帧 |
| `OnMouseExit()` | `Component::OnMouseExit` | 鼠标**离开**该对象的那一帧，一次 |
| `OnMouseDown()` | `Component::OnMouseDown` | 左键在该对象上按下的那一帧，一次 |
| `OnMouseDrag()` | `Component::OnMouseDrag` | 按住期间**每一帧**（含按下那一帧） |
| `OnMouseUp()` | `Component::OnMouseUp` | 左键松开的那一帧，一次 |
| `OnMouseUpAsButton()` | `Component::OnMouseUpAsButton` | 松开时鼠标**仍在按下时那个对象上**，一次 |

派发链：

```
Application::LogicalTick
  └─ InputSystem::Tick()
  └─ MouseEventDispatcher::Tick(state)          ← 每帧一次
        ├─ PickAt(pos) = Camera.main → ScreenPointToRay → Physics.Raycast(-1)
        └─ DispatchTo(state, under_mouse)
              ├─ Exit  → 旧 s_Hovered       （只在换对象那一帧）
              ├─ Enter → 新 under_mouse     （只在换对象那一帧）
              ├─ Over  → s_Hovered          （每帧）
              ├─ Down  → s_Hovered          （pressed 边沿，记 s_Pressed）
              ├─ Drag  → s_Pressed          （held，每帧）
              ├─ Up    → s_Pressed          （released 边沿）
              └─ UpAsButton → s_Pressed     （仅当 s_Pressed == s_Hovered）
  └─ GameObject::BroadcastMouseEvent(kind)
        └─ 判 activeInHierarchy（**不判 enabled**）→ 每个组件 → 脚本
```

## 一、三条判据，各不一样

这是整套里最容易写错、也最难发现的部分。同一个 `GameObject` 上现在有三类
派发，判据**互不相同**：

| 派发 | `activeInHierarchy` | `enabled` | 编辑器白名单 |
|---|---|---|---|
| `Tick` / `FixedTick` / `LateTick` | ✅ | ✅ | ✅ |
| `BroadcastAppEvent`（`OnApplicationFocus` 等） | ❌ | ❌ | ❌ |
| `BroadcastDrawGizmos` | ✅ | **❌** | ❌ |
| `BroadcastMouseEvent` | ✅ | **❌** | ❌ |

`enabled` 那两格是 Unity 的明文语义，MonoBehaviour 参考页原话：

> "The checkbox for disabling a MonoBehavior (on the editor) will only prevent
> Start(), Awake(), Update(), FixedUpdate(), and OnGUI() from executing."

所以**停用的脚本照样收 `OnMouse*`**（也照样画 gizmo）。把它写成"停用就什么都不
收"，表现是"点了没反应"，而日志里干干净净 —— 正是那种能拖一周的 bug。

⚠️ 反向门 DW6 / MW6 是**扫描源码**的门：它们在 `GameObject.cpp` 的实现体里找
`isEnabled` 这个字符串，找到了就失败。所以这些注释里**不能出现** `isEnabled`
这个词的完整拼写，否则门自己被自己的注释打翻。

## 二、Push 与 Pull

Tick 是**拉**的（主循环主动问每个对象）；鼠标消息是**推**的（每帧一次，由
`MouseEventDispatcher` 自己决定发给谁）。

这条区分的后果：`OnMouseOver` 在一帧里最多来一次，而"鼠标停在原地 60 帧"就是
60 次 `OnMouseOver` —— 它**不是** `OnMouseEnter` 的重复，是逐帧的。同理
`OnMouseDrag` 在按下那一帧也会来（Unity 的 `OnMouseDrag` 是"按住期间的每一
帧"，不是"按下之后移动了才来"）。

## 三、`OnMouseDrag` / `OnMouseUp` 发给**按下时**那个对象

这是 Unity 的语义，也是这套设计里唯一的"状态"：

* 按住 A，把鼠标拖到 B 上松开 → `OnMouseDown` 给 **A**，`OnMouseDrag` 给 **A**（每
  帧），`OnMouseUp` 给 **A**，`OnMouseUpAsButton` **不来**。
* 按住 A 再松开（鼠标没动）→ 上面四条全给 A，`OnMouseUpAsButton` 也给 A。

写反成"发给当前悬停的那个"，拖动逻辑会在鼠标移出物体边界的那一帧突然断掉，
而"点按钮"在慢速点击时照样正常 —— 只有拖动测试能看见。

`OnMouseUpAsButton` 与 `OnMouseUp` 唯一的差别就是那个 `s_Pressed == s_Hovered`
判据。少了它就是"拖出去松手也算点击"，正是按钮语义要防的那件事。

## 四、`PickAt` 的三条硬约束

1. **没有主相机就返回 `nullptr`**。专用服务器、headless、渲染还没起来都走这条。
   没有相机就没有"鼠标下面"这回事，不是错误。
2. **射线要拷出来**。`Camera::ScreenPointToRay` 返回的是**按相机复用的**同一个
   `Ray` 对象（见 `ScriptCamera.h` 的注释）。存指针的话，下一次 `PickAt` 读到的
   是别人的射线 —— 而两台相机交替使用时它还"看起来是对的"。
3. **层掩码传 `-1`（所有层）**。`OnMouse*` 回答的是"鼠标点到了什么"，不是"某个
   查询能打到什么"。要过滤的话脚本自己在回调里判 `layer` 更清楚。

## 五、`s_Hovered` / `s_Pressed` 必须是文件级静态

写成函数内的 `static` 局部变量，`Tick` 和 `Reset` 会各自持有一份互不相干的
状态，清掉的那份不是派发用的那份 —— 编译能过、运行不崩，只是"切关卡之后
`OnMouseExit` 打到了野指针"。

配套的两个出口：

* `MouseEventDispatcher::Forget(GameObject*)` —— 由 `~GameObject` 调用。少了它，
  销毁一个正被悬停的对象之后第一次 `Tick` 就是一次 use-after-free，而且是
  **概率性**的（只在鼠标恰好停在它上面时）。
* `MouseEventDispatcher::Reset()` —— 关卡切换 / 世界卸载时清状态。⚠️ 它**不**补发
  `OnMouseExit`：要清的是状态，不是"通知旧对象鼠标走了"，那个对象通常已经销毁
  了。补发就是把一次 use-after-free 包装成一次"看起来很贴心"的通知。

## 六、没做的 Unity 成员

| Unity | 状态 |
|---|---|
| `MonoBehaviour.OnMouseOver` 的"每帧多次" | 引擎一帧一次，与渲染帧一致 |
| 右键 / 中键的 `OnMouse*` | Unity 只有左键；多键请用 `Input.GetMouseButtonDown(n)` |
| `IPointerClickHandler` 等 EventSystem 接口 | 没有 uGUI 的 EventSystem；对应的替代是 UI 层自己的命中测试 |
| `Camera` 的 `eventMask` 过滤 | 见 §四.3：脚本侧自己判 `layer` |

## 七、测试分布

| 位置 | 覆盖 |
|---|---|
| `UnityParitySmokeTest` ME | 七个钩子的**判据**：进出只在换对象那帧、Over 每帧、Down 边沿、Drag/Up 发给 `s_Pressed`、UpAsButton 的同对象判据、停用照收、停用对象不收、`Forget` / `Reset` |
| `UnityParitySmokeTest` MW | 接线门（七个虚函数、正向判据、反向门、主循环里的驱动位置、`Forget` 在析构里、脚本面三处声明） |
| `ScriptComponentSmokeTest` S | **脚本**链路：七条各一个字母（共用计数器会漏掉"Up 与 UpAsButton 转发到同一个名字"）、回调里 `gameObject` 可读、停用照收、停用对象不收 |

变异用例见 `.mutate_me.py`。

### 已知覆盖缺口

`MouseEventDispatcher::PickAt` 的**命中**路径（真的从相机 Raycast 出一个对象）
没有行为级门：headless 下没有主相机，`TryPickRay` 恒返回 false。被覆盖的是
`PickAt` 返回 `nullptr` 那一半，以及 `DispatchTo(state, under_mouse)` 的全部
（测试直接喂 `under_mouse`）。补它需要一个能设主相机 + 能放碰撞体的 headless
夹具 —— 值得，但属于"物理 + 相机"那一块的工作，不在这一片里。
