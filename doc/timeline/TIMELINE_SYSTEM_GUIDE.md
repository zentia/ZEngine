# ZEngine Timeline系统使用指南

## 概述

ZEngine的Timeline系统参考了Unity Timeline和Unreal Engine的Sequence系统，提供了一个强大的时间轴编辑和播放框架。Timeline系统允许你创建复杂的动画序列、控制GameObject的激活状态、播放音频、触发事件等。

## 核心概念

### Timeline Asset（时间轴资源）
Timeline Asset是一个可序列化的资源文件，包含了所有轨道和片段的数据。类似于Unity的Timeline Asset或Unreal的Sequence Asset。

### Timeline Track（轨道）
轨道是Timeline上的水平行，用于组织不同类型的片段。每个轨道可以包含多个片段（Clip）。

支持的轨道类型：
- **AnimationTrack**: 动画轨道，用于播放动画
- **ActivationTrack**: 激活轨道，用于控制GameObject的激活状态
- **AudioTrack**: 音频轨道，用于播放音频
- **EventTrack**: 事件轨道，用于触发自定义事件

### Timeline Clip（片段）
片段是轨道上的时间片段，定义了在特定时间段内执行的操作。

支持的片段类型：
- **AnimationTimelineClip**: 动画片段
- **ActivationTimelineClip**: 激活片段
- **AudioTimelineClip**: 音频片段
- **EventTimelineClip**: 事件片段

### Timeline Component（时间轴组件）
TimelineComponent是一个可以附加到GameObject上的组件，用于在运行时播放Timeline。

### Timeline Director（时间轴导演）
TimelineDirector负责管理Timeline的播放状态，更新所有轨道和片段。

## 使用方法

### 1. 创建Timeline资源

在编辑器中创建Timeline资源文件（JSON或二进制格式）：

```json
{
  "$typeName": "TimelineAsset",
  "$context": {
    "m_name": "MyTimeline",
    "m_duration": 10.0,
    "m_frame_rate": 30.0,
    "m_tracks": [
      {
        "$typeName": "AnimationTimelineTrack",
        "$context": {
          "m_name": "Character Animation",
          "m_enabled": true,
          "m_target_object_id": 0,
          "m_clips": [
            {
              "$typeName": "AnimationTimelineClip",
              "$context": {
                "m_start_time": 0.0,
                "m_duration": 5.0,
                "m_enabled": true,
                "m_animation_path": "assets/animations/walk.anim",
                "m_speed": 1.0,
                "m_loop": false
              }
            }
          ]
        }
      },
      {
        "$typeName": "EventTimelineTrack",
        "$context": {
          "m_name": "Events",
          "m_enabled": true,
          "m_clips": [
            {
              "$typeName": "EventTimelineClip",
              "$context": {
                "m_start_time": 2.5,
                "m_duration": 0.0,
                "m_enabled": true,
                "m_event_name": "OnCutsceneStart",
                "m_event_params": "{}"
              }
            }
          ]
        }
      }
    ]
  }
}
```

### 2. 在GameObject上添加TimelineComponent

在GameObject定义中添加TimelineComponent：

```json
{
  "$typeName": "Component",
  "$context": {
    "$typeName": "TimelineComponent",
    "$context": {
      "m_timeline_res": {
        "m_timeline_asset_path": "assets/timelines/my_timeline.json",
        "m_play_on_awake": true,
        "m_loop": false,
        "m_speed": 1.0,
        "m_initial_time": 0.0
      }
    }
  }
}
```

### 3. 运行时控制Timeline

在代码中控制Timeline的播放：

```cpp
// 获取TimelineComponent
auto* timeline_component = game_object->tryGetComponent<TimelineComponent>("TimelineComponent");
if (timeline_component)
{
    // 播放
    timeline_component->play();
    
    // 暂停
    timeline_component->pause();
    
    // 停止
    timeline_component->stop();
    
    // 设置播放时间
    timeline_component->setTime(5.0f);
    
    // 设置播放速度
    timeline_component->setSpeed(2.0f); // 2倍速
    
    // 设置循环
    timeline_component->setLoop(true);
    
    // 获取当前时间
    float current_time = timeline_component->getTime();
    
    // 获取播放状态
    TimelinePlayState state = timeline_component->getPlayState();
}
```

## 轨道和片段详解

### AnimationTrack（动画轨道）

用于播放动画序列。需要目标GameObject有AnimationComponent。

**AnimationTimelineClip属性：**
- `m_animation_path`: 动画资源路径
- `m_speed`: 播放速度倍率（1.0为正常速度）
- `m_loop`: 是否循环播放

### ActivationTrack（激活轨道）

用于控制GameObject的激活/禁用状态。

**ActivationTimelineClip属性：**
- `m_target_object_id`: 目标GameObject的ID（0表示当前GameObject）
- `m_active`: 激活状态（true/false）

### AudioTrack（音频轨道）

用于播放音频。

**AudioTimelineClip属性：**
- `m_audio_path`: 音频资源路径
- `m_volume`: 音量（0.0-1.0）
- `m_loop`: 是否循环播放

### EventTrack（事件轨道）

用于触发自定义事件。

**EventTimelineClip属性：**
- `m_event_name`: 事件名称
- `m_event_params`: 事件参数（JSON格式字符串）

## 高级功能

### 多轨道同步

Timeline支持多个轨道同时播放，所有轨道会根据当前时间同步更新。

### 目标对象绑定

每个轨道可以指定目标GameObject：
- `m_target_object_id = 0`: 使用TimelineComponent所在的GameObject
- `m_target_object_id > 0`: 使用指定ID的GameObject（必须在同一个Level中）

### 时间控制

- **帧率**: Timeline使用帧率来精确控制时间
- **归一化时间**: 片段内部使用0-1的归一化时间，便于处理循环和速度变化
- **时间范围**: 每个片段有开始时间和持续时间，可以精确控制执行时机

## 注意事项

1. **资源加载**: Timeline资源会在TimelineComponent初始化时自动加载
2. **目标对象**: 确保目标GameObject在Timeline播放时存在
3. **性能**: 大量片段可能影响性能，建议合理组织轨道结构
4. **事件触发**: EventTimelineClip在同一时间点只会触发一次，避免重复触发

## 扩展开发

### 添加自定义轨道类型

1. 继承`TimelineTrack`创建新的轨道类
2. 继承`TimelineClip`创建对应的片段类
3. 在`TimelineDirector::updateTrack()`中添加更新逻辑
4. 使用反射系统注册新类型

### 添加自定义片段类型

1. 继承`TimelineClip`创建新的片段类
2. 在`TimelineDirector::updateTrack()`中添加处理逻辑
3. 使用反射系统注册新类型

## 示例场景

### 过场动画（Cutscene）

创建一个包含多个轨道的Timeline：
- 角色动画轨道
- 摄像机动画轨道
- 音频轨道（背景音乐和对话）
- 事件轨道（触发脚本事件）

### 游戏机制序列

使用Timeline控制游戏机制：
- 激活轨道控制门的开关
- 事件轨道触发游戏逻辑
- 动画轨道播放机关动画

## 未来改进

- [ ] 支持曲线编辑（Easing）
- [ ] 支持轨道混合
- [ ] 支持子Timeline嵌套
- [ ] 编辑器可视化界面
- [ ] 支持录制功能
- [ ] 支持时间缩放和偏移

