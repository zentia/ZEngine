# 脚本侧输入 API（Input）

## 1. 之前脚本一个输入 API 都没有

两层原因，缺哪一层的理解都会得出错误的修法：

* `InputSystem` 是 `IEngineSystem`，不是 `ZCLASS` ⇒ 脚本看不到它；
* 真正的状态在 `EnhancedInputSystem` 里，而它是 `InputSystem::Initialize()` 里
  `new` 出来的，**没有 `REGISTER_SYSTEM`** ⇒ `GET_SYSTEM(EnhancedInputSystem)`
  永远是 `nullptr`。

所以脚本只能走 `GET_SYSTEM(InputSystem)->GetEnhancedInput()`。这一点写在
`ScriptInput.h::GetInput()` 的注释里，别改成直接查 EnhancedInputSystem。

## 2. 导出的 API（17 个）

```
GetKey(key: number): boolean              键现在按着（持续）
GetKeyDown(key: number): boolean          上一帧刚按下（边沿）
GetKeyUp(key: number): boolean            上一帧刚抬起（边沿）

GetMouseButton(button): boolean           0=左 1=右 2=中
GetMouseButtonDown(button): boolean
GetMouseButtonUp(button): boolean

GetMousePosition(): Vector2               光标位置（像素）
GetMouseDelta(): Vector2                  上一帧的光标位移
GetMouseScrollDelta(): Vector2            上一帧的滚轮量

IsActionActive(name: string): boolean     动作是否被触发
GetActionAxis1D(name: string): number
GetActionAxis2D(name: string): Vector2    "Move" 的 y 前后、x 左右

GetTouchCount(): number
GetTouchIdAt(index: number): number       按 id 升序；越界 -1
GetTouchPosition(pointer_id): Vector2
GetTouchDelta(pointer_id): Vector2        相对按下点（虚拟摇杆）
GetTouchFrameDelta(pointer_id): Vector2   本帧位移（转视角）
```

引擎默认建好的动作名：`Move` / `Look` / `Jump` / `Sprint` / `Crouch` / `Fire` /
`ToggleFreeCamera`（`GameActions`，在 `InputSystem::Initialize()` 里绑到 WASD /
Space / Shift / Ctrl / F 与鼠标移动）。

键码用 `ZENUM` 导出的 `KeyCodes::Key`：

```typescript
if (Input.GetKeyDown(Key.Space)) { jump(); }
const move = Input.GetActionAxis2D("Move");
```

`ZENUM` 支持"恰好一个具名 namespace"，`KeyCodes` 正好是一个，所以不需要为了导
出而把几百个键码复制一份。脚本里的名字是 `Key`（namespace 名不进脚本），不是
Unity 的 `KeyCode` —— `ZENUM` 目前不接受 `Name=` specifier。

## 3. 帧生命周期：为什么是"上一帧"，以及为什么必须加 EndFrame

这是本次改动里唯一真正绕的地方，写错了就是"API 存在但永远是默认值"。

一帧的顺序（`Application::TickOneFrame`）：

```
LogicalTick
  ├─ InputSystem::Tick → EnhancedInputSystem::Tick()   （累积、求值、触发）
  └─ WorldManager::Tick → 组件 Tick → 脚本 OnUpdate     ← 脚本在这里读输入
RendererTick
InputSystem::EndFrame()      ← 本次新增：帧内量在这里滚动成读数
WindowSystem::PollEvents()   ← 宿主在这一步投递键鼠事件
```

两个约束推导出唯一正确的设计：

1. 脚本读输入发生在 `EnhancedInputSystem::Tick()` **之后** ⇒ 清理不能放在 Tick
   末尾。放在那里的话 `GetKeyDown` 恒为 false、`GetMouseDelta` 恒为 0 —— API
   存在但永远是默认值，而且编译期、静态检查都发现不了。
2. 事件在 `PollEvents()`（帧末尾）才到达 ⇒ 清理也不能放在下一帧 Tick 的开头，
   否则上一帧末尾到达的事件还没被读到就被清掉。

所以帧内量（按键边沿、光标位移、滚轮、触摸帧位移）统一由
`EnhancedInputSystem::EndFrame()` 在**脚本 tick 之后、PollEvents 之前**滚动：

```
收集器（本帧累积） --EndFrame--> 读数（脚本读到）  ，然后清空收集器
```

