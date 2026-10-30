# 固定时间步进（FixedUpdate）

> 配套：脚本侧 API 在 `doc/SCRIPT_TIME_API.md` §3.1。

## 1. 为什么要有这一层

帧间隔是**墙钟**决定的。同一段模拟在 60Hz 和 144Hz 的机器上跑出来，步数与
步长都不一样。任何"每步积分"的东西只要吃帧间隔，结果就跟帧率绑死：

* 换台机器复现不了（开发机上永远看不出问题）；
* 联网对不上（两端帧率不同 ⇒ 状态分叉）；
* 回放对不上（录的是时间，放的是帧）。

固定步进把"模拟推进多少"和"画了多少帧"拆开：模拟永远以 `fixedDeltaTime` 为
步长推进，一帧内跑 0..N 次，多出来的零头留在累加器里等下一帧。

**这是物理、联网同步、确定性回放的共同前置**。在此之前全仓 `FixedUpdate` /
`fixed_delta` 零命中（可复核：`grep -r FixedUpdate engine/Source/Runtime`），
也就是说这些能力没有地基。

## 2. 算法

`TimeState::AdvanceFixedSteps()`，由 `BeginFrame()` 在算完缩放后的逻辑 delta
之后调用：

```
accumulator += scaled_frame_delta
steps = 0
while (accumulator >= fixedDeltaTime && steps < maxSubsteps):
    accumulator -= fixedDeltaTime
    steps++
if steps == maxSubsteps && accumulator >= fixedDeltaTime:
    accumulator = 0          // 追不上，丢弃积压
fixedStepTotal += steps
fixedTime += steps * fixedDeltaTime
alpha = accumulator / fixedDeltaTime
```

`Application::LogicalTick` 之后按 `GetFixedStepCount()` 循环分发，步长恒为
`GetFixedDeltaTime()`。

默认 `fixedDeltaTime = 1/50`（与 Unity 一致），`maxSubsteps = 5`。

### 三个决定的理由

**累加器吃的是缩放后的逻辑 delta，不是墙钟 delta。** `timeScale = 0.5` 时固定
步必须跟着减半，`timeScale = 0` 时一步都不跑。否则"慢放"对物理无效，变成
"脚本慢放但物理照常" —— 正是 `doc/SCRIPT_TIME_API.md` §4 明确要避免的那种不
一致。

**暂停不推进累加器。** 编辑器暂停时 `should_update_logic = false`，累加器冻结、
零头留着。语义是"时间停了"，不是"攒着等恢复那一帧一起补"。后者的后果是恢复
瞬间爆出一大批步，且总步数与"暂停了多久"耦合 —— 确定性当场就没了。

**追不上时丢弃积压。** 一帧卡了 2 秒时不能真的去补 100 步（越补越慢、越慢越
要补，经典的死亡螺旋，表现是卡一次之后帧率永久下降）。丢弃确实丢掉了这段模
拟时间（内容表现为"卡的时候慢放了一下"），但那是唯一能收敛的选择。

⚠️ 这条让"总步数"在极端卡顿时依赖帧率。所有依赖步数做确定性的东西（回放 /
同步）都必须保证不触发这个分支 —— 正常帧率下不会。

## 3. 分发链

```
Application::LogicalTick
  ├─ 按 GetFixedStepCount() 循环 ──► WorldManager::FixedTick
  │                                    └─► Level::FixedTick
  │                                          ├─► GameObject::FixedTick
  │                                          │     └─► Component::FixedTick
  │                                          │           └─ TypeScriptComponent
  │                                          │                └─► JS OnFixedUpdate(dt)
  │                                          ├─► FlushPendingDestroys()
  │                                          └─► PhysicsScene::Tick(fixed_dt)
  └─ WorldManager::Tick (帧间隔) ──► Level::Tick ──► GameObject::Tick ──► Component::Tick
```

**固定步在 Update 之前。** 物理跑完之后本帧的 `Update` 才能读到这一步之后的位
姿；反过来的话角色控制器读到的永远是上一步的位置，表现为输入到位移多延迟一
帧，且帧率越高延迟越明显。

**`Character::Tick` 不进固定步。** 它做的是跟随相机的位姿与自由相机开关
（`Character.cpp:42`），是纯表现 + 输入快照，就该跟帧走。它的 `GameObject` 本来
就在 `Level::m_Gobjects` 里，组件已经被分发到了。

**输入每帧采样一次，所有固定步共用同一份快照。** 把输入按步切分需要事件带时
间戳，属于确定性回放的下一档，现在不做。

## 4. 物理为什么挪进固定步

这是本次改动的核心，也是最容易"改了个寂寞"的地方。

在此之前 `PhysicsScene::Tick(delta_time)` 在 `Level::Tick` 里，吃帧间隔 —— 那
正是"模拟跟帧率绑死"的定义。现在它在 `Level::FixedTick` 里，步长恒定。

