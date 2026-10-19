#include "Runtime/Function/Audio/AudioSystem.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Audio/AudioDecoder.h"

#include <cstdlib>

namespace
{
    bool EnvFlagIsSet(const char* in_name)
    {
        const char* value = std::getenv(in_name);
        return (value != nullptr) && (value[0] != '\0') && (std::string(value) != "0");
    }
}  // namespace

bool AudioSystem::IsDisabledByEnvironment() const
{
    return EnvFlagIsSet("ZENGINE_AUDIO_DISABLE");
}

bool AudioSystem::Initialize()
{
    if (IsDisabledByEnvironment())
    {
        // Keep the API alive but never touch a device. This is what makes
        // headless CI, the standalone tools and `-batchmode` runs behave the
        // same as the editor without any #ifdefs in game code.
        LOG_INFO(ZAudio, "Audio disabled by ZENGINE_AUDIO_DISABLE; using the null device");
        m_Device = std::make_unique<AudioDeviceNull>();
        m_Device->Open(kDefaultSampleRate, kDefaultChannels, kDefaultBufferFrames, nullptr);
        m_Device->Start();
        return true;
    }

    m_Device = CreateAudioDevice();

    AudioRenderCallback callback = [this](float* out_frames, uint32_t in_frame_count, uint32_t in_channels, uint32_t in_sample_rate) {
        m_Mixer.Render(out_frames, in_frame_count, in_channels, in_sample_rate);
    };

    if (!m_Device->Open(kDefaultSampleRate, kDefaultChannels, kDefaultBufferFrames, callback))
    {
        LOG_WARNING(ZAudio, "Audio device '{}' failed to open; falling back to silence", m_Device->GetBackendName());
        m_Device = std::make_unique<AudioDeviceNull>();
        m_Device->Open(kDefaultSampleRate, kDefaultChannels, kDefaultBufferFrames, callback);
    }

    if (!m_Device->Start())
    {
        LOG_ERROR(ZAudio, "Audio device '{}' failed to start; audio will be silent", m_Device->GetBackendName());
        return true;  // A silent game is better than an aborted startup.
    }

    const char* master_volume = std::getenv("ZENGINE_AUDIO_MASTER_VOLUME");
    if (master_volume != nullptr && master_volume[0] != '\0')
    {
        const float parsed = std::strtof(master_volume, nullptr);
        if (parsed >= 0.0f)
        {
            SetMasterVolume(parsed);
        }
    }

    LOG_INFO(ZAudio,
             "AudioSystem ready: backend {}, {} Hz, {} channel(s), master volume {:.2f}",
             m_Device->GetBackendName(),
             m_Device->GetSampleRate(),
             m_Device->GetChannels(),
             GetMasterVolume());
    return true;
}

void AudioSystem::Shutdown()
{
    // Stop voices before clips go away: a live voice holds a raw clip pointer.
    m_Mixer.StopAll();

    if (m_Device != nullptr)
    {
        m_Device->Stop();
        m_Device->Close();
        m_Device.reset();
    }

    UnloadAllClips();
    LOG_INFO(ZAudio, "AudioSystem shut down");
}

const AudioClipData* AudioSystem::GetClip(const std::filesystem::path& in_path)
{
    if (in_path.empty())
    {
        return nullptr;
    }

    const std::string key = in_path.generic_string();

    std::lock_guard<std::mutex> lock(m_ClipMutex);

    auto existing = m_Clips.find(key);
    if (existing != m_Clips.end())
    {
        return existing->second.get();
    }

    auto clip = std::make_unique<AudioClipData>();
    std::string error;
    if (!DecodeAudioFile(in_path.string(), *clip, &error))
    {
        LOG_ERROR(ZAudio, "Cannot load clip '{}': {}", key, error);
        return nullptr;
    }

    LOG_INFO(ZAudio,
             "Loaded clip '{}': {} Hz, {} channel(s), {:.2f}s",
             key,
             clip->m_SampleRate,
             clip->m_Channels,
             clip->GetDuration());

    AudioClipData* result = clip.get();
    m_Clips.emplace(key, std::move(clip));
    return result;
}

