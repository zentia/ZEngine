# ZEngine Timeline系统实现文档

## 概述

本文档描述了ZEngine Timeline系统的实现细节。Timeline系统参考了Unity Timeline和Unreal Engine的Sequence系统，提供了一个完整的时间轴编辑和播放框架。

## 系统架构

### 文件结构

```
engine/source/runtime/
├── resource/res_type/
│   ├── data/
│   │   ├── timeline_clip.h          # Timeline片段定义
│   │   ├── timeline_track.h         # Timeline轨道定义
│   │   └── timeline_asset.h          # Timeline资源定义
│   └── components/
│       └── timeline.h                # Timeline组件资源定义
├── function/
│   ├── timeline/
│   │   ├── timeline_director.h       # Timeline导演（播放控制）
│   │   └── timeline_director.cpp
│   └── framework/component/timeline/
│       ├── timeline_component.h      # Timeline组件
│       └── timeline_component.cpp
└── doc/timeline/
    ├── TIMELINE_SYSTEM_GUIDE.md      # 使用指南
    └── TIMELINE_IMPLEMENTATION.md    # 本文档
```

## 核心组件

### 1. TimelineAsset（时间轴资源）

**位置**: `engine/source/runtime/resource/res_type/data/timeline_asset.h`

TimelineAsset是Timeline系统的顶层数据结构，包含：
- 时间轴名称和总时长
- 帧率设置
- 所有轨道的列表

**关键方法**:
- `calculateActualDuration()`: 计算基于所有片段的实际时长
- `getFrameAtTime()` / `getTimeAtFrame()`: 时间与帧数转换

### 2. TimelineTrack（轨道）

**位置**: `engine/source/runtime/resource/res_type/data/timeline_track.h`

轨道是Timeline上的水平行，用于组织片段。支持的轨道类型：
- `AnimationTimelineTrack`: 动画轨道
- `ActivationTimelineTrack`: 激活轨道
- `AudioTimelineTrack`: 音频轨道
- `EventTimelineTrack`: 事件轨道

每个轨道包含：
- 名称、启用状态、锁定状态、静音状态
- 目标GameObject ID
- 片段列表

### 3. TimelineClip（片段）

**位置**: `engine/source/runtime/resource/res_type/data/timeline_clip.h`

片段是轨道上的时间片段。支持的片段类型：
- `AnimationTimelineClip`: 动画片段
- `ActivationTimelineClip`: 激活片段
- `AudioTimelineClip`: 音频片段
- `EventTimelineClip`: 事件片段

每个片段包含：
- 开始时间和持续时间
- 启用状态
- 类型特定的属性

**关键方法**:
- `containsTime()`: 检查时间是否在片段范围内
- `getNormalizedTime()`: 获取片段内的归一化时间（0-1）

### 4. TimelineComponent（组件）

**位置**: `engine/source/runtime/function/framework/component/timeline/`

TimelineComponent是一个可以附加到GameObject上的组件，用于在运行时播放Timeline。

**功能**:
- 加载Timeline资源
- 创建和管理TimelineDirector
- 提供播放控制接口（play/pause/stop）
- 支持自动播放、循环、速度控制

### 5. TimelineDirector（导演）

**位置**: `engine/source/runtime/function/timeline/`

TimelineDirector负责管理Timeline的播放状态和更新逻辑。

**功能**:
- 管理播放状态（Playing/Paused/Stopped）
- 更新当前播放时间
- 更新所有轨道和片段
- 处理循环播放
- 按进入/保持/退出评估片段，事件按经过的时间区间触发（见“片段语义”）

**更新流程**:
1. 每帧调用`tick()`更新当前时间
2. 遍历所有启用的轨道
3. 对每个轨道，检查哪些片段在当前时间范围内
4. 根据片段类型调用相应的更新方法

## 数据流

```
TimelineAsset (资源文件)
    ↓ 加载
TimelineComponent (组件)
    ↓ 初始化
TimelineDirector (导演)
    ↓ 每帧更新
更新轨道和片段
    ↓
影响GameObject状态
```

## 序列化支持

所有Timeline相关的类都使用了ZEngine的反射系统，支持：
- JSON序列化（用于编辑器）
- 二进制序列化（用于运行时）

使用`REFLECTION_TYPE`和`CLASS`宏进行类型注册，使用`META(Enable)`标记可序列化字段。

## 扩展性

### 添加新的轨道类型

1. 在`timeline_track.h`中创建新的轨道类，继承`TimelineTrack`
2. 使用反射系统注册新类型
3. 在`TimelineDirector::updateTrack()`中添加处理逻辑

### 添加新的片段类型

1. 在`timeline_clip.h`中创建新的片段类，继承`TimelineClip`
2. 使用反射系统注册新类型
3. 在`TimelineDirector::updateTrack()`中添加更新逻辑

## 片段语义（2026-10-25 落地，原“待实现功能”四项）

原先四个 `Update*Clip` 都是 TODO（只有事件片段会“标记已触发”但什么也不做）。现在每个片段都按“进入 / 保持 / 退出”
三段评估，窗口为 `[start, start + duration)`。纯数学部分在 `Function/Timeline/TimelineEvaluation.h`（只依赖标准库），
由 `Function/Timeline/Test/TimelineEvaluationTest.cpp` 无头测试（`scripts/verify_timeline_wasm.sh`，97 项断言）。

