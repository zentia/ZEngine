#include "Runtime/Function/Audio/AudioMixer.h"

#include <algorithm>
#include <cmath>

namespace
{
    /// Unity-style distance attenuation, clamped between min and max distance.
    float DistanceAttenuation(float in_distance, float in_min_distance, float in_max_distance, AudioDistanceModel in_model)
    {
        const float min_distance = std::max(in_min_distance, 0.0001f);
        const float max_distance = std::max(in_max_distance, min_distance + 0.0001f);
        const float distance = std::min(std::max(in_distance, min_distance), max_distance);

        switch (in_model)
        {
            case AudioDistanceModel::Linear:
                return 1.0f - (distance - min_distance) / (max_distance - min_distance);
            case AudioDistanceModel::InverseSquare:
            {
                const float ratio = min_distance / distance;
                return ratio * ratio;
            }
            case AudioDistanceModel::Inverse:
            default:
                return min_distance / distance;
        }
    }

    /// Equal-power pan. in_pan: -1 = hard left, 0 = centre, +1 = hard right.
    void PanGains(float in_pan, float& out_left, float& out_right)
    {
        const float angle = (in_pan + 1.0f) * 0.25f * 3.14159265358979323846f;
        out_left = std::cos(angle);
        out_right = std::sin(angle);
    }

    /// Soft knee so a wall of overlapping voices turns into warmth, not clipping.
    float SoftClip(float in_sample)
    {
        if (in_sample > 1.0f || in_sample < -1.0f)
        {
            return std::tanh(in_sample);
        }
        return in_sample;
    }
}  // namespace

AudioMixer::AudioMixer()
{
    // Bus 0 is the master bus and must always exist at index 0.
    m_Buses.push_back(Bus {"Master", 1.0f, false});
}

AudioBusId AudioMixer::CreateBus(const std::string& in_name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (uint32_t index = 0u; index < static_cast<uint32_t>(m_Buses.size()); ++index)
    {
        if (m_Buses[index].m_Name == in_name)
        {
            return index;
        }
    }

    m_Buses.push_back(Bus {in_name, 1.0f, false});
    return static_cast<AudioBusId>(m_Buses.size() - 1u);
}

AudioBusId AudioMixer::FindBus(const std::string& in_name) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (uint32_t index = 0u; index < static_cast<uint32_t>(m_Buses.size()); ++index)
    {
        if (m_Buses[index].m_Name == in_name)
        {
            return index;
        }
    }
    return kInvalidAudioBusId;
}

uint32_t AudioMixer::BusIndexLocked(AudioBusId in_bus) const
{
    return (in_bus < m_Buses.size()) ? in_bus : 0u;
}

void AudioMixer::SetBusVolume(AudioBusId in_bus, float in_volume)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Buses[BusIndexLocked(in_bus)].m_Volume = std::max(in_volume, 0.0f);
}

float AudioMixer::GetBusVolume(AudioBusId in_bus) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Buses[BusIndexLocked(in_bus)].m_Volume;
}

void AudioMixer::SetBusPaused(AudioBusId in_bus, bool in_paused)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Buses[BusIndexLocked(in_bus)].m_Paused = in_paused;
}

void AudioMixer::SetMasterVolume(float in_volume)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MasterVolume = std::max(in_volume, 0.0f);
}

void AudioMixer::SetMasterPaused(bool in_paused)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MasterPaused = in_paused;
}

void AudioMixer::SetListener(const AudioListenerData& in_listener)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Listener = in_listener;
}

AudioListenerData AudioMixer::GetListener() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Listener;
}

AudioMixer::Voice* AudioMixer::FindVoiceLocked(AudioVoiceId in_voice)
{
    for (Voice& voice : m_Voices)
    {
        if (voice.m_Id == in_voice)
        {
            return &voice;
        }
    }
    return nullptr;
}

const AudioMixer::Voice* AudioMixer::FindVoiceLocked(AudioVoiceId in_voice) const
{
    for (const Voice& voice : m_Voices)
    {
        if (voice.m_Id == in_voice)
        {
            return &voice;
        }
    }
    return nullptr;
}

