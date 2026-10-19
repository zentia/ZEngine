// =============================================================================
// Audio vertical slice smoke test (PR-A1).
//
// Runs the DSP layer (decoder + mixer) with no engine, no device and no window,
// so it can be executed in CI on a headless machine:
//
//   cmake -B build -DZENGINE_BUILD_AUDIO_SMOKE_TEST=ON
//   cmake --build build --target AudioMixerSmokeTest
//   ./build/.../AudioMixerSmokeTest
//
// Scenarios:
//   A1  WAV encode -> decode round-trip (synthesis + format coverage)
//   A2  Decode rejects garbage / unsupported extensions
//   A3  Mono clip renders to both channels with the expected peak
//   A4  Spatial blend: panning to the right, distance attenuation
//   A5  Loop keeps a voice alive past the clip end; no-loop retires it
//   A6  Voice cap is enforced (stealing, not unbounded growth)
//   A7  Bus volume and bus pause
//   A8  Fade-out then auto-removal
//   A9  Pitch rescales playback rate (duration halves at pitch 2)
// =============================================================================

#include "Runtime/Function/Audio/AudioCommon.h"
#include "Runtime/Function/Audio/AudioDecoder.h"
#include "Runtime/Function/Audio/AudioMixer.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int g_Failures = 0;
    int g_Checks = 0;

    void Check(bool in_condition, const char* in_message)
    {
        ++g_Checks;
        if (!in_condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", in_message);
        }
        else
        {
            std::printf("[ ok ] %s\n", in_message);
        }
    }

    void CheckNear(float in_actual, float in_expected, float in_tolerance, const char* in_message)
    {
        ++g_Checks;
        const float delta = std::fabs(in_actual - in_expected);
        if (delta > in_tolerance)
        {
            ++g_Failures;
            std::printf("[FAIL] %s (actual %.5f, expected %.5f +/- %.5f)\n", in_message, in_actual, in_expected, in_tolerance);
        }
        else
        {
            std::printf("[ ok ] %s (%.5f)\n", in_message, in_actual);
        }
    }

    /// Sine sweep generator: 1 second, target sample rate, target channels.
    AudioClipData MakeSineClip(float in_frequency, uint32_t in_sample_rate, uint32_t in_channels, float in_amplitude, float in_seconds)
    {
        AudioClipData clip;
        clip.m_SampleRate = in_sample_rate;
        clip.m_Channels = in_channels;
        const uint64_t frame_count = static_cast<uint64_t>(in_seconds * static_cast<float>(in_sample_rate));
        clip.m_Samples.resize(static_cast<size_t>(frame_count) * in_channels);
        for (uint64_t frame = 0u; frame < frame_count; ++frame)
        {
            const float phase = 2.0f * 3.14159265358979323846f * in_frequency *
                                (static_cast<float>(frame) / static_cast<float>(in_sample_rate));
            const float value = std::sin(phase) * in_amplitude;
            for (uint32_t channel = 0u; channel < in_channels; ++channel)
            {
                clip.m_Samples[static_cast<size_t>(frame * in_channels + channel)] = value;
            }
        }
        clip.m_Name = "sine";
        return clip;
    }

    float Peak(const std::vector<float>& in_buffer)
    {
        float peak = 0.0f;
        for (float sample : in_buffer)
        {
            peak = std::max(peak, std::fabs(sample));
        }
        return peak;
    }

    float PeakChannel(const std::vector<float>& in_buffer, uint32_t in_channels, uint32_t in_channel)
    {
        float peak = 0.0f;
        for (size_t frame = 0u; frame * in_channels < in_buffer.size(); ++frame)
        {
            peak = std::max(peak, std::fabs(in_buffer[frame * in_channels + in_channel]));
        }
        return peak;
    }

    std::vector<float> RenderFrames(AudioMixer& in_mixer, uint32_t in_frame_count, uint32_t in_sample_rate, uint32_t in_channels)
    {
        std::vector<float> buffer(static_cast<size_t>(in_frame_count) * in_channels, 0.0f);
        in_mixer.Render(buffer.data(), in_frame_count, in_channels, in_sample_rate);
        return buffer;
    }
}  // namespace