| 片段 | 进入 | 保持（每帧） | 退出 |
|---|---|---|---|
| 动画 | 解析动画长度（见下） | `AnimationComponent::SetTimelineOverride(path, phase)`：组件按该相位采样，自身状态机/混合状态都不推进 | `ClearTimelineOverride()`，组件从原处继续 |
| 激活 | 记下目标当前激活状态，设为 `m_Active` | — | 恢复进入时记下的状态 |
| 音频 | — | 播放中且本次进入还没尝试过：`AudioSystem::PlayOneShot`（2D） | `Stop(voice)` |
| 事件 | 点事件：播放推进跨过 `m_StartTime` 时触发一次 | | |

细则：
- **激活状态** = `Transform::m_Enabled`（引擎既有约定，`GameObject::Tick` 据此跳过整个对象）。片段自带的
  `m_TargetObjectId` 非 0 时优先于轨道目标。**不会停用 Timeline 自己所在的对象**（停用后它不再 Tick，就永远走不到退出、
  无法自行恢复），遇到时只警告一次。
- **动画覆盖不写序列化数据**：覆盖用组件内部一份临时单片段 `BlendState`，借用组件已有的 slot-0 骨骼映射（没有则借状态机
  当前的）；两者都没有时返回 false，导演只警告一次。编辑器里播 Timeline 不会弄脏场景。
- **动画长度**：Timeline 片段只存路径，`AnimationClip` 只有帧数没有帧率，所以只有当目标组件的 slot-0 正好是同一个
  片段时才用它的长度；否则把一次完整播放拉伸到片段窗口上（窗口按动画长度摆放时恰好播一遍）。非循环片段停在
  最后一帧（相位钳到 1 的前一个浮点数，因为采样器按 Loop 回绕，1.0 会跳回第一帧）。
- **音频**：没有 seek，总是从声音开头播；只在播放状态下开声，所以暂停/停止时拖动（`SetTime`）进入音频片段不出声，
  之后 `Play()` 才开始。Timeline 暂停/恢复会同步暂停/恢复 voice。缺文件只尝试一次。
- **事件**：每个 tick 覆盖一段时间区间，正放 `[from, to)`、倒放 `(to, from]`，非循环的最后一段含端点，循环时 0 与
  `duration` 是两个不同的点。所以每个事件点每次经过恰好触发一次：长帧跳不过、循环每圈触发、相邻 tick 不重复。
  **`SetTime()` 是跳转，不触发事件**。处理器经 `TimelineDirector/TimelineComponent::SetEventHandler` 注入
  （`TimelineEventInfo{name, params, time, target}`，params 原样透传），没有处理器时打 DEBUG 日志。

播放控制的行为修正：
- **循环保留余数**：原先到尾直接置 0，丢掉越过的那一截，每圈漂移最多一帧。
- **非循环播完停在末帧**：原先到尾调 `Stop()` 把时间置回 0，又按 0 时刻重新应用片段（比如 0 时刻的激活片段被再次
  打开）。现在停在 `duration`，末段事件照常触发，结束于末尾的片段正常退出；之后 `Play()` 从头开始。
- **`Stop()` 退出所有片段**（恢复激活、停音频、交还动画）；静音/禁用轨道也会让其上已激活的片段退出。
- 负速度倒放；一帧长于整条 Timeline 时只回绕一次（不在一帧里重放多圈事件）。
- 导演析构只停 voice，不碰 GameObject（通常发生在关卡拆除时，对象不保证还活着）；`TimelineComponent::PostLoadResource`
  重建导演前先 `Stop()` 旧导演，避免重载时把已应用的状态遗留下来。

**验证范围**：`TimelineEvaluation.h` 97/97 无头断言（含两组变异：去掉循环余数 6 项红、循环末端改成开区间 6 项红）；
`TimelineDirector.cpp` / `TimelineComponent.cpp` / `AnimationComponent.cpp` 在真实 web 构建编译通过，并用
`scripts/syntax_check_dx12.py`（Windows Debug|x64 的 include/define）类型检查通过。**导演本体（对象查找、组件调用、
音频）没有运行级测试**——它依赖完整对象系统，需要在编辑器里放一条带四类片段的 Timeline 播一遍确认。

## 性能考虑

1. **片段查找优化**: 当前实现遍历所有片段，对于大量片段可能需要优化（如使用时间索引）
2. **对象查找优化**: 目标GameObject的查找通过Level的ID映射，性能较好
3. **事件触发**: 按 tick 覆盖的时间区间判定（`TimeStep::Crosses`），不需要“已触发”表

## 与现有系统的集成

- **反射系统**: 所有Timeline类都使用反射系统，支持序列化和编辑器集成
- **资源系统**: Timeline资源通过AssetManager加载
- **组件系统**: TimelineComponent继承自Component，可以附加到GameObject
- **Level系统**: TimelineDirector需要Level来查找目标GameObject

## 测试建议

1. **单元测试**:
   - TimelineClip的时间计算
   - TimelineDirector的播放状态管理
   - 片段的时间范围检查

2. **集成测试**:
   - Timeline资源的加载和序列化
   - TimelineComponent的初始化和播放
   - 多轨道同步播放

3. **性能测试**:
   - 大量片段时的性能
   - 长时间播放的内存使用

## 未来改进方向

1. **编辑器支持**: 可视化Timeline编辑器
2. **曲线编辑**: 支持动画曲线和缓动函数
3. **轨道混合**: 支持多个动画轨道的混合
4. **子Timeline**: 支持Timeline嵌套
5. **录制功能**: 支持从运行时录制Timeline
6. **时间缩放**: 支持轨道级别的时间缩放和偏移

