# Wwise 音频后端接入

SDK：`E:/Agent2/SVN/Wwise/developer2019_os/wwise_sgame`（**Wwise 2019.2.15**，本地改造版，见其 `RELEASE.md`）。

## 1. 定位：并存，不是替换

ZEngine 现在有两条**互相独立**的音频通路，运行时二选一：

```
内部通路（默认）  AudioSystem -> AudioMixer -> IAudioDevice (WASAPI / Null)
Wwise 通路       WwiseAudioSystem -> Wwise sound engine -> 平台 sink
```

`AudioSystem::Initialize()` 一开始就问 `WwiseAudioSystem` 有没有拿到输出。拿到了，
它就**主动打开 null device**——两条通路绝不会同时驱动同一组扬声器。没拿到，走原逻辑。

因此：

| 条件 | 谁在出声 |
| --- | --- |
| 默认（或 SDK 未配置） | 内部 mixer + WASAPI |
| `ZENGINE_WWISE_ENABLE=1` 且 SDK 已链入且初始化成功 | Wwise |
| 上面任一条件不满足 | 内部（并打日志说明 Wwise 为什么没上） |
| `ZENGINE_AUDIO_DISABLE=1` | 静音（Wwise 也跳过，CI 依赖这条） |

## 2. 编译期开：关

```bash
# 无 Wwise（默认，CI 就是这个）
cmake -S . -B build

# 有 Wwise
cmake -S . -B build -DZENGINE_WWISE_SDK_DIR=E:/Agent2/SVN/Wwise/developer2019_os/wwise_sgame
```

`engine/cmake/WwiseSDK.cmake` 负责探测并生成 INTERFACE target `WwiseSDK`
（include 目录 + 静态库 + `ZENGINE_HAS_WWISE=1`）。**探测失败就静默退回无 Wwise**，
并在 configure 输出里说明原因（路径不对 / 非 Win64 / 缺某个 config 的 lib）。

关键取舍：

- **链非 StaticCRT 变体**。引擎用 `MultiThreaded[$<CONFIG:Debug>:Debug]DLL`（/MDd、/MD），
  Wwise 的 `Debug(StaticCRT)` 是 /MT，混进去必吃 LNK2038。代价是 `bin/<Config>/` 必须
  有那 40 个 Wwise DLL——`WwiseSDK.cmake` 在 configure 时直接拷过去。
- 库的清单抄自 SDK 自带 `IntegrationDemo_Windows_vc160.vcxproj`，**不精简**。少链一个
  插件库的表现不是链接错误，是运行时"声音没了"且无日志。

`WwiseAudioSystem` **无条件编译**。`ZENGINE_HAS_WWISE` 没定义时，每个入口点返回失败、
`GetBackendName()` 返回 `"Wwise (not compiled in)"`。这不是"假支持"：
`IsCompiledIn()` / `IsActive()` / 返回值三层都如实报告，调用方能区分。

## 3. 运行期开关

| 环境变量 | 作用 |
| --- | --- |
| `ZENGINE_WWISE_ENABLE=1` | 启用（不设 = 用内部通路） |
| `ZENGINE_WWISE_BANK_DIR=<dir>` | bank 目录，默认 `asset/soundbanks/Windows` |
| `ZENGINE_AUDIO_DISABLE=1` | 全静音，优先级最高 |

bank 目录不存在时会**打 WARNING**。因为下游所有症状（游戏没声音、`PostEvent` 返回
`AK_INVALID_PLAYING_ID`）长得都像代码 bug，不说一句就会浪费半天。

## 4. API

`WwiseAudioSystem`（`<engine>/Source/Runtime/Function/Audio/Wwise/WwiseAudioSystem.h`）
头文件**不含任何 Wwise 头**，跨边界的只有 POD，所以没有 SDK 的构建也能编译所有调用方。

