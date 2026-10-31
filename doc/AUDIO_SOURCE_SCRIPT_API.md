# AudioSource 脚本 API（`AudioSourceComponent`）

配套源码：

| 文件 | 作用 |
| --- | --- |
| `engine/Source/Runtime/Function/Framework/Component/Audio/AudioSourceComponent.h/.cpp` | 音源组件（Unity 的 `AudioSource`） |
| `engine/Source/Runtime/Function/Audio/AudioMixer.h/.cpp` | 纯 DSP：voice / bus / master，新增播放头查询与延迟 |
| `engine/Source/Runtime/Function/Audio/AudioSystem.h/.cpp` | 引擎门面，转发 mixer |
| `engine/Source/Runtime/Function/Audio/AudioCommon.h` | `AudioPlayParams`（新增 `m_DelaySeconds`） |

既有的三层结构没动（`AudioClipData` = 解码后的 PCM / `AudioMixer` = 混音 /
`IAudioDevice` = 输出），Wwise 路线的取舍见 `doc/WWISE_AUDIO.md`。

## 0. 这次补的是什么

组件此前只有 5 个 `ZFUNCTION`（`Play` / `Stop` / `SetPaused` / `IsPlaying` /
`GetVoiceId`）。`volume` / `pitch` / `loop` / `spatialBlend` 这些是 `ZPROPERTY`
自动导出的字段，**字段有，动作没有**：

* 播到哪儿了 —— 没有（Unity 的 `time` / `timeSamples`）；
* 延迟播放 —— 没有（`PlayDelayed`）；
* 暂停的查询端 —— 只有 `SetPaused`，问不出来（`IsPaused`）；
* 静音 —— 没有（`mute`。用 `volume = 0` 代替会**丢掉原音量**，取消静音恢复不回去）；
* 在不占用本组件 voice 的前提下叠一个一次性音效 —— 没有（`PlayOneShot`）。

## 1. 新增的脚本面

```ts
// 播放
source.Play(): number                      // 返回 voice id
source.PlayDelayed(seconds): void
source.Stop(fadeOutSeconds = 0): void
source.Pause(): void
source.UnPause(): void
source.IsPaused(): boolean
source.IsPlaying(): boolean
source.PlayOneShot(clipPath, volumeScale): number   // 不占用本组件的 voice

// 进度
source.time              // 只读（写端是 SetTime）
source.SetTime(seconds): void
source.timeSamples       // 只读
source.clipLength        // 只读，已折算 pitch
source.clipPath          // 读写

// 字段（ZPROPERTY）
source.mute: boolean
source.volume: number
source.pitch: number
source.loop: boolean
source.playOnAwake: boolean
source.spatialBlend: number
source.minDistance / source.maxDistance: number
source.fadeInSeconds: number
source.priority: number
```

`PlayOneShot` 与 `Play()` 唯一但关键的区别：**它不抢本组件的 voice**。所以它
可以在背景音乐上叠脚步声；`Play()` 不行（一个 `AudioSource` 是单个发声体，
叠两条 voice 之后"到底在播吗"就没法回答了）。一次性音效也**不会继承 `loop`**
—— 叠一个循环体上去就再也停不下来了，而且没有人会去 `Stop` 它。

## 2. 这次修掉的两个真 bug

### 2.1 2D 音源改 volume / pitch / mute 完全不生效

`Tick` 里原来有一条 `if (m_SpatialBlend <= 0) return;`，位置在推参数**之前**。
于是：

* 那些字段是 `ZPROPERTY`，脚本**直接写字段**，绕过了任何 setter；
* 唯一的生效路径就是 `Tick` 里的 `UpdateVoice`；
* 2D 音源（`spatialBlend == 0`）每帧在这里提前返回，参数永远推不下去。

表现是"游戏里调小音量没反应"，而且**静默**。3D 音源因为要更新位置，顺带把参数
也推了，所以这个 bug 一直没被发现。

判据是**输出峰值**：`volume` 从 1 调到 0.25，渲染出来的峰值就得变成四分之一；
静音就得是 0。光看参数"推出去了没有"量不到这件事。

### 2.2 暂停一帧之后 voice 句柄被丢掉

`Tick` 判断"这条 voice 还在吗"时只问了 `IsPlaying()`。而
`AudioSystem::IsPlaying()` 按 Unity 的语义来 —— **`AudioSource.isPlaying` 在
`Pause()` 之后是 `false`**。于是暂停中的音源在下一帧被当成"播完了"：句柄一清，
`UnPause()` 从此没有对象可以恢复，且没有任何日志。

修法是分开问两件事：

```cpp
const bool voice_alive = (audio_system != nullptr) &&
                         (audio_system->IsPlaying(m_VoiceId) || audio_system->IsVoicePaused(m_VoiceId));
```

"不在播"和"没了"不是一回事。测试里连着 Tick 十帧再断言 `IsPaused()` 仍为真，
就是专门盯这一条。

## 3. `AudioMixer` 新增的播放头接口

```cpp
static constexpr double kUnknownPlaybackTime = -1.0;
double GetVoicePlaybackTime(AudioVoiceId) const;   // 秒；找不到 = -1
uint64_t GetVoicePlaybackFrames(AudioVoiceId) const;
double GetVoiceDuration(AudioVoiceId) const;       // 已折算 pitch；找不到 = -1
bool SetVoicePlaybackTime(AudioVoiceId, double);   // 夹到 [0, 最后一个可播帧]
bool IsVoicePaused(AudioVoiceId) const;
```

### 3.1 为什么"找不到"是 -1 而不是 0