于是每个查询拿到的是"上一帧滚出来的读数"——比实时晚一帧，这是事件投递时机
决定的，不是延迟 bug。

`EndFrame()` 由 `Application::TickOneFrame` 每帧无条件调用（**不受**
`should_update_logic` 影响）：暂停时也要滚，否则暂停期间攒下的边沿会在恢复那
一帧一次性全部触发。

## 4. 三态与 REPEAT

`GetKey` / `GetKeyDown` / `GetKeyUp` 必须分开，否则"按一次空格跳一下"会变成
"按住空格每帧都跳"。

`REPEAT`（长按的自动重复）**不算**一次新的按下：它只维持 held，不进边沿集合。
否则 `GetKeyDown` 会退化成 `GetKey`。

鼠标键与键盘共用同一套状态（`OnMouseButton` 把 button 编成
`MouseButton1 + button` 塞进 `m_PressedKeys`），所以 `GetMouseButton(0)` 与
`GetKey(0)` 查的是同一个位 —— 这不是巧合，别在脚本里混用。

## 5. 触摸：为什么要 `GetTouchIdAt`

`m_Touches` 是 `unordered_map<pointer_id, TouchPoint>`，迭代顺序不确定。直接
"取第 0 个元素"会在帧之间跳。所以遍历手指的正确写法是先要一个稳定的 id 列表：

```typescript
const n = Input.GetTouchCount();
for (let i = 0; i < n; ++i) {
    const id = Input.GetTouchIdAt(i);
    const p  = Input.GetTouchPosition(id);
}
```

`GetTouchIdAt` 按 pointer_id 升序返回（越界 -1）。

## 6. 没有输入系统时

`InputSystem` 缺失、或它的 `EnhancedInput` 还没建起来（未 `Initialize`、
headless 冒烟测试、编辑模式还没进 Play）时，`Input.*` 返回**安全默认值**：
`false` / `0` / 零向量 / `GetTouchIdAt` 的 `-1`。不崩，脚本拿到的就是"没有输入"。

## 7. 冒烟测试

`engine/Source/Runtime/Scripting/ScriptInput/Test/ScriptInputSmokeTest.cpp`，
45 项检查。它**直接构造一个 `EnhancedInputSystem`**（不 `Initialize()`，否则会
向 WindowSystem 注册 GLFW 回调，headless 跑不了），手工投递
`OnKey` / `OnMouseButton` / `OnScroll` / `OnTouch`，然后 `Tick()` + `EndFrame()`
推进帧，断言查询结果。

重点钉住两条：边沿必须在 `EndFrame` 之后才可读、且只活一帧（K2 / K3）；
REPEAT 不算新按下（K4）。以及脚本 API 在没有输入系统时的降级（D1）。

```bash
# 有 cmake
cmake -B build -DZENGINE_BUILD_SCRIPT_INPUT_SMOKE_TEST=ON
cmake --build build --target ScriptInputSmokeTest
ctest --test-dir build -R ScriptInputSmokeTest

# 没有 cmake（EnhancedInputSystem.cpp 改过，旧的 libZRuntime.a 里没有新符号）
python3 scripts/run_smoke_test.py \
    engine/Source/Runtime/Scripting/ScriptInput/Test/ScriptInputSmokeTest.cpp \
    --extra-source engine/Source/Runtime/Function/Input/EnhancedInputSystem.cpp
```

负向验证：让 REPEAT 也算一次按下后 K4c 失败（`45 checks, 1 failed`），已还原。

## 8. 刻意没做的

| Unity API | 为什么不做 |
| --- | --- |
| `Input.GetAxis("Horizontal")` | 引擎没有 Unity 那套具名输入轴表（InputManager）。等价物是 action 查询：`Input.GetActionAxis1D("Move")` |
| `Input.GetJoystickNames` / 手柄 | `EnhancedInputSystem` 没有手柄/游戏杆状态，加了就是恒为空的假 API |
| `Input.inputString`（本帧输入的文本） | 引擎没有 IME / 文本合成的收集点（`MacOSWindow.mm` 只转发按键），做出来是空的 |
| `Input.GetTouch(i)` 返回完整 Touch 结构 | 需要给 `Touch` 造一个 Object 派生类才能进绑定表；拆成 4 个按 id 查询的函数够用，且不需要新类型 |

量化（`ZHeaderTool generate --dump-ir`）：类 71 → 72，方法 276 → 293，静态方法
63 → 80，缺 Converter 0。