const AudioClipData* AudioSystem::FindClip(const std::filesystem::path& in_path) const
{
    if (in_path.empty())
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_ClipMutex);

    const auto found = m_Clips.find(in_path.generic_string());
    return (found != m_Clips.end()) ? found->second.get() : nullptr;
}

void AudioSystem::UnloadAllClips()
{
    std::lock_guard<std::mutex> lock(m_ClipMutex);
    m_Clips.clear();
}

AudioVoiceId AudioSystem::Play(const AudioClipData* in_clip, const AudioPlayParams& in_params)
{
    if (in_clip == nullptr)
    {
        return kInvalidAudioVoiceId;
    }
    return m_Mixer.Play(in_clip, in_params);
}

AudioVoiceId AudioSystem::PlayOneShot(const std::filesystem::path& in_path, const AudioPlayParams& in_params)
{
    const AudioClipData* clip = GetClip(in_path);
    if (clip == nullptr)
    {
        return kInvalidAudioVoiceId;
    }
    return m_Mixer.Play(clip, in_params);
}

void AudioSystem::Stop(AudioVoiceId in_voice, float in_fade_out_seconds)
{
    m_Mixer.Stop(in_voice, in_fade_out_seconds);
}

void AudioSystem::StopAll(float in_fade_out_seconds)
{
    m_Mixer.StopAll(in_fade_out_seconds);
}

bool AudioSystem::SetPaused(AudioVoiceId in_voice, bool in_paused)
{
    return m_Mixer.SetPaused(in_voice, in_paused);
}

bool AudioSystem::IsPlaying(AudioVoiceId in_voice) const
{
    return m_Mixer.IsPlaying(in_voice);
}

bool AudioSystem::UpdateVoice(AudioVoiceId in_voice, const AudioPlayParams& in_params)
{
    return m_Mixer.UpdateVoiceParams(in_voice, in_params);
}

void AudioSystem::SetListener(const AudioListenerData& in_listener)
{
    m_Mixer.SetListener(in_listener);
}

AudioListenerData AudioSystem::GetListener() const
{
    return m_Mixer.GetListener();
}

AudioVec3 AudioSystem::ToAudioVec3(const Vector3& in_vector)
{
    return AudioVec3(in_vector.x, in_vector.y, in_vector.z);
}

void AudioSystem::SetListener(const Vector3& in_position, const Vector3& in_forward, const Vector3& in_up)
{
    AudioListenerData listener;
    listener.m_Position = ToAudioVec3(in_position);
    listener.m_Forward = ToAudioVec3(in_forward).Normalised();
    listener.m_Up = ToAudioVec3(in_up).Normalised();
    m_Mixer.SetListener(listener);
}

AudioBusId AudioSystem::CreateBus(const std::string& in_name)
{
    return m_Mixer.CreateBus(in_name);
}

AudioBusId AudioSystem::FindBus(const std::string& in_name) const
{
    return m_Mixer.FindBus(in_name);
}

void AudioSystem::SetBusVolume(AudioBusId in_bus, float in_volume)
{
    m_Mixer.SetBusVolume(in_bus, in_volume);
}

float AudioSystem::GetBusVolume(AudioBusId in_bus) const
{
    return m_Mixer.GetBusVolume(in_bus);
}

void AudioSystem::SetBusPaused(AudioBusId in_bus, bool in_paused)
{
    m_Mixer.SetBusPaused(in_bus, in_paused);
}

void AudioSystem::SetMasterVolume(float in_volume)
{
    m_Mixer.SetMasterVolume(in_volume);
}

float AudioSystem::GetMasterVolume() const
{
    return m_Mixer.GetMasterVolume();
}

void AudioSystem::SetMasterPaused(bool in_paused)
{
    m_Mixer.SetMasterPaused(in_paused);
}

bool AudioSystem::IsDeviceRunning() const
{
    return (m_Device != nullptr) && m_Device->IsRunning();
}

const char* AudioSystem::GetBackendName() const
{
    return (m_Device != nullptr) ? m_Device->GetBackendName() : "None";
}

uint32_t AudioSystem::GetDeviceSampleRate() const
{
    return (m_Device != nullptr) ? m_Device->GetSampleRate() : 0u;
}

uint32_t AudioSystem::GetDeviceChannels() const
{
    return (m_Device != nullptr) ? m_Device->GetChannels() : 0u;
}
