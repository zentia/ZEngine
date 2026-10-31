#pragma once

#include "Runtime/Function/Audio/AudioCommon.h"
// Wwise-free by design (POD-only boundary), so including it here costs nothing
// in a build configured without the SDK -- but the constants it exposes
// (kInvalidPlayingId / kInvalidGameObject) are the only correct "nothing is
// playing" values for the Wwise route, and inventing local ones is how a
// component ends up comparing against a value Wwise can actually return.
#include "Runtime/Function/Audio/Wwise/WwiseAudioSystem.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "AudioSourceComponent.generated.h"

namespace Runtime
{
    /// Plays an AudioClip in the world. Unity's AudioSource analogue.
    ///
    /// The component owns exactly one voice at a time. Calling Play() while a
    /// voice is live stops the previous one -- an AudioSource is a single
    /// emitter, and stacking voices behind one component makes "is it playing?"
    /// unanswerable. Fire-and-forget one-shots should call
    /// AudioSystem::PlayOneShot() directly instead.
    ZCLASS()
    class AudioSourceComponent : public Component
    {
        ZGENERATED_BODY(AudioSourceComponent)

    public:
        AudioSourceComponent() = default;
        ~AudioSourceComponent() override;

        void PostLoadResource(GameObject* parent_object) override;
        void Tick(float delta_time) override;

        // AudioVoiceId is uint64_t, so it crosses the boundary as a JS number.
        ZFUNCTION()
        AudioVoiceId Play();
        ZFUNCTION()
        void Stop(float in_fade_out_seconds = 0.0f);
        ZFUNCTION()
        void SetPaused(bool in_paused);
        ZFUNCTION()
        bool IsPlaying() const;

        ZFUNCTION()
        AudioVoiceId GetVoiceId() const { return m_VoiceId; }

        // ---- Unity 的 AudioSource 里本引擎缺的那些 ------------------------
        //
        // 补全前这个组件只有 5 个 ZFUNCTION（Play / Stop / SetPaused /
        // IsPlaying / GetVoiceId），而 volume / pitch / loop / spatialBlend /
        // minDistance / maxDistance / priority / playOnAwake 是靠 ZPROPERTY 自动
        // 导出的字段 —— 字段有了，但"播到哪了""延迟播放""静音""在别处放一个一
        // 次性音效"这些**动作**一个都没有。

        /// 延迟 delay_seconds 秒后开始（Unity 的 `PlayDelayed`）。
        ///
        /// 与 `Play()` 一样会先停掉当前这条 voice。延迟期间 `IsPlaying()` 是
        /// **true**（声音已经排上了），但 `GetTime()` 是 0 —— 还没开始播。
        ZFUNCTION()
        void PlayDelayed(float delay_seconds);

        /// 暂停 / 继续。`Pause()` 保留播放头，`Stop()` 不保留。
        ZFUNCTION()
        void Pause();
        ZFUNCTION()
        void UnPause();
        ZFUNCTION()
        bool IsPaused() const;

        /// **不占用本组件的 voice** 地放一个一次性音效（Unity 的
        /// `AudioSource.PlayOneShot`）。
        ///
        /// 这是它和 `Play()` 唯一但关键的区别：`Play()` 会让本组件"正在播的就是
        /// 这条"，`PlayOneShot` 不会 —— 可以在背景音乐上叠脚步声。
        /// 返回新 voice 的 id（拿不到素材时是 0）。
        ZFUNCTION()
        AudioVoiceId PlayOneShot(const eastl::string& clip_path, float volume_scale);

        /// 已播放的秒数（Unity 的 `time`）。
        ///
        /// ⚠️ 进度由 `AudioMixer::Render()` 推进 —— **没有设备在跑就恒为 0**（无头
        /// / null device）。不是 bug：进度是"渲染到第几帧"的结果。
        ZFUNCTION()
        float GetTime() const;

        /// 跳到指定秒数（Unity 的 `time` 的写端）。超出素材长度夹到末尾。
        ZFUNCTION()
        void SetTime(float time_seconds);

        /// 已播放的**源采样帧**数（Unity 的 `timeSamples`）。
        ZFUNCTION()
        int GetTimeSamples() const;

        /// 当前 clip 的时长（秒），已折算 `pitch`。没有 clip 时是 0。
        ZFUNCTION()
        float GetClipLength() const;

        ZFUNCTION()
        eastl::string GetClipPath() const { return m_ClipPath; }
        ZFUNCTION()
        void SetClipPath(const eastl::string& clip_path) { m_ClipPath = clip_path; }

    private:
        /// Builds AudioPlayParams from the serialized fields + current transform.
        AudioPlayParams BuildPlayParams() const;

