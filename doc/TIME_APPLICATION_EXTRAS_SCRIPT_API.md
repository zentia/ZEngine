# `Time` 补项 / `Application` 补项 / 粒子播放控制

TS 侧：`Time.maximumDeltaTime`、`Time.captureFramerate`、`Time.timeSinceLevelLoad`、
`Application.identifier`、`Application.OpenURL(url)`、粒子组件上的
`Play()` / `Stop()` / `Pause()` / `isPlaying`。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Core/Time/TimeState.{h,cpp}` | 单帧步长的两个闸门 + 关卡时钟 |
| `Runtime/Scripting/ScriptTime/ScriptTime.h` | `Time` 门面补项 |
| `Runtime/Scripting/ScriptScene/ScriptScene.cpp` | 加载成功时调 `NotifyLevelLoaded()` |
| `Runtime/Application/Application.{h,cpp}` | `Application` 补项 + `SystemLanguage` / `ThreadPriority` |
| `Runtime/Function/Framework/Component/Particle/ParticleComponent.{h,cpp}` | 粒子播放状态 |

---

## 1. `Time.maximumDeltaTime` / `Time.captureFramerate`

两个都是**真的接进 `TimeState::BeginFrame`** 的，不是只记状态。

### 作用点：本帧 delta 诞生之前

```
sim_delta = captureFramerate > 0 ? 1/captureFramerate : simulation_delta_time
sim_delta = clamp(sim_delta, 0, maximumDeltaTime)
deltaTime = sim_delta * timeScale
```

放在更后面（比如给脚本的 `deltaTime` 单独夹一次）会造出"世界用了 2 秒、脚本看到
0.33 秒"的分叉 —— 那比不夹更糟，因为脚本的积分与世界的积分从此不同步。

### `maximumDeltaTime`（默认 1/3 秒，与 Unity 相同）

卡一顿（断点、GC、切窗口）之后 deltaTime 会是一个巨大的值：物理一步积分出去就穿
墙、角色瞬移出地图。夹住它 = 卡顿时**慢放**而不是让世界跳过去。副作用是模拟时间
落后于真实时间 —— 这正是想要的。

**先夹再缩放**，不是先缩放再夹：Unity 的语义是"限制缩放后的实际步长"，于是
`timeScale = 0.1` 时上限仍然是 1/3 秒，慢放不会因为限幅变得更细碎。

`<= 1ms` 的值夹到 1ms。0 会让世界永远不推进 —— 那是"冻结"，应该用
`timeScale = 0` 表达，两者语义不同。

### `captureFramerate`（默认 0 = 关闭）

> 0 时每帧步长被钉成 `1/value`。录序列帧用它：真实帧率 200 也好 12 也好，每帧固定
推进 1/30 秒，输出的图片序列在时间上是均匀的。

⚠️ 它改的是**步长**不是真帧率 —— 引擎仍然尽力跑，只是世界推进的量被固定了。要
真限帧用 `Application.targetFrameRate`。

`<= 0` 一律当"关闭"，不夹到 1fps —— 传 0 是明确的"我要关掉"，夹到 1 会变成"每帧
1 秒"的慢动作，那看起来就像引擎挂了。

---

## 2. `Time.timeSinceLevelLoad`

由 `SceneManager` 在**加载成功之后**调 `TimeState::NotifyLevelLoaded()` 打点（
`LoadScene` / `LoadSceneAdditive` / `ReloadActiveScene` 三处）。

与 `Time.time` 的区别：后者是"自引擎启动"。关卡限时 / 计时赛这类玩法计时一律用
这个 —— 用 `Time.time` 会把"引擎启动到进关卡那几秒"也算进去，编辑器里 PIE 一下就
偏一点。

叠加加载也重置：Unity 的语义是"自**最后一次**加载"，不管是不是叠加。

`TimeState::Reset()` 会一并归零关卡时钟，否则 `GetTimeSinceLevelLoad()` 会夹在 0
上一段时间才重新开始涨。

---

## 3. `Application` 补项

| 项 | 数据源 | 说明 |
| --- | --- | --- |
| `identifier` | `companyName` + `productName` | 反转域名形式；非法字符压成点分段；任一为空时 `"com.zengine.unnamed"` |
| `temporaryCachePath` | `std::filesystem::temp_directory_path()` | 取不到返回**空串**，不 fallback 到 persistentDataPath |
| `consoleLogPath` | `persistentDataPath/Player.log` | 是"该把日志放哪"，不是"引擎正在写的那个文件" |
| `isFocused` | `WindowSystem::IsWindowFocused()` | 无窗口系统时返回 **true**（false 会在 headless 下把世界冻住） |
| `isMobilePlatform` | 编译期平台 | Android / iOS / 鸿蒙 |
| `isConsolePlatform` | 恒 `false` | 没有主机后端；不用"既不是桌面也不是移动"推导（那会把 Web 也算成主机） |
| `systemLanguage` | `GetUserDefaultUILanguage` / `LANG` | 只映射主流语种，其余 `Unknown` |
| `backgroundLoadingPriority` | 进程内状态 | ⚠️ 暂时**没有消费方**（异步加载还没分优先级队列） |
| `OpenURL(url)` | `ShellExecuteA` / `open` / `xdg-open` | 见下 |

### `identifier` 为什么不读"包名字段"

工程里没有那一份设置（`PlayerSettings` 只有 company / product / version）。编一个
字段出来只会多一处要维护的真相。要支持自定义包名时应该给 `PlayerSettings` 加字段，
而不是在这里加特例。

### `OpenURL` 的白名单校验（防 shell 注入）

只接受 `http://` `https://` `mailto:` `file://` 开头；其余字符里不能有空白与任何
shell 元字符（`` ` `` `$` `\` `;` `|` `&` `<` `>` `(` `)` `{` `}` `[` `]` `*`
`?` `!` `#` `"` `'` 与控制字符）。

