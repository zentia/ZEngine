#pragma once

// =============================================================================
// Dependency-free audio primitives (PR-A1, audio vertical slice).
//
// Everything in this header is deliberately free of engine headers: no LogSystem,
// no eastl, no math library. That keeps the DSP layer (decoder + mixer) unit
// testable outside a running engine -- see Function/Audio/Test, which compiles
// and runs these sources standalone.
//
// Design reference:
//   * Unity  -- AudioClip (decoded PCM) / AudioSource (voice) / AudioMixer (bus)
//   * Unreal -- USoundWave / UAudioComponent / FAudioDevice
//
// ZEngine keeps Unity's three-object split because it maps 1:1 onto what the
// runtime actually needs (asset, emitter, output) and because the editor's
// Inspector can show the same three things.
// =============================================================================

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using AudioVoiceId = uint64_t;
using AudioBusId = uint32_t;

static constexpr AudioVoiceId kInvalidAudioVoiceId = 0u;
static constexpr AudioBusId kInvalidAudioBusId = 0xFFFFFFFFu;

/// Bus 0 is the master bus and always exists.
static constexpr AudioBusId kAudioMasterBusId = 0u;

/// Hard cap on simultaneous voices. Matches Unity's "virtual voice" idea only in
/// spirit: exceeding the cap steals the lowest-priority voice rather than
/// queuing, so the cost of a runaway Play() loop is bounded.
static constexpr uint32_t kAudioMaxVoices = 64u;

/// Minimal 3-vector so the DSP layer stays engine-math-free.
/// ZEngine is Z-up, so the listener convention is forward = -Y, up = +Z
/// (see CameraComponent), but nothing here depends on it.
struct AudioVec3
{
    float x {0.0f};
    float y {0.0f};
    float z {0.0f};

    AudioVec3() = default;
    AudioVec3(float in_x, float in_y, float in_z) : x(in_x), y(in_y), z(in_z) {}

    AudioVec3 operator+(const AudioVec3& in_rhs) const { return AudioVec3(x + in_rhs.x, y + in_rhs.y, z + in_rhs.z); }
    AudioVec3 operator-(const AudioVec3& in_rhs) const { return AudioVec3(x - in_rhs.x, y - in_rhs.y, z - in_rhs.z); }
    AudioVec3 operator*(float in_scale) const { return AudioVec3(x * in_scale, y * in_scale, z * in_scale); }

    float Dot(const AudioVec3& in_rhs) const { return x * in_rhs.x + y * in_rhs.y + z * in_rhs.z; }

    AudioVec3 Cross(const AudioVec3& in_rhs) const
    {
        return AudioVec3(y * in_rhs.z - z * in_rhs.y, z * in_rhs.x - x * in_rhs.z, x * in_rhs.y - y * in_rhs.x);
    }

    float LengthSquared() const { return Dot(*this); }
    float Length() const { return LengthSquared() > 0.0f ? static_cast<float>(std::sqrt(LengthSquared())) : 0.0f; }

    AudioVec3 Normalised() const
    {
        const float length = Length();
        return length > 1.0e-6f ? (*this) * (1.0f / length) : AudioVec3();
    }
};

/// How a spatial voice attenuates between min and max distance.
enum class AudioDistanceModel : uint8_t
{
    /// Unity's default "logarithmic" rolloff approximation: min/max clamp + 1/d.
    Inverse = 0,
    /// Linear fade from 1 (at min distance) to 0 (at max distance).
    Linear = 1,
    /// UE-style inverse-square with the same min/max clamp as Inverse.
    InverseSquare = 2
};

/// Decoded PCM, always normalised to interleaved float32 in [-1, 1].
///
/// V1 loads whole clips into memory (Unity's "Decompress On Load"). Streaming
/// (big music beds) needs the clip to be chunked behind a read-ahead thread and
/// is deliberately out of scope for the first slice.
struct AudioClipData
{
    uint32_t m_SampleRate {0u};
    uint32_t m_Channels {0u};
    /// Interleaved: frame i lives at [i * m_Channels, (i + 1) * m_Channels).
    std::vector<float> m_Samples;
    std::string m_Name;

    bool IsValid() const
    {
        return m_SampleRate > 0u && m_Channels > 0u && !m_Samples.empty() &&
               (m_Samples.size() % m_Channels) == 0u;
    }

    uint64_t GetFrameCount() const { return m_Channels > 0u ? m_Samples.size() / m_Channels : 0u; }

    float GetDuration() const
    {
        return (m_SampleRate > 0u && m_Channels > 0u)
                   ? static_cast<float>(GetFrameCount()) / static_cast<float>(m_SampleRate)
                   : 0.0f;
    }
};

