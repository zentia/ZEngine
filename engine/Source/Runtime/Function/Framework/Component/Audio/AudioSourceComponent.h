#pragma once

#include "Runtime/Function/Audio/AudioCommon.h"
#include "Runtime/Function/Framework/Component/Component.h"

namespace Runtime
{
    /// Plays an AudioClip in the world. Unity's AudioSource analogue.
    ///
    /// The component owns exactly one voice at a time. Calling Play() while a
    /// voice is live stops the previous one -- an AudioSource is a single
    /// emitter, and stacking voices behind one component makes "is it playing?"
    /// unanswerable. Fire-and-forget one-shots should call
    /// AudioSystem::PlayOneShot() directly instead.
    class AudioSourceComponent : public Component
    {
        REGISTER_CLASS(AudioSourceComponent);
        DECLARE_OBJECT_SERIALIZE();

    public:
        AudioSourceComponent() = default;
        ~AudioSourceComponent() override;

        void PostLoadResource(GameObject* parent_object) override;
        void Tick(float delta_time) override;

        AudioVoiceId Play();
        void Stop(float in_fade_out_seconds = 0.0f);
        void SetPaused(bool in_paused);
        bool IsPlaying() const;

        AudioVoiceId GetVoiceId() const { return m_VoiceId; }

    private:
        /// Builds AudioPlayParams from the serialized fields + current transform.
        AudioPlayParams BuildPlayParams() const;

        eastl::string m_ClipPath;
        /// Empty = master bus. Resolved by name on every Play().
        eastl::string m_BusName;
        float m_Volume {1.0f};
        float m_Pitch {1.0f};
        bool m_Loop {false};
        bool m_PlayOnAwake {false};
        /// 0 = 2D, 1 = fully spatial.
        float m_SpatialBlend {1.0f};
        float m_MinDistance {1.0f};
        float m_MaxDistance {50.0f};
        float m_FadeInSeconds {0.0f};
        int32_t m_Priority {0};

        /// Not serialized: a voice id is only meaningful for this process.
        AudioVoiceId m_VoiceId {kInvalidAudioVoiceId};
    };
}  // namespace Runtime
