#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Audio/AudioCommon.h"
#include "Runtime/Function/Audio/AudioDevice.h"
#include "Runtime/Function/Audio/AudioMixer.h"
#include "Runtime/Function/Audio/Wwise/WwiseAudioSystem.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// =============================================================================
// AudioSystem: the engine-facing owner of clips, voices and the output device.
//
// Layering (mirrors Unreal's FAudioDevice + Unity's AudioSettings split):
//
//   AudioClipData   -- decoded PCM, cached here for the process lifetime
//   AudioMixer      -- voices / buses / master, pure DSP, device-agnostic
//   IAudioDevice    -- platform output (WASAPI today, Null elsewhere)
//
// Clips are cached forever on purpose: a voice only stores a raw clip pointer,
// so evicting a clip while it is playing would be a use-after-free. Projects
// that need memory back can call UnloadAllClips() after StopAll().
//
// Headless escape hatch: ZENGINE_AUDIO_DISABLE=1 keeps every API working (so
// game code needs no #ifdefs) while the device stays silent. CI depends on this.
// =============================================================================

class AudioSystem : public IEngineSystem
{
public:
    std::string GetName() const override { return "AudioSystem"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Gameplay; }

    // ---- clips ----
    /// Load (and cache) a clip. Returns nullptr when the file is missing or the
    /// format is unsupported; the failure is logged under the ZAudio category.
    const AudioClipData* GetClip(const std::filesystem::path& in_path);

    /// Cache lookup without decoding. nullptr when not loaded yet.
    const AudioClipData* FindClip(const std::filesystem::path& in_path) const;

    /// Drop every cached clip. Caller must have stopped all voices first.
    void UnloadAllClips();

    // ---- playback ----
    AudioVoiceId Play(const AudioClipData* in_clip, const AudioPlayParams& in_params);
    AudioVoiceId PlayOneShot(const std::filesystem::path& in_path, const AudioPlayParams& in_params);

    void Stop(AudioVoiceId in_voice, float in_fade_out_seconds = 0.0f);
    void StopAll(float in_fade_out_seconds = 0.0f);
    bool SetPaused(AudioVoiceId in_voice, bool in_paused);
    bool IsPlaying(AudioVoiceId in_voice) const;
    /// Push a new emitter state (position / volume / pitch) into a live voice.
    bool UpdateVoice(AudioVoiceId in_voice, const AudioPlayParams& in_params);

    // ---- listener ----
    void SetListener(const AudioListenerData& in_listener);
    AudioListenerData GetListener() const;
    /// Convenience overload for engine math types (Z-up: forward ~ -Y, up ~ +Z).
    void SetListener(const Vector3& in_position, const Vector3& in_forward, const Vector3& in_up);
    static AudioVec3 ToAudioVec3(const Vector3& in_vector);

    // ---- buses / master ----
    AudioBusId CreateBus(const std::string& in_name);
    AudioBusId FindBus(const std::string& in_name) const;
    void SetBusVolume(AudioBusId in_bus, float in_volume);
    float GetBusVolume(AudioBusId in_bus) const;
    void SetBusPaused(AudioBusId in_bus, bool in_paused);
    void SetMasterVolume(float in_volume);
    float GetMasterVolume() const;
    void SetMasterPaused(bool in_paused);

    // ---- introspection ----
    AudioMixer& GetMixer() { return m_Mixer; }
    bool IsDeviceRunning() const;
    /// Reports what is *audibly* playing, which is not always m_Device: when the
    /// Wwise backend is active this returns the Wwise name even though the
    /// device here is the null one.
    const char* GetBackendName() const;
    /// True when WwiseAudioSystem owns the output. File-clip playback through
    /// Play()/PlayOneShot() is unavailable in that mode -- use
    /// WwiseAudioSystem::PostEvent() instead.
    bool IsWwiseActive() const;
    uint32_t GetDeviceSampleRate() const;
    uint32_t GetDeviceChannels() const;

protected:
    bool Initialize() override;
    void Shutdown() override;

private:
    /// Device sample rate / channel / latency budget for Initialize().
    static constexpr uint32_t kDefaultSampleRate = 48000u;
    static constexpr uint32_t kDefaultChannels = 2u;
    /// ~10 ms at 48 kHz: the usual compromise between xrun safety and latency.
    static constexpr uint32_t kDefaultBufferFrames = 480u;

    bool IsDisabledByEnvironment() const;
    /// Asks WwiseAudioSystem whether it initialised and therefore owns the
    /// speakers. Called once from Initialize(); the result is cached in
    /// m_WwiseOwnsOutput so hot paths branch on a plain bool.
    bool QueryWwiseOwnsOutput() const;
    /// Returns false (after logging once) when file-clip playback would be
    /// silent because Wwise owns the output.
    bool WarnOrProceedForFilePlayback(const char* in_api_name) const;

    AudioMixer m_Mixer;
    std::unique_ptr<IAudioDevice> m_Device;
    bool m_WwiseOwnsOutput {false};
    mutable bool m_WarnedWwiseFilePlayback {false};

    mutable std::mutex m_ClipMutex;
    /// Key: std::filesystem::path::generic_string() (forward slashes, stable).
    std::unordered_map<std::string, std::unique_ptr<AudioClipData>> m_Clips;
};
