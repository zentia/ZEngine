#include "Runtime/Function/Audio/AudioDevice.h"

bool AudioDeviceNull::Open(uint32_t in_sample_rate, uint32_t in_channels, uint32_t in_buffer_frames, AudioRenderCallback in_callback)
{
    (void)in_callback;

    m_SampleRate = in_sample_rate;
    m_Channels = in_channels;
    m_BufferFrames = in_buffer_frames;
    return true;
}

bool AudioDeviceNull::Start()
{
    m_Running = true;
    return true;
}

void AudioDeviceNull::Stop()
{
    m_Running = false;
}

void AudioDeviceNull::Close()
{
    m_Running = false;
}

std::unique_ptr<IAudioDevice> CreateAudioDevice()
{
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return CreateWasapiAudioDevice();
#else
    return std::make_unique<AudioDeviceNull>();
#endif
}