```cpp
auto* wwise = GET_SYSTEM(WwiseAudioSystem);

wwise->SetBankDirectory(".../GeneratedSoundBanks/Windows");
wwise->LoadBank("Init");                       // <dir>/Init.bnk
wwise->RegisterGameObject(100, "Footstep");
wwise->SetGameObjectTransform(100, pos, fwd, up);
wwise->SetListener(listener);
uint64_t playing = wwise->PostEvent("Play_Footstep", 100);
wwise->StopAll(100);
wwise->SetRTPC("PlayerHealth", 0.5f);
wwise->SetSwitch("Surface", "Stone", 100);
wwise->SetState("GameState", "Combat");
wwise->RenderAudio();                          // 每帧

// 按 playing id 操作单条发声（Stop / 暂停 / 恢复）
wwise->StopPlayingId(playing, 0.1f);           // 0.1s 淡出
wwise->SetPlayingPaused(playing, true);
wwise->SetPlayingPaused(playing, false);
wwise->IsPlayingId(playing);                   // "这个 id 还活着吗"
```

事件**按名字投递**（`PostEvent(const char*, ...)` 走 Wwise 的字符串哈希），
**不需要从 Wwise 工程生成任何 ID 头文件**。

`IsPlayingId()` 不是查询 Wwise —— 2019.2 没有 "这个 playing id 还活着吗" 的查询接口。
它是**簿记**：`PostEvent` 每次都挂 `AK_EndOfEvent` 回调，回调在音频线程里把结束的 id
写进一个无锁环形缓冲（`std::atomic<uint64_t>`，容量 256），主线程读这个环判断。
音频线程里**不能加锁**（一加每声都爆音），所以这个环是**故意有损**的：环满了就丢最老的，
丢失的后果是 `IsPlayingId()` 对一个已结束的 id 多返回一次 true，不会误报 false。

## 5. 几个"错了不报错"的坑（都已由测试钉住）

1. **注册顺序**。`WwiseAudioSystem` 必须注册在 `AudioSystem` **之前**。
   反过来的话 `AudioSystem::Initialize()` 永远得到"Wwise 没启用"，然后自己去开 WASAPI ——
   能编译、能运行、两条通路都活着，只是 Wwise 那套永远没人用。
2. **地板平面**。Wwise 默认 `AkFloorPlane_XZ`（Y-up）。ZEngine 是 Z-up，
   必须 `AkFloorPlane_XY`；不设则是所有 3D 声像左右镜像，**没有任何报错**。
3. **`SetBusVolume` 在 2019.2 不存在**（2021 才加）。总线音量要走 RTPC。
   测试 B10 直接断言源码里不出现这个调用。
4. **bank 基础路径没注册** ⇒ `LoadBank("Init")` 返回 `AK_FileNotFound`，
   但只看返回值会以为"bank 坏了"。
5. **Wwise 模式下文件播放**。Wwise 放的是 event 不是文件。
   `AudioSystem::Play()/PlayOneShot()` 在 Wwise 接管时**直接报错返回 invalid**——
   否则它们会乖乖渲染进 null device，返回一个完全合法的 voice id，然后一点声音都没有。
6. **播放路由必须显式裁决**。上面那条只挡住了"从 `AudioSystem` 直接放文件"，挡不住
   `AudioSourceComponent`：它同时持有 `m_ClipPath` 和 `m_WwiseEvent` 两个字段，
   两边都填了就有个"听谁的"的问题，而**任何一种静默回退都是错的**（回退到 clip ⇒ Wwise
   模式下没声音；回退到 event ⇒ 内部模式下没声音）。所以路由是一个纯函数
   `ResolveAudioPlayRoute(wwise_owns_output, clip_path, wwise_event)`，
   返回 `kClipFile` / `kWwiseEvent` / `kNone`；**`kNone` 要打 error，不能当空操作**。
   判据是"哪个后端拥有输出"，不是"哪个字段非空"——字段非空只决定这条路能不能走。
