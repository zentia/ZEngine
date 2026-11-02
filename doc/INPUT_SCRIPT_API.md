# `Input` 脚本门面（Unity `UnityEngine.Input` 对齐）

TS 侧：`Input.GetAxis("Horizontal")`、`Input.touches`、`Input.multiTouchEnabled`。
对位是 Unity 的 `UnityEngine.Input`（legacy Input Manager 那一层），**拼写与 Unity 一致**。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Scripting/ScriptInput/ScriptInput.h` | `Input` 门面（header-only，纯转发） |
| `Runtime/Function/Input/InputEnums.h` | `TouchPhase` / `DeviceOrientation` / `IMECompositionMode` |
| `Runtime/Function/Input/Touch.h` | `Touch` 值类型（14 字段，与 Unity 同名） |
| `Runtime/Function/Input/EnhancedInputSystem.h/.cpp` | 真正的输入后端：文本编码、触摸相位、连击 |
| `Runtime/Scripting/Binding/ScriptBinding.h` | `Converter<Touch>`（值类型跨边界） |

发布面：**43 个成员**（23 个方法 + 20 个属性），见文末清单。

⚠️ 注意"方法数"比 C++ 里的 `ZFUNCTION` 数少：静态访问器会被 `codegen.py` 从方法表
里摘掉（同一个 `MethodCallback` 编进两个 COMDAT 段 ⇒ LNK1179），所以
`Input.GetMousePosition()` 在 TS 里**不存在**，只有 `Input.mousePosition`。这恰好
就是 Unity 的形状（Unity 也没有 `Input.GetMousePosition()`）。`ScriptVMSmokeTest`
的 E4 钉的是这一条。

---

## 1. 三层的分工：`Input` 不持有任何状态

```
脚本  ──▶  Input（ScriptInput.h，纯静态，不存数据）
              │
              ├─▶ EnhancedInputSystem  （按键 / 光标 / 触摸 / 文本）
              └─▶ InputSettings 单例   （multiTouchEnabled 等开关）
```

`Input` 是 header-only 的**转发层**：每个方法只做"查系统 → 没有就返回安全默认值"。
这样做的原因是 Unity 的 `Input` 本来就是静态入口，脚本在 `Awake` 里就能读，而
ZEngine 的 `EnhancedInputSystem` 是 `IEngineSystem`，可能还没注册（编辑器、无头
进程、单测都这样）。

⚠️ **没有系统时返回的是"安全默认"，不是编造的值**：

| 量 | 无系统时的值 | 为什么是这个 |
| --- | --- | --- |
| `mousePresent` | `true`（桌面）/ `false`（触摸平台） | 由 `ZENGINE_PLATFORM_*` 决定，是平台事实 |
| `touchSupported` | `false` | 桌面 build 上永远收不到触摸事件，`true` 会把脚本带上一条跑不通的路 |
| `stylusTouchSupported` / `touchPressureSupported` | `false` | 没有任何后端上报这两项 |
| `acceleration` / `gravity` | `Vector2.ZERO` | 没有加速度计 |
| `deviceOrientation` | `Unknown`(=0) | 不是 `Portrait`：后者是"猜一个更可能的"，猜错比不明说更糟 |
| `joystickNames` | 空数组 | Unity 也是"没有手柄就是空数组"，不是 `[null]` |
| `inputString` | `""` | — |
| `imeIsSelected` / `compositionString` | `false` / `""` | 没有 IME 后端 |

这条"拿不到就说 Unknown / 0 / 空，绝不编造 plausible 的假值"的约定与
`SystemInfo` 一致（见 `doc/APPLICATION_SCREEN_SCRIPT_API.md`）。

---

## 2. `GetAxis` / `GetAxisRaw`：两个方法不能塌成一个

Unity 的 legacy 轴语义：

* **有输入**时朝目标按 **Sensitivity**（默认 3.0/秒）爬 —— 不是立刻到位；
* **无输入**时按 **Gravity**（默认 3.0/秒）衰减回 0 —— 松手后还有一小段滑行；
* `|值| < Dead`（0.001）直接归 0 —— 摇杆回中时的零点抖动；
* 结果恒在 `[-1, 1]`。

`GetAxisRaw` 跳过前三条，直接给 -1/0/1。

⚠️ **Gravity 或 Sensitivity 任一个归零，`GetAxis` 就退化成 `GetAxisRaw`**，
两个方法变成同一个函数 —— 这是后来"简化这段代码"最可能的走法，所以变异测试里
IP21/IP22 各留了一条专门盯它。

判据抽成了**公开纯函数**：

```cpp
static float AdvanceAxis(float current, float raw, float dt);
```

因为走 `GetAxis` 那条路要先有 `InputSystem`，而 `InputSystem` 要 `WindowSystem`；
无头进程里根本走不到。做成显式入参/出参之后，测试可以直接喂数（IN28–IN35、IN42）。

### 轴名映射

| Unity 名 | 增强输入的 action |
| --- | --- |
| `Horizontal` | `Move.x` |
| `Vertical` | `Move.y` |
| `Fire1` | `Fire` |
| 其它（如 `Mouse X`） | 按原名透传 |

按钮同理：`Fire1` → `Fire`。

---

## 3. 触摸：`Touch` 值类型 + 抬起的那一帧

### 3.1 为什么 `Touch` 是值 struct 而不是 ZCLASS

Unity 的 `Touch` 是 struct：脚本读 `touch.position` 拿到的是**快照**，改它不影响
引擎。`ZCLASS` 会发布成引用类型，语义就错了。

所以 `Touch` 走"值类型模式"：C++ 侧普通 struct + `Converter<Touch>`，d.ts 侧
`declare interface Touch`，`GetTouches()` 返回 `Touch[]`。

14 个字段与 Unity 同名同义：`fingerId` `position` `rawPosition` `deltaPosition`
`deltaTime` `tapCount` `phase` `pressure` `maximumPossiblePressure` `type`
`radius` `radiusVariance` `altitudeAngle` `azimuthAngle`。

⚠️ `pressure` / `maximumPossiblePressure` 在不支持压力的设备上是 **1.0 不是 0**。
这是 Unity 明确规定的（0 会被当成"没按下去"）。`radius` / `radiusVariance` /
`altitudeAngle` / `azimuthAngle` 是真的拿不到，给 **0**。

### 3.2 相位：真正动过才是 `Moved`

宿主（Android / WM_POINTER）**每帧都推 move 事件**，不管手指动没动。所以：

```
OnTouch(Move):  frame_dx/dy 累加；**位移非 0** 才 phase = Moved 且 moved_this_frame = true
EndFrame():     phase == Moved 且 !moved_this_frame  ⇒ 降为 Stationary
                frame_d* 清零，moved_this_frame 复位