副作用：60fps 下物理从 60Hz 变成 50Hz（0.0167 s 一帧 vs 0.02 s 一步，平均每帧
0.83 步）。这是**正确**行为，不是退化 —— 物理不该跟着帧率变。

`Level::FixedTick` 每步都 `FlushPendingDestroys()`：一帧跑 3 步时，第 1 步里
Destroy 的对象不该再被第 2 步 tick 到。

## 5. 测试

`engine/Source/Runtime/Core/Time/Test/FixedTimeStepSmokeTest.cpp`，47 项检查，
`-DZENGINE_BUILD_FIXED_TIME_STEP_SMOKE_TEST=ON`。

```bash
cmake -B build -DZENGINE_BUILD_FIXED_TIME_STEP_SMOKE_TEST=ON
cmake --build build --target FixedTimeStepSmokeTest
ctest --test-dir build -C Debug -R FixedTimeStepSmokeTest
```

钉的是：

| 组 | 内容 |
| --- | --- |
| A | 累加器零头跨帧攒着（**带非零头样本**） |
| B | **同一段墙钟时间、不同帧率 ⇒ 同样总步数**（特性的定义本身） |
| C | 死亡螺旋闸门 |
| D/E | timeScale 与暂停都作用在累加器上 |
| F | 配置夹取（0 步长 ⇒ 死循环，必须夹） |
| G | `Reset` 归零时间但不冲掉配置 |
| H | 源码级接线门 |

### B 组的浮点选择

严格组用 2 的幂（1/32、1/64、1/128）：二进制精确，若干次加法之后仍是精确的
1.0，于是"总步数 == 64"是严格断言。1/60 这类日常帧率不精确，同一条断言只能给
±1 容差。两组都留着 —— 前一组守正确性，后一组守"真实帧率下也不离谱"。

### H 组：接线门

A~G 全绿只证明算术对。真正决定"FixedUpdate 存在"的是三处接线，而它们全都是
**能编译、能运行、静默失效**的形态：

* `LogicalTick` 不按步数循环 ⇒ 固定步一次都不跑，没人报错；
* 物理留在 `Level::Tick` 里 ⇒ `FixedUpdate` 照跑，但物理还是吃帧间隔。

所以 H 组做源码级断言（H2/H3/H4 钉主循环，H6/H7 钉物理的归属）。

### 负向验证

四个变异，每个都按预期变红之后已还原（`git diff` 已核对）：

| 变异 | 结果 |
| --- | --- |
| 累加器不累加（`+=` → `=`） | 13 项失败，含 B1。**只有 128fps 那条抓到了** —— 32/64fps 每帧本来就够一步，验证了必须包含高于固定步率的帧率 |
| 去掉死亡螺旋闸门 | C2/C3 失败，积压 9.92 秒（636 步的债） |
| 累加器吃未缩放的墙钟 delta | D1/D2 失败 |
| 暂停不冻结累加器 | D1/D2/E2/E3/E4/E5 失败 |

> ⚠️ 第四轮的第一次跑 E4 **没有**变红：暂停帧用的是整数倍（16 帧 × 1/32），
> 走完刚好回到余量 0，于是"余量不变"照样绿。这是老坑 —— 只断言"偏移为 0 的
> 那条"分不出对错。已改成 13 帧 × 1/128（留非零余量），重跑后 E4 正确变红。

## 6. 还没做的

| 项 | 说明 |
| --- | --- |
| **渲染侧插值** | `GetFixedStepAlpha()` 已导出，但引擎还没用它在本步与上一步的位姿之间插值。所以 50Hz 模拟 + 144Hz 渲染目前仍会抖。需要渲染实体保存上一步变换，是独立一件事 |
| 输入按步切分 | 需要事件带时间戳，属于确定性回放的下一档 |
| 网络同步 / 回放 | 现在**有了**可用的时间基准（`GetFixedStepTotal()`），但没人对齐过 |

## 7. 文件清单

```
engine/Source/Runtime/Core/Time/TimeState.h/.cpp          累加器 + 配置 + 夹取
engine/Source/Runtime/Application/Application.cpp         LogicalTick 按步数循环分发
engine/Source/Runtime/Function/Framework/World/WorldManager.h/.cpp
engine/Source/Runtime/Function/Framework/Level/Level.h/.cpp    FixedTick + 物理归属
engine/Source/Runtime/BaseClasses/GameObject.h/.cpp       FixedTick 分发
engine/Source/Runtime/Function/Framework/Component/Component.h  virtual FixedTick
engine/Source/Runtime/Function/Framework/Component/Script/TypeScriptComponent.*  → JS OnFixedUpdate
engine/Source/Runtime/Scripting/ScriptTime/ScriptTime.h   脚本 API
engine/Source/Runtime/Core/Time/Test/                     新增测试
```