AudioVoiceId AudioMixer::Play(const AudioClipData* in_clip, const AudioPlayParams& in_params)
{
    if (in_clip == nullptr || !in_clip->IsValid())
    {
        return kInvalidAudioVoiceId;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    // Voice stealing: drop the least important voice when the pool is full.
    if (m_Voices.size() >= kAudioMaxVoices)
    {
        size_t victim = 0u;
        for (size_t index = 1u; index < m_Voices.size(); ++index)
        {
            const Voice& candidate = m_Voices[index];
            const Voice& current = m_Voices[victim];
            const bool lower_priority = candidate.m_Params.m_Priority < current.m_Params.m_Priority;
            const bool same_priority_but_older =
                (candidate.m_Params.m_Priority == current.m_Params.m_Priority) && (candidate.m_Id < current.m_Id);
            if (lower_priority || same_priority_but_older)
            {
                victim = index;
            }
        }
        m_Voices.erase(m_Voices.begin() + static_cast<std::ptrdiff_t>(victim));
    }

    Voice voice;
    voice.m_Id = m_NextVoiceId++;
    voice.m_Clip = in_clip;
    voice.m_Params = in_params;
    voice.m_Playhead = 0.0;
    voice.m_State = AudioVoiceState::Playing;

    if (in_params.m_FadeInSeconds > 0.0f)
    {
        voice.m_FadeGain = 0.0f;
        voice.m_FadeSeconds = in_params.m_FadeInSeconds;
    }
    voice.m_FadeTarget = 1.0f;

    m_Voices.push_back(voice);
    return voice.m_Id;
}

void AudioMixer::Stop(AudioVoiceId in_voice, float in_fade_out_seconds)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    Voice* voice = FindVoiceLocked(in_voice);
    if (voice == nullptr)
    {
        return;
    }

    if (in_fade_out_seconds <= 0.0f)
    {
        m_Voices.erase(m_Voices.begin() + static_cast<std::ptrdiff_t>(voice - m_Voices.data()));
        return;
    }

    voice->m_FadeTarget = 0.0f;
    voice->m_FadeSeconds = in_fade_out_seconds;
    voice->m_FadeRate = 0.0f;  // Recomputed on the next Render(): needs the device rate.
    voice->m_RemoveWhenFaded = true;
}

void AudioMixer::StopAll(float in_fade_out_seconds)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (in_fade_out_seconds <= 0.0f)
    {
        m_Voices.clear();
        return;
    }

    for (Voice& voice : m_Voices)
    {
        voice.m_FadeTarget = 0.0f;
        voice.m_FadeSeconds = in_fade_out_seconds;
        voice.m_FadeRate = 0.0f;
        voice.m_RemoveWhenFaded = true;
    }
}

bool AudioMixer::SetPaused(AudioVoiceId in_voice, bool in_paused)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    Voice* voice = FindVoiceLocked(in_voice);
    if (voice == nullptr)
    {
        return false;
    }

    // A voice mid fade-out stays stopped; resuming it would resurrect sound the
    // caller already asked to silence.
    if (voice->m_RemoveWhenFaded)
    {
        return false;
    }

    voice->m_State = in_paused ? AudioVoiceState::Paused : AudioVoiceState::Playing;
    return true;
}

AudioVoiceState AudioMixer::GetVoiceState(AudioVoiceId in_voice) const
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    const Voice* voice = FindVoiceLocked(in_voice);
    return (voice != nullptr) ? voice->m_State : AudioVoiceState::Stopped;
}

bool AudioMixer::UpdateVoiceParams(AudioVoiceId in_voice, const AudioPlayParams& in_params)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    Voice* voice = FindVoiceLocked(in_voice);
    if (voice == nullptr)
    {
        return false;
    }

    // Keep the clip: params describe how a voice is played, not what it plays.
    const AudioClipData* clip = voice->m_Clip;
    voice->m_Params = in_params;
    voice->m_Clip = clip;
    return true;
}

uint32_t AudioMixer::GetActiveVoiceCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<uint32_t>(m_Voices.size());
}

