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
```

事件**按名字投递**（`PostEvent(const char*, ...)` 走 Wwise 的字符串哈希），
**不需要从 Wwise 工程生成任何 ID 头文件**。

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

## 6. 已知边界

- **ZEngine 还没有自己的 `.bnk`**。本机没装 Wwise Authoring，bank 必须由 Authoring
  生成（`GeneratedSoundBanks/<Platform>`）。目前测试用 SDK 自带的
  `samples/IntegrationDemo/Motion/WwiseProject/GeneratedSoundBanks/Windows`。
- 文件 ID（file package）解析没接，返回 `AK_FileNotFound`；ZEngine 现在只用散装 `.bnk`。
- `AudioSourceComponent` / `AudioListenerComponent` 仍然只接内部通路，还没转 Wwise。
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

15 条默认断言（A 组"无假支持" + B 组接线门，源码级、无声卡也能跑），
加 `--with-device` 后 27 条。C 组跳过时会**打印跳过条数**，不是静默零断言。

**未加入默认 CI 矩阵**：C 组需要声卡。A/B 组不需要，这才让它在仓库里有意义。

变异验证（4 个变异，每个都被预期断言捕获）：

| 变异 | 变红 |
| --- | --- |
| `IsCompiledIn()` 恒 false | A1, A4 |
| `eFloorPlane` 改成 XZ | B9 |
| 注册顺序颠倒 | B3 |
| 不注册 bank 基础路径 | C5, C6, C12 |
