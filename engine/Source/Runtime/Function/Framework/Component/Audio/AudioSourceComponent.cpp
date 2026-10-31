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
        // 静音在**这里**生效而不是让脚本去改 volume：改 volume 会把原值冲掉，
        // 取消静音就恢复不回去了（见 m_Mute 的注释）。
        params.m_Volume = m_Mute ? 0.0f : m_Volume;
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
        return PlayWithDelay(0.0f);
    }

    void AudioSourceComponent::PlayDelayed(float delay_seconds)
    {
        // 负延迟没有意义（"立刻"就是 0），夹掉而不是让 mixer 去解释一个负数。
        PlayWithDelay(delay_seconds > 0.0f ? delay_seconds : 0.0f);
    }

    AudioVoiceId AudioSourceComponent::PlayWithDelay(float in_delay_seconds)
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

            if (in_delay_seconds > 0.0f)
            {
                // Wwise 的 event 没有"延迟参数"：post 出去就响。所以在这里排队，
                // 由 Tick 数完再 post（见 m_WwisePendingDelay 的注释）。
                m_WwisePendingDelay = in_delay_seconds;
                m_HasWwisePendingDelay = true;
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

        AudioPlayParams params = BuildPlayParams();
        params.m_DelaySeconds = in_delay_seconds;
        m_VoiceId = audio_system->Play(clip, params);
        return m_VoiceId;
    }

    void AudioSourceComponent::Stop(float in_fade_out_seconds)
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr)
        {
            return;
        }

        // 取消还没排上的 Wwise 延迟：否则 Stop() 之后 Tick 仍会把 event post 出去，
        // 于是"停了还在响"，而且没有任何日志。
        m_HasWwisePendingDelay = false;
        m_WwisePendingDelay = 0.0f;

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
        m_WwisePaused = in_paused;

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

    void AudioSourceComponent::Pause()
    {
        SetPaused(true);
    }

    void AudioSourceComponent::UnPause()
    {
        SetPaused(false);
    }

    bool AudioSourceComponent::IsPaused() const
    {
        if (m_WwisePlayingId != WwiseAudioSystem::kInvalidPlayingId)
        {
            // Wwise 侧没有"查询暂停"的接口（只有 set），所以这里报的是组件自己
            // 记下的最后一次意图。返回值只在"没人调过 SetPaused"时是 false。
            return m_WwisePaused;
        }

        const AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        return (audio_system != nullptr) && (m_VoiceId != kInvalidAudioVoiceId) && audio_system->IsVoicePaused(m_VoiceId);
    }

    AudioVoiceId AudioSourceComponent::PlayOneShot(const eastl::string& clip_path, float volume_scale)
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || clip_path.empty())
        {
            return kInvalidAudioVoiceId;
        }

        // 拿本组件的参数当基底（bus / 空间化 / 距离衰减都一样），只把音量和位置
        // 按这一次调用覆盖。这样"在 3D 音源上叠一个一次性音效"仍然是从这个音源
        // 的位置发出的，而不是从原点。
        AudioPlayParams params = BuildPlayParams();
        params.m_Volume *= (volume_scale > 0.0f) ? volume_scale : 0.0f;
        // 一次性音效不该继承 loop：叠一个循环体上去就再也停不下来了。
        params.m_Loop = false;
        params.m_DelaySeconds = 0.0f;

        return audio_system->PlayOneShot(clip_path.c_str(), params);
    }

    float AudioSourceComponent::GetTime() const
    {
        const AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_VoiceId == kInvalidAudioVoiceId)
        {
            return 0.0f;
        }

        const double seconds = audio_system->GetVoicePlaybackTime(m_VoiceId);
        // -1 表示"这条 voice 已经没了"，对脚本来说就是 0（没在播）。
        return (seconds > 0.0) ? static_cast<float>(seconds) : 0.0f;
    }

    void AudioSourceComponent::SetTime(float time_seconds)
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_VoiceId == kInvalidAudioVoiceId)
        {
            return;
        }
        audio_system->SetVoicePlaybackTime(m_VoiceId, static_cast<double>(time_seconds));
    }

    int AudioSourceComponent::GetTimeSamples() const
    {
        const AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_VoiceId == kInvalidAudioVoiceId)
        {
            return 0;
        }
        return static_cast<int>(audio_system->GetVoicePlaybackFrames(m_VoiceId));
    }

    float AudioSourceComponent::GetClipLength() const
    {
        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        if (audio_system == nullptr || m_ClipPath.empty())
        {
            return 0.0f;
        }

        const AudioClipData* clip = audio_system->GetClip(m_ClipPath.c_str());
        if (clip == nullptr || clip->m_SampleRate == 0u)
        {
            return 0.0f;
        }

        // 折算 pitch：pitch 2 的素材听起来只有一半长，`time` 走到头也就是一半。
        const float pitch = (m_Pitch > 0.0f) ? m_Pitch : 1.0f;
        return clip->GetDuration() / pitch;
    }

    void AudioSourceComponent::Tick(float delta_time)
    {
        // ---- Wwise 路线的 PlayDelayed 倒计时 ----------------------------
        if (m_HasWwisePendingDelay)
        {
            m_WwisePendingDelay -= delta_time;
            if (m_WwisePendingDelay > 0.0f)
            {
                return;
            }
            m_HasWwisePendingDelay = false;
            m_WwisePendingDelay = 0.0f;

            WwiseAudioSystem* wwise = GET_SYSTEM(WwiseAudioSystem);
            const uint64_t game_object = ResolveWwiseEmitter();
            if (wwise != nullptr && game_object != WwiseAudioSystem::kInvalidGameObject)
            {
                PushWwiseEmitterTransform(game_object);
                m_WwisePlayingId = wwise->PostEvent(m_WwiseEvent.c_str(), game_object);
            }
            return;
        }

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

        if (m_VoiceId == kInvalidAudioVoiceId)
        {
            return;
        }

        AudioSystem* audio_system = GET_SYSTEM(AudioSystem);
        // ⚠️ 暂停中的 voice 也是"活着的"。
        //
        // `AudioSystem::IsPlaying()` 按 Unity 的 `AudioSource.isPlaying` 语义来：
        // 暂停时返回 **false**（Unity 文档明说 Pause() 之后 isPlaying 是 false）。
        // 但"不在播"和"没了"是两回事 —— 只问 IsPlaying 的话，一个暂停中的音源
        // 下一帧就会在这里被当成"播完了"，句柄一丢，`UnPause()` 就再也没有对象
        // 可以恢复，而且没有任何日志。所以暂停要单独认。
        const bool voice_alive = (audio_system != nullptr) &&
                                 (audio_system->IsPlaying(m_VoiceId) || audio_system->IsVoicePaused(m_VoiceId));
        if (!voice_alive)
        {
            // The voice ended on its own; drop the handle so IsPlaying() stays
            // honest and a later Play() does not try to stop a dead voice.
            m_VoiceId = kInvalidAudioVoiceId;
            return;
        }

        // 每帧都推，不管是不是 3D。
        //
        // 原来这里有一条 `m_SpatialBlend <= 0` 的提前返回 —— 于是**2D 音源改
        // volume / pitch / mute 完全不生效**：那些字段是 ZPROPERTY，脚本直接写
        // 字段、绕过了任何 setter，唯一的生效路径就是这里。表现为"游戏里调小音量
        // 没反应"，而且是静默的。3D 音源因为要更新位置所以顺带生效了，这就是这
        // 个 bug 一直没被发现的原因。
        audio_system->UpdateVoice(m_VoiceId, BuildPlayParams());
    }
}  // namespace Runtime
