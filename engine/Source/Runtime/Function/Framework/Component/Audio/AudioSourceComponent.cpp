#include "Runtime/Function/Framework/Component/Audio/AudioSourceComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

IMPLEMENT_REGISTER_CLASS_3(Runtime, AudioSourceComponent, typeid(Runtime::AudioSourceComponent).hash_code())
IMPLEMENT_OBJECT_SERIALIZE(Runtime::AudioSourceComponent)

namespace Runtime
{
    template<typename TransferFunction>
    void AudioSourceComponent::Transfer(TransferFunction& transfer)
    {
        Super::Transfer(transfer);
        transfer.Transfer(m_ClipPath, "clip_path");
        transfer.Transfer(m_BusName, "bus_name");
        transfer.Transfer(m_Volume, "volume");
        transfer.Transfer(m_Pitch, "pitch");
        transfer.Transfer(m_Loop, "loop");
        transfer.Transfer(m_PlayOnAwake, "play_on_awake");
        transfer.Transfer(m_SpatialBlend, "spatial_blend");
        transfer.Transfer(m_MinDistance, "min_distance");
        transfer.Transfer(m_MaxDistance, "max_distance");
        transfer.Transfer(m_FadeInSeconds, "fade_in_seconds");
        transfer.Transfer(m_Priority, "priority");
    }

    INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AudioSourceComponent)

    AudioSourceComponent::~AudioSourceComponent()
    {
        // Leaving a voice running after the component dies would play audio
        // attached to nothing and leak a slot in the 64-voice pool.
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system != nullptr)
        {
            audio_system->Stop(m_VoiceId);
        }
    }

    void AudioSourceComponent::PostLoadResource(GameObject* parent_object)
    {
        m_ParentObject = parent_object;

        if (m_PlayOnAwake && !m_ClipPath.empty())
        {
            Play();
        }
    }

    AudioPlayParams AudioSourceComponent::BuildPlayParams() const
    {
        AudioPlayParams params;
        params.m_Volume = m_Volume;
        params.m_Pitch = m_Pitch;
        params.m_Loop = m_Loop;
        params.m_SpatialBlend = m_SpatialBlend;
        params.m_MinDistance = m_MinDistance;
        params.m_MaxDistance = m_MaxDistance;
        params.m_FadeInSeconds = m_FadeInSeconds;
        params.m_Priority = m_Priority;

        if (!m_BusName.empty())
        {
            AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
            const AudioBusId bus = (audio_system != nullptr) ? audio_system->FindBus(m_BusName.c_str()) : kInvalidAudioBusId;
            params.m_Bus = (bus != kInvalidAudioBusId) ? bus : kAudioMasterBusId;
        }

        if (m_ParentObject != nullptr && m_SpatialBlend > 0.0f)
        {
            const Transform* transform = m_ParentObject->tryGetComponent<Transform>("Transform");
            if (transform != nullptr)
            {
                params.m_Position = AudioSystem::ToAudioVec3(transform->GetPosition());
            }
        }

        return params;
    }

    AudioVoiceId AudioSourceComponent::Play()
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_ClipPath.empty())
        {
            return kInvalidAudioVoiceId;
        }

        const AudioClipData* clip = audio_system->GetClip(m_ClipPath.c_str());
        if (clip == nullptr)
        {
            return kInvalidAudioVoiceId;
        }

        // One voice per source: stop whatever is currently playing.
        if (m_VoiceId != kInvalidAudioVoiceId)
        {
            audio_system->Stop(m_VoiceId);
            m_VoiceId = kInvalidAudioVoiceId;
        }

        m_VoiceId = audio_system->Play(clip, BuildPlayParams());
        return m_VoiceId;
    }

    void AudioSourceComponent::Stop(float in_fade_out_seconds)
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_VoiceId == kInvalidAudioVoiceId)
        {
            return;
        }

        audio_system->Stop(m_VoiceId, in_fade_out_seconds);
        m_VoiceId = kInvalidAudioVoiceId;
    }

    void AudioSourceComponent::SetPaused(bool in_paused)
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system != nullptr && m_VoiceId != kInvalidAudioVoiceId)
        {
            audio_system->SetPaused(m_VoiceId, in_paused);
        }
    }

    bool AudioSourceComponent::IsPlaying() const
    {
        const AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        return (audio_system != nullptr) && (m_VoiceId != kInvalidAudioVoiceId) && audio_system->IsPlaying(m_VoiceId);
    }

    void AudioSourceComponent::Tick(float delta_time)
    {
        (void)delta_time;

        if (m_VoiceId == kInvalidAudioVoiceId || m_SpatialBlend <= 0.0f)
        {
            return;
        }

        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || !audio_system->IsPlaying(m_VoiceId))
        {
            // The voice ended on its own; drop the handle so IsPlaying() stays
            // honest and a later Play() does not try to stop a dead voice.
            m_VoiceId = kInvalidAudioVoiceId;
            return;
        }

        audio_system->UpdateVoice(m_VoiceId, BuildPlayParams());
    }
}  // namespace Runtime