**0 是一个合法的播放位置**（刚开头）。拿 0 当"没有这条 voice"，"还没开始"和
"已经在播"就分不开了。组件层的 `GetTime()` 负责把 -1 翻译成 0 再给脚本 ——
脚本世界里"没在播"就是 0。

### 3.2 为什么 `SetTime` 夹到"最后一个可播帧"而不是"帧数"

播放头等于帧数时，`Render()` 里 `frame_index == 帧数`，于是
`m_Samples[frame_index * channels]` 是一次**越界读**（线性插值还要再读下一帧）。
正常播放永远碰不到这个状态（播放头是推进**之后**才判到头的），只有从外面 seek
才可能把它摆到正好等于帧数。

少一帧的代价是"跳到末尾"要等下一次渲染才真正结束 —— 同一个可观测结果，但不是
UB。变异测试里把上界改回帧数，测试进程直接 `0xC0000005`。

负数同理要夹到 0：负播放头会让 `static_cast<uint64_t>(playhead)` 变成一个巨大
的下标。

## 4. `PlayDelayed`

两条路线的等待逻辑不同源，所以分开实现：

| 路线 | 延迟在哪儿等 |
| --- | --- |
| 内部后端（解码 clip 文件） | `AudioPlayParams::m_DelaySeconds` → mixer 的 `Voice::m_DelayRemaining`，`Render()` 里倒数 |
| Wwise | event 一旦 post 就立刻响，没有"延迟参数"；组件自己在 `Tick` 里数完再 post（`m_HasWwisePendingDelay`） |

延迟放在 `AudioPlayParams` 里而不是给 `Play()` 多加一个形参：`UpdateVoiceParams`
整块替换 params，多一个形参的话组件每帧推参数时会把延迟一起抹掉。放进来之后
它也只在 `Play()` 时读一次 —— 播放中的声音改它没有意义。

**等待期间不推进播放头**：这一段静音是"还没开始"，不是"播到了一段空白"。推进
的话 `PlayDelayed(2)` 之后 `time` 会是 2 而不是 0，淡入也会在声音出现之前跑完。

`Stop()` 会取消还没排上的 Wwise 延迟 —— 否则停了之后 `Tick` 仍把 event post
出去，"停了还在响"。

## 5. 几个会让人踩的既有语义

| 现象 | 原因 |
| --- | --- |
| 无头 / null device 下 `time` 恒为 0 | 进度只在 `AudioMixer::Render()` 里推进。没有设备在跑就没人调 `Render()`。测试里手动 `Render` 就等价于声卡把这段播过去了 |
| 2D 音源满音量渲染出来峰值是 **0.707** 不是 1 | 等功率声像：居中时左右各 `1/√2`。这是对的 |
| `IsPlaying()` 暂停时为 false | Unity 同（`AudioSource.isPlaying` 在 `Pause()` 后为 false）。想问"还在不在"用 `IsPaused()` 或 voice id |
| `GetClipLength()` 随 `pitch` 变 | `pitch 2` 听起来只有一半长，`time` 走到头也就是一半。mixer 侧的 `GetVoiceDuration` 同样折算 |
| `Play()` 会先停掉当前这条 | 一个音源一条 voice；要叠音用 `PlayOneShot` |

## 6. 与 Unity 的偏差

| 点 | Unity | ZEngine | 原因 |
| --- | --- | --- | --- |
| `time` 的写端 | 属性可读写 | `SetTime()` | 静态属性做不了：`ZSCRIPTPROPERTY` 的 getter 不允许 static |
| `PlayOneShot` 重载 | `(clip)` / `(clip, volumeScale)` | `(clipPath, volumeScale)` | 用路径寻址，与 `clipPath` 字段一致 |
| `AudioSource.clip` | `AudioClip` 对象 | `clipPath: string` | 本引擎的 clip 是"解码后的 PCM + 缓存键"，没有可跨语言的对象句柄 |
| `PlayClipAtPoint` | 静态 | 没有 | 等价于 `AudioSystem::PlayOneShot` + 位置，尚未做成门面 |
| `SetScheduledStart/EndTime` | 有（DSP 时钟） | 没有 | 没有 DSP 时钟概念；`PlayDelayed` 覆盖常见用法 |
| 滤波器 / 混响发送 / 多普勒 | 有 | 没有 | 需要 DSP 图，V1 没有 |

## 7. 已知未做

* **`AudioSource.spread` / `rolloffMode` / `dopplerLevel` / `reverbZoneMix`**：需要
  DSP 图与区域系统。
* **`AudioSource.outputAudioMixerGroup`**：只有具名 bus（`busName`），没有
  AudioMixerGroup 资产。
* **`AudioSource.priority` 的语义**：目前只用于 voice 上限时的抢占排序，没有
  Unity 的 0..256 归一化含义。
* **`PlayScheduled`**：见 6。
* **`AudioSource.isVirtual` / `virtualize`**：没有虚拟 voice 概念（超过 64 条
  直接按优先级抢占，不排队）。

## 8. 测试与变异覆盖

`UnityParitySmokeTest` 的 **AW 组（行为，45 条）** 与 **AV 组（接线门，22 条）**。
AW 组靠手动 `Render` 驱动 mixer，因此断言的都是物理后果（输出峰值、播放头秒数、
源帧数），不是"参数被赋值了"。

`.mutate_au.py` 有 18 个变异，全部被抓到（M18 是编译期失败——去掉 `ZPROPERTY`
后 `ZhtScriptSet_m_Mute` 就不存在了）。其中 M13（seek 夹到帧数）直接把测试进程
打崩，印证了 3.2 那条越界读是真的。
