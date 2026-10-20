#pragma once

#include "Runtime/Function/Audio/AudioCommon.h"
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

        AudioVoiceId Play();
        void Stop(float in_fade_out_seconds = 0.0f);
        void SetPaused(bool in_paused);
        bool IsPlaying() const;

        AudioVoiceId GetVoiceId() const { return m_VoiceId; }

    private:
        /// Builds AudioPlayParams from the serialized fields + current transform.
        AudioPlayParams BuildPlayParams() const;

        ZPROPERTY(meta=(DisplayName="Audio Clip", Tooltip="Asset path of the clip to play."))
        eastl::string m_ClipPath;
        /// Empty = master bus. Resolved by name on every Play().
        ZPROPERTY(meta=(DisplayName="Output Bus", Tooltip="Mixer bus name; empty routes to the master bus."))
        eastl::string m_BusName;
        ZPROPERTY(meta=(ClampMin=0, ClampMax=1))
        float m_Volume {1.0f};
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
    };
}  // namespace Runtime
