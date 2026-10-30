#include "Runtime/Function/Framework/Component/Audio/AudioSourceComponent.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Audio/AudioSystem.h"
#include "Runtime/Function/Audio/Wwise/WwiseAudioSystem.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"


namespace Runtime
{

namespace
{
    /// Emitter game-object ids are the owning GameObject's id plus this offset.
    ///
    /// The offset exists because WwiseAudioSystem reserves id 1 for the single
    /// default listener, and GameObject ids come from a counter that starts at 0.
    /// Without the offset the first object in a scene would register itself as
    /// the listener, and every frame it would overwrite the listener transform
    /// with its own -- all 3D audio would then be positioned by whatever emitter
    /// happened to tick last, with nothing in the log to explain it.
    constexpr uint64_t kEmitterGameObjectIdOffset = 0x40000000ull;
}  // namespace

    AudioSourceComponent::~AudioSourceComponent()
    {
        // Leaving a voice running after the component dies would play audio
        // attached to nothing and leak a slot in the 64-voice pool. Stop() covers
        // both routes; the Wwise one matters just as much because a posted event
        // keeps its emitter registered until it is stopped explicitly.
        Stop();
    }

    void AudioSourceComponent::PostLoadResource(GameObject* parent_object)
    {
        m_ParentObject = parent_object;

        // Either field can be the live one depending on which backend owns the
        // output, so "anything to play" is not `!m_ClipPath.empty()` any more:
        // a Wwise project legitimately has no clip path at all.
        if (m_PlayOnAwake && (!m_ClipPath.empty() || !m_WwiseEvent.empty()))
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

    uint64_t AudioSourceComponent::ResolveWwiseEmitter()
    {
        if (m_WwiseGameObject != WwiseAudioSystem::kInvalidGameObject)
        {
            return m_WwiseGameObject;
        }

        if (m_ParentObject == nullptr || m_ParentObject->GetID() == k_invalid_gobject_id)
        {
            LOG_ERROR(ZAudio,
                      "AudioSourceComponent cannot play through Wwise: the owning GameObject has no allocated id, "
                      "so there is no stable identity to use as an emitter");
            return WwiseAudioSystem::kInvalidGameObject;
        }

        WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem);
        if (wwise == nullptr)
        {
            return WwiseAudioSystem::kInvalidGameObject;
        }

        const uint64_t candidate = kEmitterGameObjectIdOffset + static_cast<uint64_t>(m_ParentObject->GetID());
        if (!wwise->RegisterGameObject(candidate, m_ParentObject->GetName().c_str()))
        {
            return WwiseAudioSystem::kInvalidGameObject;
        }

        m_WwiseGameObject = candidate;
        return candidate;
    }

    void AudioSourceComponent::PushWwiseEmitterTransform(uint64_t in_game_object) const
    {
        WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem);
        if (wwise == nullptr || m_ParentObject == nullptr)
        {
            return;
        }

        const Transform* transform = m_ParentObject->tryGetComponent<Transform>("Transform");
        if (transform == nullptr)
        {
            return;
        }