为什么要校验：POSIX 那两条路只能走 `system("open <url>")` /
`system("xdg-open <url>")`，而 `system()` **走 shell**。URL 常常来自外部（服务器下发
的公告链接、玩家点的聊天消息），直接拼进命令行就是一条命令注入。Windows 用
`ShellExecuteA`（不过 shell），是唯一安全的那一条路。

⚠️ 这条路径**会阻塞**（`system()` 是同步的）。不要在主循环里对不可信来源的 URL 调
它。Android / 鸿蒙没有可移植的 CLI，显式返回 `false`（= 没打开），不假装成功。

### `systemLanguage` 为什么不用 `setlocale`

`std::getenv("LANG")` 而不是 `setlocale(LC_ALL, "")` 再 `localeconv` —— 后者会
**改进程的 locale**（影响 printf 的数字格式、`strtod` 的小数点）。一个"我只是想问
系统语言"的查询不该有这种副作用。

---

## 4. 粒子播放控制

`ParticleComponent` 现在有 `Play()` / `Stop()` / `Pause()` / `Clear()` /
`isPlaying` / `isPaused` / `isStopped` / `isAlive` / `time` / `playState`。

### 三态枚举而不是三个 bool

`isPlaying` / `isPaused` / `isStopped` 在 Unity 里是三个 bool，实际上**不独立**：
`isStopped` 为真时另外两个必为假。用一个 `ParticlePlayState` 枚举，是为了让"两个
真同时成立"这种不可能状态**表示不出来** —— 那种状态在 `Tick` 里的行为没有定义。

### `Tick` 里的行为

* `Stopped` → 完全不参与（连 transform 更新也跳过：停止的发射器不需要知道自己
  位置变了）；
* `Paused` → **不**推进模拟（不 `AddTickParticleEmitter`），但照旧同步 transform。
  暂停中移动发射器，恢复后粒子从新位置继续 —— 而不是"移动被吞掉"；
* `Playing` → 正常推进，并累加 `m_Time`。

### 没有 `Emit(count)` / `GetParticles()` / `SetParticles()`

这些要能**读写单个粒子**，而本引擎的粒子是 GPU 驱动的：粒子数据住在 GPU buffer
里，CPU 侧没有可读回的数组。给一个 `GetParticles()` 却只能返回空数组，是典型的假
成功。

模块那一半（main / emission / shape / velocityOverLifetime …）对应的是
`ParticleComponentRes` 的字段 —— 那些走资产序列化，不是运行时脚本 API，要导出得
先把 `ParticleDesc.h` 那一层抬上来。

---

## 5. TS 用法

```ts
// 卡顿时慢放而不是让世界跳过去
Time.maximumDeltaTime = 0.25;

// 关卡计时
const elapsed = Time.timeSinceLevelLoad;

// 进玩法锁鼠标、隐藏光标
Cursor.SetCursorState(false, CursorLockMode.Locked);

// 打开公告链接（校验失败返回 false）
if (!Application.OpenURL(url)) {
    Debug.Log("拒绝打开：" + url);
}
```
