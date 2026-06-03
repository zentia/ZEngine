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
- 处理事件触发（避免重复触发）

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

## 待实现功能

以下功能在代码中标记为TODO，需要后续实现：

1. **动画片段更新** (`TimelineDirector::updateAnimationClip`)
   - 需要集成动画系统，加载动画资源
   - 设置动画组件的播放状态和时间

2. **激活片段更新** (`TimelineDirector::updateActivationClip`)
   - 需要实现GameObject的激活/禁用接口
   - 可能需要通过组件来实现

3. **音频片段更新** (`TimelineDirector::updateAudioClip`)
   - 需要音频系统的支持
   - 加载和播放音频资源

4. **事件系统** (`TimelineDirector::updateEventClip`)
   - 需要实现事件系统或回调机制
   - 解析事件参数并触发相应逻辑

## 性能考虑

1. **片段查找优化**: 当前实现遍历所有片段，对于大量片段可能需要优化（如使用时间索引）
2. **对象查找优化**: 目标GameObject的查找通过Level的ID映射，性能较好
3. **事件触发**: 使用map跟踪已触发事件，避免重复触发

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