        // ZEngine is Z-up: forward = -Y, up = +Z (see AudioCommon.h). Wwise takes
        // both because its own spatialisation needs an orientation, not just a
        // point -- a listener behind an emitter must not sound like one in front.
        const AudioVec3 position = AudioSystem::ToAudioVec3(transform->GetPosition());
        const AudioVec3 forward = AudioSystem::ToAudioVec3(transform->TransformDirection(Vector3(0.0f, -1.0f, 0.0f)));
        const AudioVec3 up = AudioSystem::ToAudioVec3(transform->TransformDirection(Vector3(0.0f, 0.0f, 1.0f)));
        wwise->SetGameObjectTransform(in_game_object, position, forward, up);
    }

    AudioVoiceId AudioSourceComponent::Play()
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr)
        {
            return kInvalidAudioVoiceId;
        }

        // One voice per source on whichever route is live: stop first, then start.
        Stop();

        const AudioPlayRoute route = ResolveAudioPlayRoute(audio_system->IsWwiseActive(),
                                                          m_ClipPath.c_str(),
                                                          m_WwiseEvent.c_str());

        if (route == AudioPlayRoute::kWwiseEvent)
        {
            WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem);
            const uint64_t game_object = ResolveWwiseEmitter();
            if (wwise == nullptr || game_object == WwiseAudioSystem::kInvalidGameObject)
            {
                return kInvalidAudioVoiceId;
            }

            // Post from the right place: Wwise does not read the scene graph, so
            // an emitter that is registered but never positioned plays from the
            // origin until something happens to move it.
            PushWwiseEmitterTransform(game_object);
            m_WwisePlayingId = wwise->PostEvent(m_WwiseEvent.c_str(), game_object);
            return m_WwisePlayingId;
        }

        if (route != AudioPlayRoute::kClipFile)
        {
            // Loud rather than silent: a Play() that quietly does nothing is the
            // worst failure an audio component has, because "it compiled and ran"
            // looks exactly like "this field was never filled in".
            LOG_ERROR(ZAudio,
                      "AudioSourceComponent::Play() has nothing to play: {}",
                      audio_system->IsWwiseActive()
                          ? "the Wwise backend owns the output but m_WwiseEvent is empty "
                            "(a clip path is meaningless to Wwise -- banks hold events, not files)"
                          : "m_ClipPath is empty");
            return kInvalidAudioVoiceId;
        }

        const AudioClipData* clip = audio_system->GetClip(m_ClipPath.c_str());
        if (clip == nullptr)
        {
            return kInvalidAudioVoiceId;
        }

        m_VoiceId = audio_system->Play(clip, BuildPlayParams());
        return m_VoiceId;
    }

    void AudioSourceComponent::Stop(float in_fade_out_seconds)
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr)
        {
            return;
        }

        // One route owns this source at a time, so whichever handle is set is the
        // one to release -- and releasing the wrong one would be a silent no-op.
        if (m_WwisePlayingId != WwiseAudioSystem::kInvalidPlayingId)
        {
            if (WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem))
            {
                wwise->StopPlayingId(m_WwisePlayingId, in_fade_out_seconds);
            }
            m_WwisePlayingId = WwiseAudioSystem::kInvalidPlayingId;
            return;
        }

        if (m_VoiceId == kInvalidAudioVoiceId)
        {
            return;
        }

        audio_system->Stop(m_VoiceId, in_fade_out_seconds);
        m_VoiceId = kInvalidAudioVoiceId;
    }

    void AudioSourceComponent::SetPaused(bool in_paused)
    {
        if (m_WwisePlayingId != WwiseAudioSystem::kInvalidPlayingId)
        {
            if (WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem))
            {
                wwise->SetPlayingPaused(m_WwisePlayingId, in_paused);
            }
            return;
        }

        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system != nullptr && m_VoiceId != kInvalidAudioVoiceId)
        {
            audio_system->SetPaused(m_VoiceId, in_paused);
        }
    }

    bool AudioSourceComponent::IsPlaying() const
    {
        if (m_WwisePlayingId != WwiseAudioSystem::kInvalidPlayingId)
        {
            // The handle is cleared in Tick() -- this method is const on purpose,
            // so it reports what Wwise last told us instead of mutating state.
            const WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem);
            return (wwise != nullptr) && wwise->IsPlayingId(m_WwisePlayingId);
        }

        const AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        return (audio_system != nullptr) && (m_VoiceId != kInvalidAudioVoiceId) && audio_system->IsPlaying(m_VoiceId);
    }

    void AudioSourceComponent::Tick(float delta_time)
    {
        (void)delta_time;

        if (m_WwisePlayingId != WwiseAudioSystem::kInvalidPlayingId)
        {
            WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem);
            if (wwise == nullptr || !wwise->IsPlayingId(m_WwisePlayingId))
            {
                // The event ended on its own. Drop the handle for the same reason
                // the clip route does: so IsPlaying() stays honest and a later
                // Play() does not try to stop an event that is already over.
                m_WwisePlayingId = WwiseAudioSystem::kInvalidPlayingId;
                return;
            }

            if (m_SpatialBlend > 0.0f && m_WwiseGameObject != WwiseAudioSystem::kInvalidGameObject)
            {
                PushWwiseEmitterTransform(m_WwiseGameObject);
            }
            return;
        }

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