7. **Wwise 的 game object id 不能从 1 开始**。默认 listener 占用 id 1，
   所以 `AudioSourceComponent` 的发射体从 `0x40000000` 起编号（`kEmitterGameObjectIdOffset`），
   避免发射体抢走 listener 的 id。

## 6. 已知边界

- **ZEngine 还没有自己的 `.bnk`**。本机没装 Wwise Authoring，bank 必须由 Authoring
  生成（`GeneratedSoundBanks/<Platform>`）。目前测试用 SDK 自带的
  `samples/IntegrationDemo/Motion/WwiseProject/GeneratedSoundBanks/Windows`。
- 文件 ID（file package）解析没接，返回 `AK_FileNotFound`；ZEngine 现在只用散装 `.bnk`。
- `AudioSourceComponent` 已通 Wwise（路由裁决 / emitter transform / stop / pause / is-playing
  都接了），`AudioListenerComponent` 仍然只接内部通路。
  Wwise 模式下 component 需要填 `m_WwiseEvent`（event 名），`m_ClipPath` 被忽略且**会打 error**；
  内部模式下反之。
- 只接了 Windows（vc160 x64）。Android / iOS / OpenHarmony 的库目录 SDK 里都有，
  接法是同一套：加 platform dir + 库清单。

## 7. 测试

```bash
cmake -B build -DZENGINE_BUILD_WWISE_SMOKE_TEST=ON
cmake --build build --target WwiseIntegrationSmokeTest
ctest --test-dir build -C Debug -R WwiseIntegrationSmokeTest

# C 组（真机）：需要声卡 + bank 目录
WwiseIntegrationSmokeTest --with-device \
  --bank-dir E:/Agent2/SVN/Wwise/.../GeneratedSoundBanks/Windows
```

默认（**未编 Wwise**，CI 就是这个）跑 37 条断言；带 `-DZENGINE_WWISE_SDK_DIR=...` 的构建跑 31 条
——差别只在 A 组：没编 Wwise 时 A 组会展开成 10 条（每个入口点都要返回失败并如实自报家门），
编了就只有 4 条。B 组 11 条 + D 组 16 条两种构建一样，都是**源码级**、无声卡也能跑，
所以它们在仓库里才有意义。加 `--with-device` 后再多 12 条（C 组，只在 SDK 构建上有意义，
非 SDK 构建上它会**显式判失败**而不是跳过）。C 组跳过时会**打印跳过条数**，不是静默零断言。

D 组存在的唯一理由就是老规矩：**接线门**。路由函数写得再对，没接进 `Play()` 等于没实现，
而且**能编译能运行、静默失效**（走内部通路、什么都不响）。所以 D8–D16 全是源码级计数断言，
钉住 `ResolveAudioPlayRoute` / `PostEvent` / `PushWwiseEmitterTransform` / `StopPlayingId` /
`IsPlayingId` 真的出现在 `AudioSourceComponent.cpp` 里，以及 `AK_EndOfEvent` 真的出现在
`WwiseAudioSystem.cpp` 里。`PushWwiseEmitterTransform` 要求 ≥3 次调用（Play / Tick / 至少一处），
因为只调一次的话"只在 Play 时设过 transform"这种移动物体不跟声像的 bug 抓不到。

**未加入默认 CI 矩阵**：C 组需要声卡。A/B/D 组不需要，这才让它在仓库里有意义。

变异验证（6 个变异，每个都被预期断言捕获）：

| 变异 | 变红 |
| --- | --- |
| `IsCompiledIn()` 恒 false | A1, A4 |
| `eFloorPlane` 改成 XZ | B9 |
| 注册顺序颠倒 | B3 |
| 不注册 bank 基础路径 | C5, C6, C12 |
| Wwise 接管但没填 event ⇒ 悄悄回退到 clip 通路 | D5 |
| 路由不看出端拥有者（只看字段非空） | D4, D5, D6, D7 |