        /// Registers this component's emitter with Wwise on first use and returns
        /// its id, or kInvalidGameObject (after logging) when it cannot -- an
        /// object with no allocated id has no stable identity to be an emitter.
        /// Idempotent: the id is cached in m_WwiseGameObject.
        uint64_t ResolveWwiseEmitter();

        /// `Play()` / `PlayDelayed()` 共用的实现。`Play()` 就是延迟 0。
        AudioVoiceId PlayWithDelay(float in_delay_seconds);

        /// Pushes the current world position/orientation to Wwise. Wwise does not
        /// read the scene graph; without this a moving emitter stays at the
        /// position it was posted from, which sounds like the sound was attached
        /// to a wall.
        void PushWwiseEmitterTransform(uint64_t in_game_object) const;

        ZPROPERTY(meta=(DisplayName="Audio Clip", Tooltip="Asset path of the clip to play."))
        eastl::string m_ClipPath;
        /// Wwise event name, used only when the Wwise backend owns the output.
        ///
        /// It is a separate field -- never derived from m_ClipPath -- because the
        /// two backends address content in incompatible ways: the internal one
        /// decodes a file, Wwise plays a named event out of a loaded sound bank.
        /// There is no spelling of one in terms of the other, so guessing the
        /// event from the file name would be a guess that fails silently.
        ZPROPERTY(meta=(DisplayName="Wwise Event", Tooltip="Wwise event name. Used only when the Wwise backend owns the audio output; ignored by the internal backend."))
        eastl::string m_WwiseEvent;
        /// Empty = master bus. Resolved by name on every Play().
        ZPROPERTY(meta=(DisplayName="Output Bus", Tooltip="Mixer bus name; empty routes to the master bus."))
        eastl::string m_BusName;
        ZPROPERTY(meta=(ClampMin=0, ClampMax=1))
        float m_Volume {1.0f};
        /// 静音（Unity 的 `mute`）。
        ///
        /// 与"把 volume 调成 0"的区别是**它不改变 volume**：取消静音后音量回到
        /// 原来的值，而调成 0 之后原来的值就丢了。脚本里"临时静音再恢复"只能用
        /// 前者。
        ZPROPERTY(meta=(Tooltip="Silences the source without changing its volume."))
        bool m_Mute {false};
        ZPROPERTY(meta=(Tooltip="Playback rate multiplier (1 = original pitch)."))
        float m_Pitch {1.0f};
        ZPROPERTY()
        bool m_Loop {false};
        ZPROPERTY()
        bool m_PlayOnAwake {false};
        /// 0 = 2D, 1 = fully spatial.
        ZPROPERTY(meta=(Category="3D Sound", Tooltip="0 = 2D, 1 = fully spatial.", ClampMin=0, ClampMax=1))
        float m_SpatialBlend {1.0f};
        ZPROPERTY(meta=(Category="3D Sound", Tooltip="Distance below which the sound is not attenuated.", ClampMin=0))
        float m_MinDistance {1.0f};
        ZPROPERTY(meta=(Category="3D Sound", Tooltip="Distance beyond which the sound stops attenuating further.", ClampMin=0))
        float m_MaxDistance {50.0f};
        ZPROPERTY(meta=(DisplayName="Fade In (s)", ClampMin=0))
        float m_FadeInSeconds {0.0f};
        ZPROPERTY()
        int32_t m_Priority {0};

        /// Not serialized: a voice id is only meaningful for this process.
        AudioVoiceId m_VoiceId {kInvalidAudioVoiceId};

        /// Wwise-side handle for the same one-voice-per-source rule. Not
        /// serialized: a playing id is only meaningful for this process.
        uint64_t m_WwisePlayingId {WwiseAudioSystem::kInvalidPlayingId};
        /// The emitter Wwise knows this component by. Derived from the owning
        /// GameObject's id, offset so it can never collide with the reserved
        /// default-listener id -- a source that claimed the listener's id would
        /// overwrite the listener transform every frame and turn all 3D audio
        /// into whatever the last emitter happened to be.
        uint64_t m_WwiseGameObject {WwiseAudioSystem::kInvalidGameObject};

        /// Wwise 路线的 `PlayDelayed` 倒计时。
        ///
        /// 内部后端把延迟交给 mixer（见 `AudioPlayParams::m_DelaySeconds`），而
        /// Wwise 的 event 一旦 post 出去就立刻响，没有"延迟参数" —— 只能由组件
        /// 在 `Tick` 里数完再 post。两条路线的等待逻辑不同源，所以分开存。
        float m_WwisePendingDelay {0.f};
        bool m_HasWwisePendingDelay {false};
        /// Wwise 侧**没有**"查暂停"的接口，所以组件自己记最后一次 SetPaused 的
        /// 意图，`IsPaused()` 才能回答。
        bool m_WwisePaused {false};
    };
}  // namespace Runtime
