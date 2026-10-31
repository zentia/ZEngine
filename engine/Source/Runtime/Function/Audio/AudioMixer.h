#pragma once

#include "Runtime/Function/Audio/AudioCommon.h"

#include <mutex>
#include <string>
#include <vector>

// =============================================================================
// Software mixer: voices -> buses -> master -> device buffer.
//
// Threading contract
// ------------------
// Render() is called from the platform audio device thread (WASAPI's render
// thread today). Every public entry point therefore takes m_Mutex. The critical
// section is short (a handful of voices, no allocation, no I/O, no logging),
// which is what makes a plain mutex acceptable here instead of a lock-free
// command queue: the alternative would be a triple-buffered voice snapshot that
// is much harder to keep correct with voice stealing and pause/resume.
//
// Clip lifetime
// -------------
// A voice stores a raw pointer to its AudioClipData. The owner (AudioSystem)
// keeps decoded clips alive for the lifetime of the process, so a voice can
// never outlive its clip. Do not feed this class a stack/transient clip.
// =============================================================================

class AudioMixer
{
public:
    /// `GetVoicePlaybackTime` / `GetVoiceDuration` 找不到 voice 时的返回值。
    ///
    /// 用 -1 而不是 0：**0 是一个合法的播放位置**（刚开头），拿它当"没有这条
    /// voice"会让"还没开始"和"已经在播"分不开。
    static constexpr double kUnknownPlaybackTime = -1.0;

    AudioMixer();

    // ---- buses ----
    AudioBusId CreateBus(const std::string& in_name);
    AudioBusId FindBus(const std::string& in_name) const;
    void SetBusVolume(AudioBusId in_bus, float in_volume);
    float GetBusVolume(AudioBusId in_bus) const;
    /// Suspend a whole bus (e.g. pause menu killing SFX but not music).
    void SetBusPaused(AudioBusId in_bus, bool in_paused);

    void SetMasterVolume(float in_volume);
    float GetMasterVolume() const { return m_MasterVolume; }
    void SetMasterPaused(bool in_paused);
    bool IsMasterPaused() const { return m_MasterPaused; }

    // ---- listener ----
    void SetListener(const AudioListenerData& in_listener);
    AudioListenerData GetListener() const;

    // ---- voices ----
    /// Starts a voice. Returns kInvalidAudioVoiceId when the clip is invalid.
    /// When the voice cap is reached, the lowest-priority (then oldest) voice is
    /// stopped to make room instead of dropping the new sound.
    AudioVoiceId Play(const AudioClipData* in_clip, const AudioPlayParams& in_params);

    /// Immediate stop, or a fade-out if in_fade_out_seconds > 0.
    void Stop(AudioVoiceId in_voice, float in_fade_out_seconds = 0.0f);
    void StopAll(float in_fade_out_seconds = 0.0f);

    bool SetPaused(AudioVoiceId in_voice, bool in_paused);
    AudioVoiceState GetVoiceState(AudioVoiceId in_voice) const;
    bool IsPlaying(AudioVoiceId in_voice) const { return GetVoiceState(in_voice) == AudioVoiceState::Playing; }

    /// Push updated emitter state (position, volume, pitch) into a live voice.
    /// The clip pointer cannot be swapped -- start a new voice instead.
    bool UpdateVoiceParams(AudioVoiceId in_voice, const AudioPlayParams& in_params);

    // ---- 播放进度（Unity 的 `AudioSource.time` / `timeSamples`）----------
    //
    // 进度只在 `Render()` 里推进，所以**没有设备在跑就没有进度**。这不是缺陷：
    // 进度本来就是"渲染到第几帧"的结果，凭空算一个时间出来反而会和实际听到的
    // 对不上。

    /// 已播放的秒数。找不到这条 voice 时返回 -1（`kUnknownPlaybackTime`）。
    double GetVoicePlaybackTime(AudioVoiceId in_voice) const;

    /// 已播放的**源**帧数（不受输出采样率影响）。找不到时返回 0。
    uint64_t GetVoicePlaybackFrames(AudioVoiceId in_voice) const;

    /// 这条 voice 的素材时长（秒）；考虑 `m_Pitch`（倍速会缩短听感时长）。
    /// 找不到时返回 -1。
    double GetVoiceDuration(AudioVoiceId in_voice) const;

    /// 跳到指定秒数。超出素材长度时夹到末尾，负数夹到 0。找不到时返回 false。
    bool SetVoicePlaybackTime(AudioVoiceId in_voice, double in_seconds);

    bool IsVoicePaused(AudioVoiceId in_voice) const;

    uint32_t GetActiveVoiceCount() const;

    // ---- rendering (audio device thread) ----
    /// Fills out_frames with in_frame_count interleaved frames at in_channels
    /// channels. Silence is written when nothing is playing, so the caller never
    /// has to zero the buffer itself.
    void Render(float* out_frames, uint32_t in_frame_count, uint32_t in_channels, uint32_t in_sample_rate);

private:
    struct Voice
    {
        AudioVoiceId m_Id {kInvalidAudioVoiceId};
        const AudioClipData* m_Clip {nullptr};
        AudioPlayParams m_Params;
        /// Playhead in *source* frames (fractional).
        double m_Playhead {0.0};
        AudioVoiceState m_State {AudioVoiceState::Playing};
        float m_FadeGain {1.0f};
        float m_FadeTarget {1.0f};
        /// Duration of the fade currently in flight (in or out), seconds.
        float m_FadeSeconds {0.0f};
        /// Fade delta per rendered output frame; derived from m_FadeSeconds and
        /// the device sample rate on the first render after it is set.
        float m_FadeRate {0.0f};
        /// Remove the voice once the fade-out reaches silence.
        bool m_RemoveWhenFaded {false};
        /// PlayDelayed 剩下的等待秒数。只在 Play() 时从 params 读一次。
        float m_DelayRemaining {0.0f};
    };

    struct Bus
    {
        std::string m_Name;
        float m_Volume {1.0f};
        bool m_Paused {false};
    };

    Voice* FindVoiceLocked(AudioVoiceId in_voice);
    const Voice* FindVoiceLocked(AudioVoiceId in_voice) const;
    /// 播放头换算成"素材秒数"：源帧数 / 源采样率。播放头是**源**帧，不随输出
    /// 采样率变，所以这里只能用素材自己的采样率。
    double VoicePlaybackSecondsLocked(const Voice& in_voice) const;
    uint32_t BusIndexLocked(AudioBusId in_bus) const;

    mutable std::mutex m_Mutex;
    std::vector<Voice> m_Voices;
    std::vector<Bus> m_Buses;
    AudioListenerData m_Listener;
    AudioVoiceId m_NextVoiceId {1u};
    float m_MasterVolume {1.0f};
    bool m_MasterPaused {false};
};