int main()
{
    // ---- A1: WAV round-trip ----
    {
        const AudioClipData source = MakeSineClip(440.0f, 44100u, 2u, 0.5f, 0.25f);
        std::vector<uint8_t> encoded;
        std::string error;
        Check(EncodeWavS16(source, encoded, &error), "A1: encode sine clip to 16-bit WAV");

        AudioClipData decoded;
        Check(DecodeWav(encoded.data(), encoded.size(), decoded, &error), "A1: decode the WAV back");
        Check(decoded.m_SampleRate == 44100u && decoded.m_Channels == 2u, "A1: sample rate and channel count survive");
        Check(decoded.GetFrameCount() == source.GetFrameCount(), "A1: frame count survives");
        CheckNear(Peak(decoded.m_Samples), 0.5f, 0.02f, "A1: peak amplitude survives 16-bit quantisation");
    }

    // ---- A2: rejection paths ----
    {
        std::string error;
        AudioClipData clip;

        const std::vector<uint8_t> garbage(64u, 0u);
        Check(!DecodeWav(garbage.data(), garbage.size(), clip, &error), "A2: garbage buffer rejected");
        Check(!error.empty(), "A2: rejection reports a reason");

        const AudioClipData sine = MakeSineClip(220.0f, 8000u, 1u, 0.25f, 0.05f);
        std::vector<uint8_t> wav_bytes;
        EncodeWavS16(sine, wav_bytes, nullptr);
        Check(!DecodeAudioBuffer(".ogg", wav_bytes.data(), wav_bytes.size(), clip, &error), "A2: .ogg reported unsupported (V1)");
        Check(DecodeAudioBuffer(".wav", wav_bytes.data(), wav_bytes.size(), clip, nullptr), "A2: .wav accepted");
    }

    // ---- A3: mono -> stereo output ----
    {
        AudioMixer mixer;
        const AudioClipData clip = MakeSineClip(1000.0f, 48000u, 1u, 0.8f, 0.5f);

        AudioPlayParams params;
        params.m_Volume = 1.0f;
        params.m_SpatialBlend = 0.0f;
        const AudioVoiceId voice = mixer.Play(&clip, params);
        Check(voice != kInvalidAudioVoiceId, "A3: Play returns a valid voice id");

        const std::vector<float> stereo = RenderFrames(mixer, 1024u, 48000u, 2u);
        const float left = PeakChannel(stereo, 2u, 0u);
        const float right = PeakChannel(stereo, 2u, 1u);
        Check(left > 0.05f, "A3: rendered signal is audible");
        CheckNear(left, right, 1.0e-4f, "A3: mono clip is centred (L == R)");
        Check(Peak(stereo) <= 1.0f, "A3: output never exceeds full scale");

        const std::vector<float> mono = RenderFrames(mixer, 256u, 48000u, 1u);
        Check(Peak(mono) > 0.05f, "A3: mono device layout also renders");
    }

    // ---- A4: spatial blend ----
    {
        AudioMixer mixer;
        AudioListenerData listener;
        listener.m_Position = AudioVec3(0.0f, 0.0f, 0.0f);
        // Z-up engine: forward = -Y, up = +Z  =>  right = forward x up = (-Y) x (+Z) = -X
        listener.m_Forward = AudioVec3(0.0f, -1.0f, 0.0f);
        listener.m_Up = AudioVec3(0.0f, 0.0f, 1.0f);
        mixer.SetListener(listener);

        const AudioClipData clip = MakeSineClip(800.0f, 48000u, 1u, 0.9f, 0.5f);

        AudioPlayParams near_right;
        near_right.m_SpatialBlend = 1.0f;
        near_right.m_Position = AudioVec3(-5.0f, 0.0f, 0.0f);  // -X == listener's right
        near_right.m_MinDistance = 1.0f;
        near_right.m_MaxDistance = 100.0f;
        mixer.Play(&clip, near_right);
        const std::vector<float> panned = RenderFrames(mixer, 2048u, 48000u, 2u);
        const float panned_left = PeakChannel(panned, 2u, 0u);
        const float panned_right = PeakChannel(panned, 2u, 1u);
        Check(panned_right > panned_left * 1.2f, "A4: emitter on the listener's right pans right");

        AudioMixer far_mixer;
        far_mixer.SetListener(listener);
        AudioPlayParams far_params;
        far_params.m_SpatialBlend = 1.0f;
        far_params.m_Position = AudioVec3(0.0f, -40.0f, 0.0f);  // straight ahead, far away
        far_params.m_MinDistance = 1.0f;
        far_params.m_MaxDistance = 100.0f;
        far_mixer.Play(&clip, far_params);
        const std::vector<float> attenuated = RenderFrames(far_mixer, 2048u, 48000u, 2u);
        Check(Peak(attenuated) < Peak(panned) * 0.5f, "A4: distant emitter is attenuated");
    }

    // ---- A5: loop vs one-shot ----
    {
        const AudioClipData clip = MakeSineClip(500.0f, 48000u, 1u, 0.7f, 0.05f);  // 2400 frames

        AudioMixer looping;
        AudioPlayParams loop_params;
        loop_params.m_Loop = true;
        looping.Play(&clip, loop_params);
        RenderFrames(looping, 4096u, 48000u, 2u);  // longer than the clip
        Check(looping.GetActiveVoiceCount() == 1u, "A5: looping voice survives past the clip end");
        Check(Peak(RenderFrames(looping, 512u, 48000u, 2u)) > 0.05f, "A5: looping voice still produces sound");

        AudioMixer once;
        AudioPlayParams once_params;
        once_params.m_Loop = false;
        once.Play(&clip, once_params);
        RenderFrames(once, 4096u, 48000u, 2u);
        Check(once.GetActiveVoiceCount() == 0u, "A5: one-shot voice is retired at the clip end");
    }

    // ---- A6: voice cap ----
    {
        AudioMixer mixer;
        const AudioClipData clip = MakeSineClip(300.0f, 48000u, 1u, 0.2f, 2.0f);
        for (uint32_t index = 0u; index < kAudioMaxVoices + 32u; ++index)
        {
            AudioPlayParams params;
            params.m_Priority = static_cast<int32_t>(index);
            mixer.Play(&clip, params);
        }
        Check(mixer.GetActiveVoiceCount() <= kAudioMaxVoices, "A6: voice count never exceeds the cap");
        Check(mixer.GetActiveVoiceCount() == kAudioMaxVoices, "A6: cap is filled rather than dropped");
    }

    // ---- A7: buses ----
    {
        AudioMixer mixer;
        const AudioClipData clip = MakeSineClip(700.0f, 48000u, 1u, 0.6f, 1.0f);

        const AudioBusId music = mixer.CreateBus("Music");
        Check(music != kInvalidAudioBusId && music != kAudioMasterBusId, "A7: bus created with its own id");
        Check(mixer.FindBus("Music") == music, "A7: bus is findable by name");
        Check(mixer.CreateBus("Music") == music, "A7: duplicate bus name is idempotent");

        AudioPlayParams params;
        params.m_Bus = music;
        mixer.Play(&clip, params);
        const float full = Peak(RenderFrames(mixer, 1024u, 48000u, 2u));

        mixer.SetBusVolume(music, 0.5f);
        const float halved = Peak(RenderFrames(mixer, 1024u, 48000u, 2u));
        CheckNear(halved, full * 0.5f, full * 0.08f, "A7: bus volume scales the voice");

        mixer.SetBusPaused(music, true);
        Check(Peak(RenderFrames(mixer, 512u, 48000u, 2u)) == 0.0f, "A7: paused bus renders silence");
    }

    // ---- A8: fade-out then removal ----
    {
        AudioMixer mixer;
        const AudioClipData clip = MakeSineClip(600.0f, 48000u, 1u, 0.8f, 5.0f);
        AudioPlayParams params;
        params.m_Loop = true;
        const AudioVoiceId voice = mixer.Play(&clip, params);
        RenderFrames(mixer, 1024u, 48000u, 2u);

        mixer.Stop(voice, 0.05f);  // 50 ms fade => ~2400 frames at 48 kHz
        float level = 1.0f;
        for (int iteration = 0; iteration < 40; ++iteration)
        {
            level = Peak(RenderFrames(mixer, 128u, 48000u, 2u));
        }
        Check(mixer.GetActiveVoiceCount() == 0u, "A8: faded-out voice is removed");
        CheckNear(level, 0.0f, 1.0e-5f, "A8: fade-out reaches silence");
    }

    // ---- A9: pitch rescales playback rate ----
    {
        const AudioClipData clip = MakeSineClip(400.0f, 48000u, 1u, 0.7f, 0.25f);  // 12000 frames

        AudioMixer normal;
        normal.Play(&clip, AudioPlayParams());
        RenderFrames(normal, 12000u, 48000u, 2u);
        const bool normal_done = normal.GetActiveVoiceCount() == 0u;

        AudioMixer fast;
        AudioPlayParams fast_params;
        fast_params.m_Pitch = 2.0f;
        fast.Play(&clip, fast_params);
        RenderFrames(fast, 6000u, 48000u, 2u);
        const bool fast_done = fast.GetActiveVoiceCount() == 0u;

        Check(normal_done, "A9: clip ends after its own frame count at pitch 1");
        Check(fast_done, "A9: clip ends twice as early at pitch 2");
    }

    std::printf("\nAudio smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