```

少了 `moved_this_frame` 这一位，每个静止的手指都会永远报 `Moved`，靠
`deltaPosition` 判静止的玩法逻辑永远不会 settle。

### 3.3 抬起的手指只活一帧

Unity 的 `Input.touches` 在**抬起的那一帧仍然能枚举到**这个手指（相位 `Ended`），
下一帧消失。少了这条，脚本永远读不到抬手；多了这条（`m_EndedThisFrame` 不清），
`touchCount` 只涨不落。

枚举顺序：**还按着的在前（按 fingerId 升序），本帧结束的在后**。
`m_Touches` 是 `std::map`，遍历顺序刚好按 id 升序，但那是"碰巧"——`GetTouchAt`
显式排序，不依赖容器。

`Input.GetTouchCount()`（内部摇杆/相机那条路）与 `Input.touches.length` 是**两个
不同的计数**：前者只数还按着的，后者含本帧刚抬起的。EN24/EN26 就是钉这一条。

### 3.4 连击（tapCount）

窗口 `kDoubleTapSeconds = 0.3`，落点容差 `kDoubleTapSlopPx = 30`，连击封顶 10。
三者缺一：窗口没了 → 隔多久点都继续 +1；容差没了 → 在屏幕另一头点一下也算双击。

---

## 4. `inputString`：手工 UTF-8 编码

`OnChar(codepoint)` 收到的码元可能来自 WM_CHAR（UTF-16）或 IME 提交，所以自己
按 1/2/3/4 字节四档编码。丢掉的三种：

* **孤立代理对**（`0xD800..0xDFFF`）—— 坏数据，替它编一个序列会变成乱码。
  丢一个字符比显示一个 U+FFFD 好查。
* `> 0x10FFFF`。
* **控制字符**（`< 0x20` 与 `0x7F`）—— 回车走 `GetKeyDown`，不该变成输入框里的
  一个字符。

⚠️ `inputString` 是**一帧的读数**：`EndFrame()` 里收集器滚成读数后清空，否则每个
字符会被读到两帧。

⚠️ 4 字节那一档的边界要单独测。**只测 U+1F600 抓不出位移错**：`0x1F600 >> 18`
和 `>> 17` 都是 0（0x1F600 = 128000 < 2^17），两种实现编出来的首字节一模一样。
首字节里真正带位的是 ≥ U+40000 的那一段，所以测试里有一条 U+10FFFF
（`F4 8F BF BF`）。

---

## 5. `TouchType` 为什么不是 ZENUM

TS 的 `lib.dom.d.ts` 已经有一个全局 `type TouchType = "direct" | "stylus"`。
再发一个 `declare enum TouchType` 会撞名，报错是
`TS2693: 'TouchType' only refers to a type`，而且**落在用户工程**里。

所以 `TouchType` 保持普通 C++ enum（取值 0/1/2 不变），d.ts 里 `type: number`，
脚本侧写 `touch.type === 0`。

推论：**给值类型新增"可能与 DOM 同名"的枚举前，先确认 d.ts 能过 `tsc`**。
`TouchPhase` / `DeviceOrientation` / `IMECompositionMode` 都没撞，所以它们照常
是 ZENUM；但 ZENUM 必须放**全局命名空间**（`zht` 生成的 `.gen.cpp` 在全局作用域
引用枚举名且不加限定，见 `doc/PHYSICS_SCRIPT_API.md` 里 `ParticlePlayState` 那一节）。

---

## 6. `ResetInputAxes()` 清轴，不清键

Unity 的语义是"把所有轴归零"，**按键状态不受影响**。实现里只清累积器，
`m_PressedKeys` 不动 —— 否则打开菜单的那一帧，所有还按着的键都会被当成松开，
`GetKey` 会闪一帧。

---

## 7. 发布面清单

方法（`Input.xxx(...)`）：

```
GetKey  GetKeyDown  GetKeyUp
GetMouseButton  GetMouseButtonDown  GetMouseButtonUp  GetMouseDelta
IsActionActive  GetActionAxis1D  GetActionAxis2D
GetTouchCount  GetTouchIdAt  GetTouchPosition  GetTouchDelta  GetTouchFrameDelta
GetAxis  GetAxisRaw  GetButton  GetButtonDown  GetButtonUp  ResetInputAxes
GetTouch  GetJoystickNames
```

（`GetAnyKey` / `GetInputString` / `GetTouches` / `GetMousePresent` / … 在 C++ 里
是 `ZFUNCTION`，但它们同时是某个属性的 getter，所以只在 TS 里以属性形式出现。）

属性（`Input.xxx`）：

```
mousePosition  mouseScrollDelta  anyKey  anyKeyDown  inputString
touchCount  touches  mousePresent  touchSupported
stylusTouchSupported  touchPressureSupported
deviceOrientation  acceleration  gravity
multiTouchEnabled      (get/set)
backButtonLeavesApp    (get/set)
simulateMouseWithTouches (get/set)
imeCompositionMode     (get/set)
imeIsSelected  compositionString
```

⚠️ `multiTouchEnabled` 默认 **true**（Unity 的默认值），`backButtonLeavesApp`
默认 **false**。这两个是脚本可写的进程内设置，**设置自身就是数据源**，所以无系统
时读写也照常工作（不是降级空操作）。

---

## 8. 测试与变异

| 位置 | 内容 |
| --- | --- |
| `UnityParitySmokeTest.cpp` → `TestInputParity()` | EN0–EN44（驱动 `EnhancedInputSystem`）+ IN0–IN43（门面安全默认值 + `AdvanceAxis` + 发布面源码门） |
| `Scripts/Mover.ts` → `inputProbe` | 端到端：默认值 / `Touch` 值类型字段 / 设置回环，28 段全 `true` |
| `ScriptComponentSmokeTest.cpp` → ZA3 | 比对 `inputProbe` 的 golden |
| `.mutate_ip.py` | 34 个变异体，**34/34 caught** |

变异测试里三条值得记的教训：

1. **IP05 一开始是等价变异**（4 字节位移 18→17）。对 U+1F600 两种写法产出同一个
   字节 —— 变异"存活"不代表测试弱，可能只是挑的样本落在不敏感区间。修法是**补
   边界样本**（U+10FFFF），不是改变异。
2. **IP08 是测试缺口**：只测了"Moved → Stationary"，没测"Moved 跨 EndFrame 还是
   Moved"。补 EN44。
3. **IP23 原断言是白测的**：`AdvanceAxis(-0.0005, 0, 1)` 里重力一帧走 3.0，越零
   保护本来就把它压成 0 了，死区在不在都一样。改成"推进之后仍停在死区里"的两种
   输入（IN34/IN43）才真正钉住死区。

---

## 9. 与 Unity 的已知差异

| Unity | ZEngine | 原因 |
| --- | --- | --- |
| `Input.acceleration` 是 `Vector3` | `Vector2` | 没有加速度计后端，Z 恒 0；返回二维避免脚本读出假的三轴 |
| `Input.gyro` / `compass` / `location` | 未发布 | 同上：拿不到就说没有，不发布比发一个恒零的门面好 |
| `Input.GetKey(KeyCode)` 用 `KeyCode` 枚举 | `GetKey(int)` | `KeyCodes::Key` 已发布，参数走 int；TS 侧传 `KeyCode.W` |
| `Input.GetAxis` 读 Input Manager 配置 | 映射到增强输入 action | 引擎没有 Input Manager 资产；映射表在 `AxisActionName()` |
