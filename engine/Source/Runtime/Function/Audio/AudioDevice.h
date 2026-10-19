#pragma once

#include "Runtime/Function/Audio/AudioCommon.h"

#include <functional>
#include <memory>

// =============================================================================
// Platform audio output (V1: WASAPI on Windows, Null elsewhere).
//
// The device owns a dedicated render thread and pulls frames from AudioSystem
// through a callback. Keeping the platform layer this thin means the mixer has
// no idea whether it is feeding WASAPI, CoreAudio, AAudio or nothing at all --
// the same shape Unreal's FAudioDevice + IAudioPlatformInterface uses.
//
// Buffer contract: float32, interleaved, in_channels channels, in_sample_rate Hz.
// The callback MUST fill every frame it is handed; it is given a zeroed buffer.
// =============================================================================

using AudioRenderCallback = std::function<void(float* out_frames, uint32_t in_frame_count, uint32_t in_channels, uint32_t in_sample_rate)>;

class IAudioDevice
{
public:
    virtual ~IAudioDevice() = default;

    /// Acquire the endpoint and allocate buffers. Does not start the thread.
    virtual bool Open(uint32_t in_sample_rate, uint32_t in_channels, uint32_t in_buffer_frames, AudioRenderCallback in_callback) = 0;

    /// Start rendering (spawns the render thread on platforms that need one).
    virtual bool Start() = 0;

    /// Stop rendering and join the render thread. Safe to call twice.
    virtual void Stop() = 0;

    /// Release the endpoint. Called from the destructor as well.
    virtual void Close() = 0;

    virtual bool IsRunning() const = 0;
    virtual const char* GetBackendName() const = 0;
    virtual uint32_t GetSampleRate() const = 0;
    virtual uint32_t GetChannels() const = 0;
};

/// Silence-only device: reports itself as running so the rest of the engine
/// behaves identically, but never invokes the callback. Used on platforms with
/// no backend yet (Linux/macOS/Android/Web) and when WASAPI init fails.
class AudioDeviceNull : public IAudioDevice
{
public:
    bool Open(uint32_t in_sample_rate, uint32_t in_channels, uint32_t in_buffer_frames, AudioRenderCallback in_callback) override;
    bool Start() override;
    void Stop() override;
    void Close() override;
    bool IsRunning() const override { return m_Running; }
    const char* GetBackendName() const override { return "Null"; }
    uint32_t GetSampleRate() const override { return m_SampleRate; }
    uint32_t GetChannels() const override { return m_Channels; }

private:
    uint32_t m_SampleRate {0u};
    uint32_t m_Channels {0u};
    uint32_t m_BufferFrames {0u};
    bool m_Running {false};
};

/// Windows-only. Declared here so the factory can stay platform-agnostic.
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
std::unique_ptr<IAudioDevice> CreateWasapiAudioDevice();
#endif

/// Picks the backend for the current platform. Never returns null.
std::unique_ptr<IAudioDevice> CreateAudioDevice();