/// The single listener. ZEngine has no split-screen audio, so one is enough.
struct AudioListenerData
{
    AudioVec3 m_Position;
    /// Unit forward vector. Defaults to -Y (ZEngine's Z-up camera convention).
    AudioVec3 m_Forward {0.0f, -1.0f, 0.0f};
    /// Unit up vector. Defaults to +Z.
    AudioVec3 m_Up {0.0f, 0.0f, 1.0f};
    /// 0 = fully deaf, 1 = normal.
    float m_Gain {1.0f};
};

/// Per-playback settings. Mirrors Unity's AudioSource inspector minus the
/// fields that need a DSP graph (filters, reverb sends, doppler).
struct AudioPlayParams
{
    /// Linear gain, 0..N. >1 clips at the master stage (soft, via tanh).
    float m_Volume {1.0f};
    /// Playback rate multiplier; 1 = native pitch. Also rescales duration.
    float m_Pitch {1.0f};
    bool m_Loop {false};
    /// 0 = pure 2D (no attenuation, no panning), 1 = full 3D. Blends between.
    float m_SpatialBlend {0.0f};
    /// World position of the emitter. Only read when m_SpatialBlend > 0.
    AudioVec3 m_Position;
    float m_MinDistance {1.0f};
    float m_MaxDistance {50.0f};
    AudioDistanceModel m_DistanceModel {AudioDistanceModel::Inverse};
    /// Fade-in from silence, seconds. 0 = start immediately.
    float m_FadeInSeconds {0.0f};
    /// 延迟启动，秒（Unity 的 `AudioSource.PlayDelayed`）。0 = 立刻。
    ///
    /// 放在 params 而不是给 Play() 多加一个形参：`UpdateVoiceParams` 整块替换
    /// params，多一个形参的话组件每帧推参数时会把延迟一起抹掉。放进来之后它也
    /// 只是"新开一条声音时的设置" —— 播放中的声音改它没有意义，mixer 只在
    /// Play() 时读一次。
    float m_DelaySeconds {0.0f};
    /// Higher wins when the voice cap is hit.
    int32_t m_Priority {0};
    /// Output bus. kAudioMasterBusId unless the project routed it elsewhere.
    AudioBusId m_Bus {kAudioMasterBusId};
};

/// Runtime state of a voice, exposed for the Inspector / debugging.
enum class AudioVoiceState : uint8_t
{
    Playing = 0,
    Paused = 1,
    Stopped = 2
};

inline float ClampAudioFloat(float in_value, float in_min, float in_max)
{
    return in_value < in_min ? in_min : (in_value > in_max ? in_max : in_value);
}

// =============================================================================
// Which backend a single Play() actually goes through.
//
// The two backends address content differently and the difference is not a
// detail: the internal one plays a *decoded clip file*, Wwise plays a *named
// event* out of a loaded sound bank. There is no way to spell one in terms of
// the other, so a component that only knows about clip paths is silent the
// moment Wwise owns the output.
//
// That makes "which route" a real decision with three outcomes, and it is worth
// pulling out as a pure function: it is the part that can be wrong without any
// compiler or runtime complaining (the wrong branch is simply silent), and a
// pure function is the only shape in which it can be tested without a sound
// card, a bank or an initialised sound engine.
//
// `kNone` is deliberately not "try the other one": falling back to a clip when
// the event name is missing would play nothing through the null device and
// report success, which is exactly the silent failure this replaces.
// =============================================================================
enum class AudioPlayRoute : uint8_t
{
    /// Nothing can play: no route is configured for the active backend.
    kNone = 0,
    /// Internal backend: decode m_ClipPath into a mixer voice.
    kClipFile = 1,
    /// Wwise backend: post the named event on a registered game object.
    kWwiseEvent = 2
};

/// Picks the route for one Play() call. Pure -- same inputs, same answer.
///
/// `in_wwise_owns_output` is AudioSystem::IsWwiseActive(), passed in rather than
/// queried here so this stays dependency-free.
inline AudioPlayRoute ResolveAudioPlayRoute(bool in_wwise_owns_output,
                                            const char* in_clip_path,
                                            const char* in_wwise_event)
{
    const bool has_clip = (in_clip_path != nullptr) && (in_clip_path[0] != '\0');
    const bool has_event = (in_wwise_event != nullptr) && (in_wwise_event[0] != '\0');

    if (in_wwise_owns_output)
    {
        // A clip path is meaningless to Wwise: banks hold events, not files.
        return has_event ? AudioPlayRoute::kWwiseEvent : AudioPlayRoute::kNone;
    }
    // The internal backend never sees Wwise events.
    return has_clip ? AudioPlayRoute::kClipFile : AudioPlayRoute::kNone;
}
