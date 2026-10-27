#pragma once

// ============================================================================
// UWidgetAnimation — 命名属性轨道动画（UE FWidgetAnimation / UWidgetAnimation 对应物）
// ----------------------------------------------------------------------------
// 状态机层：持有若干条命名轨道（property_path → AnimTrack），自己推进时间，每帧把
// 归一化进度 lerp 喂给每条轨道 Evaluate，再按 property_path 写进目标 RenderTransform。
// 纯插值/路径分派在 UMGAnimation.h（可 headless 测），时间推进在这里用一个极小的
// 归一化进度状态机实现（Play/Pause/Resume/PlayReverse/Stop/loop），不依赖 ZSlate 的
// FCurveSequence——那颗头的成员函数前向声明顺序在 MSVC 单独包含时会报 C2027，且本层
// 只需要"进度随时间线性推进 + 可循环/反向"这一小撮语义，自持反而更轻、可 headless 测。
//
// 用法（设计器/代码两侧一致）：
//   UWidgetAnimation fade;
//   fade.AddTrack("render_opacity", 0.0f, 1.0f, anim_core::Easing::QuadOut);
//   fade.Play(0.25f /*seconds*/, /*loop=*/false);
//   ...
//   // 每帧（UWidget::Tick 已在逐帧调用链里）：
//   fade.Tick(delta_time, widget.GetRenderTransform());  // 就地写目标 widget 的变换
//
// 目标 RenderTransform 由调用方传入（通常就是某个 UWidget 持有的那一份），本类不持有
// widget 引用，避免生命周期耦合；Tick 返回是否仍在播放。
// ============================================================================

#include "Runtime/UMG/Animation/UMGAnimation.h"

#include <cmath>
#include <string>
#include <unordered_map>

namespace ZUMG
{
class UWidgetAnimation
{
public:
    // 添加/替换一条命名轨道。同名轨道会被覆盖（便于设计器改曲线后重放）。
    void AddTrack(const std::string& property_path, float start, float end,
                  anim_core::Easing easing = anim_core::Easing::Linear)
    {
        anim_core::AnimTrack track;
        track.StartValue = start;
        track.EndValue = end;
        track.Easing_ = easing;
        m_Tracks[property_path] = track;
    }

    void RemoveTrack(const std::string& property_path) { m_Tracks.erase(property_path); }
    void ClearTracks() { m_Tracks.clear(); }
    size_t GetTrackCount() const { return m_Tracks.size(); }

    // 播放。duration<=0 视为瞬时：标记已结束，Tick 时一次性写到终点。
    void Play(float duration, bool loop = false)
    {
        m_Duration = (duration > 0.0f) ? duration : 0.0f;
        m_Loop = loop;
        m_Reverse = false;
        m_Position = 0.0f;      // 归一化进度 0..1
        m_Playing = (m_Duration > 0.0f);
        m_Finished = (m_Duration <= 0.0f);
    }

    // 反向播放：从终点(1)退回起点(0)。
    void PlayReverse(float duration)
    {
        Play(duration);
        m_Reverse = true;
        m_Position = 1.0f;
        m_Playing = (m_Duration > 0.0f);
        m_Finished = (m_Duration <= 0.0f);
    }

    void Pause()  { m_Playing = false; }
    void Resume() { if (!m_Finished) m_Playing = true; }
    void Stop()   { m_Playing = false; m_Position = m_Reverse ? 1.0f : 0.0f; m_Finished = false; }

    bool IsPlaying() const { return m_Playing; }
    bool IsFinished() const { return m_Finished; }
    float GetLerp() const { return m_Position; }

    // 推进一帧：把归一化进度喂给每条轨道，按 property_path 写进 target。
    // 返回 true 表示仍在播放（调用方应继续每帧 Tick）；false 表示已结束/暂停。
    bool Tick(float delta_time, anim_core::RenderTransform& target)
    {
        if (m_Duration <= 0.0f)
        {
            // 瞬时动画：一次性写到终点（reverse 则是起点）。
            ApplyLerp(m_Reverse ? 0.0f : 1.0f, target);
            return false;
        }
        if (!m_Playing)
            return false;

        const float deltaNorm = delta_time / m_Duration;
        m_Position += m_Reverse ? -deltaNorm : deltaNorm;

        if (m_Loop)
        {
            // 循环：回绕到 [0,1)。
            m_Position = std::fmod(m_Position, 1.0f);
            if (m_Position < 0.0f)
                m_Position += 1.0f;
        }
        else
        {
            if (!m_Reverse && m_Position >= 1.0f)
            {
                m_Position = 1.0f;
                m_Playing = false;
                m_Finished = true;
            }
            else if (m_Reverse && m_Position <= 0.0f)
            {
                m_Position = 0.0f;
                m_Playing = false;
                m_Finished = true;
            }
        }

        ApplyLerp(m_Position, target);
        return m_Playing;
    }

    // 不推进时间，直接按当前进度写入（供设计器拖动时间轴预览）。
    void Evaluate(anim_core::RenderTransform& target) const { ApplyLerp(m_Position, target); }

private:
    void ApplyLerp(float lerp, anim_core::RenderTransform& target) const
    {
        for (const auto& kv : m_Tracks)
        {
            target.ApplyToSlot(kv.first.c_str(), kv.second.Evaluate(lerp));
        }
    }

    std::unordered_map<std::string, anim_core::AnimTrack> m_Tracks;
    float m_Duration {1.0f};     // seconds (>0)
    float m_Position {0.0f};     // normalized progress 0..1
    bool  m_Playing {false};
    bool  m_Loop {false};
    bool  m_Reverse {false};
    bool  m_Finished {false};
};
}  // namespace ZUMG