void AudioMixer::Render(float* out_frames, uint32_t in_frame_count, uint32_t in_channels, uint32_t in_sample_rate)
{
    if (out_frames == nullptr || in_frame_count == 0u || in_channels == 0u || in_sample_rate == 0u)
    {
        return;
    }

    const size_t sample_count = static_cast<size_t>(in_frame_count) * in_channels;

    std::lock_guard<std::mutex> lock(m_Mutex);

    std::fill(out_frames, out_frames + sample_count, 0.0f);

    if (m_MasterPaused || m_MasterVolume <= 0.0f)
    {
        return;
    }

    // Right vector for stereo panning, derived once per buffer.
    const AudioVec3 right = m_Listener.m_Forward.Cross(m_Listener.m_Up).Normalised();

    for (size_t voice_index = 0u; voice_index < m_Voices.size();)
    {
        Voice& voice = m_Voices[voice_index];
        const AudioClipData* clip = voice.m_Clip;

        if (clip == nullptr || !clip->IsValid())
        {
            m_Voices.erase(m_Voices.begin() + static_cast<std::ptrdiff_t>(voice_index));
            continue;
        }

        const AudioPlayParams& params = voice.m_Params;
        const Bus& bus = m_Buses[BusIndexLocked(params.m_Bus)];

        if (voice.m_FadeRate <= 0.0f && voice.m_FadeSeconds > 0.0f)
        {
            voice.m_FadeRate = 1.0f / (voice.m_FadeSeconds * static_cast<float>(in_sample_rate));
        }

        const bool fading_in = voice.m_FadeTarget > voice.m_FadeGain;
        const bool can_sound = (voice.m_State == AudioVoiceState::Playing) && !bus.m_Paused && bus.m_Volume > 0.0f &&
                               params.m_Volume > 0.0f && (voice.m_FadeGain > 0.0f || fading_in);

        bool finished = false;

        if (can_sound)
        {
            const uint64_t source_frames = clip->GetFrameCount();
            const uint32_t clip_channels = clip->m_Channels;
            const double rate = static_cast<double>(params.m_Pitch) * static_cast<double>(clip->m_SampleRate) /
                                static_cast<double>(in_sample_rate);

            // ---- spatialisation, evaluated once per buffer ----
            float attenuation = 1.0f;
            float pan = 0.0f;
            if (params.m_SpatialBlend > 0.0f)
            {
                const AudioVec3 offset = params.m_Position - m_Listener.m_Position;
                const float distance = offset.Length();
                const float spatial_attenuation =
                    DistanceAttenuation(distance, params.m_MinDistance, params.m_MaxDistance, params.m_DistanceModel);
                const float spatial_pan = (distance > 1.0e-4f) ? (offset * (1.0f / distance)).Dot(right) : 0.0f;

                const float blend = std::min(std::max(params.m_SpatialBlend, 0.0f), 1.0f);
                attenuation = 1.0f + (spatial_attenuation - 1.0f) * blend;
                pan = spatial_pan * blend;
            }

            float pan_left = 1.0f;
            float pan_right = 1.0f;
            if (in_channels >= 2u)
            {
                PanGains(std::min(std::max(pan, -1.0f), 1.0f), pan_left, pan_right);
            }

            const float voice_gain = params.m_Volume * attenuation * bus.m_Volume;

            for (uint32_t frame = 0u; frame < in_frame_count; ++frame)
            {
                // ---- fade ----
                if (voice.m_FadeGain != voice.m_FadeTarget)
                {
                    if (voice.m_FadeGain < voice.m_FadeTarget)
                    {
                        voice.m_FadeGain = std::min(voice.m_FadeGain + voice.m_FadeRate, voice.m_FadeTarget);
                    }
                    else
                    {
                        voice.m_FadeGain = std::max(voice.m_FadeGain - voice.m_FadeRate, voice.m_FadeTarget);
                    }
                }

                if (voice.m_RemoveWhenFaded && voice.m_FadeGain <= 0.0f)
                {
                    finished = true;
                    break;
                }

                // ---- resample: linear interpolation per source channel ----
                const double playhead = voice.m_Playhead;
                const uint64_t frame_index = static_cast<uint64_t>(playhead);
                const float fraction = static_cast<float>(playhead - static_cast<double>(frame_index));

                float left = 0.0f;
                float right_sample = 0.0f;
                for (uint32_t channel = 0u; channel < 2u; ++channel)
                {
                    // Mono clips feed both output channels; clips with >= 2
                    // channels use their first two; anything beyond that is
                    // dropped (V1 has no speaker-matrix routing).
                    const uint32_t source_channel = std::min(channel, clip_channels - 1u);

                    const uint64_t base_index = frame_index * clip_channels + source_channel;
                    uint64_t next_index = base_index + clip_channels;
                    if (next_index >= clip->m_Samples.size())
                    {
                        next_index = params.m_Loop ? (source_channel) : next_index;
                    }

                    const float sample_a = clip->m_Samples[static_cast<size_t>(base_index)];
                    const float sample_b = (next_index < clip->m_Samples.size())
                                               ? clip->m_Samples[static_cast<size_t>(next_index)]
                                               : sample_a;
                    const float sample = sample_a + (sample_b - sample_a) * fraction;

                    if (channel == 0u)
                    {
                        left = sample;
                    }
                    else
                    {
                        right_sample = sample;
                    }
                }

                const float gain = voice_gain * voice.m_FadeGain;
                out_frames[static_cast<size_t>(frame) * in_channels] += left * gain * pan_left;
                if (in_channels >= 2u)
                {
                    out_frames[static_cast<size_t>(frame) * in_channels + 1u] += right_sample * gain * pan_right;
                }

                // ---- advance ----
                voice.m_Playhead += rate;
                if (voice.m_Playhead >= static_cast<double>(source_frames))
                {
                    if (params.m_Loop)
                    {
                        voice.m_Playhead -= static_cast<double>(source_frames);
                        if (voice.m_Playhead < 0.0)
                        {
                            voice.m_Playhead = 0.0;
                        }
                    }
                    else
                    {
                        finished = true;
                        break;
                    }
                }
            }
        }

        if (finished)
        {
            m_Voices.erase(m_Voices.begin() + static_cast<std::ptrdiff_t>(voice_index));
            continue;
        }

        ++voice_index;
    }

    // ---- master gain + listener gain + soft knee ----
    const float final_gain = m_MasterVolume * m_Listener.m_Gain;
    if (final_gain != 1.0f)
    {
        for (size_t index = 0u; index < sample_count; ++index)
        {
            out_frames[index] = SoftClip(out_frames[index] * final_gain);
        }
    }
    else
    {
        for (size_t index = 0u; index < sample_count; ++index)
        {
            out_frames[index] = SoftClip(out_frames[index]);
        }
    }
}
