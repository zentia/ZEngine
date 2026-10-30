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

    private:
        /// Builds AudioPlayParams from the serialized fields + current transform.
        AudioPlayParams BuildPlayParams() const;

        /// Registers this component's emitter with Wwise on first use and returns
        /// its id, or kInvalidGameObject (after logging) when it cannot -- an
        /// object with no allocated id has no stable identity to be an emitter.
        /// Idempotent: the id is cached in m_WwiseGameObject.
        uint64_t ResolveWwiseEmitter();

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
    };
}  // namespace Runtime
