#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include "Runtime/Function/Audio/AudioDevice.h"

#include "Runtime/Core/Base/Macro.h"

#define WIN32_LEAN_AND_MEAN
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

// =============================================================================
// WASAPI shared-mode render device.
//
// Why shared mode: exclusive mode would give us the lowest possible latency but
// steals the device from everything else on the machine, which is wrong for an
// editor that also plays the system's notifications. Shared mode with a
// ~10 ms event-driven buffer is what most engines ship as their default.
//
// Format strategy: ask for IEEE float first (the mixer's native format). If the
// endpoint rejects it, fall back to the device mix format and convert in the
// render loop -- supporting 16-bit PCM there costs ~20 lines and covers every
// endpoint that refuses float.
// =============================================================================

namespace
{
    constexpr uint32_t kWasapiMinBufferFrames = 48u;  // ~1 ms at 48 kHz
}  // namespace

class AudioDeviceWasapi : public IAudioDevice
{
public:
    ~AudioDeviceWasapi() override { Close(); }

    bool Open(uint32_t in_sample_rate, uint32_t in_channels, uint32_t in_buffer_frames, AudioRenderCallback in_callback) override;
    bool Start() override;
    void Stop() override;
    void Close() override;

    bool IsRunning() const override { return m_Running; }
    const char* GetBackendName() const override { return "WASAPI"; }
    uint32_t GetSampleRate() const override { return m_SampleRate; }
    uint32_t GetChannels() const override { return m_Channels; }

private:
    void RenderLoop();

    AudioRenderCallback m_Callback;

    IMMDeviceEnumerator* m_Enumerator {nullptr};
    IMMDevice* m_Device {nullptr};
    IAudioClient* m_Client {nullptr};
    IAudioRenderClient* m_RenderClient {nullptr};
    HANDLE m_Event {nullptr};

    std::thread m_Thread;
    std::atomic<bool> m_StopRequested {false};
    std::atomic<bool> m_Running {false};
    /// Only used when the endpoint refuses IEEE float.
    std::vector<float> m_Scratch;

    uint32_t m_SampleRate {0u};
    uint32_t m_Channels {0u};
    uint32_t m_BufferFrameCount {0u};
    uint32_t m_FrameBytes {0u};
    bool m_IsFloatFormat {true};
    bool m_ComInitialized {false};
};

bool AudioDeviceWasapi::Open(uint32_t in_sample_rate, uint32_t in_channels, uint32_t in_buffer_frames, AudioRenderCallback in_callback)
{
    if (m_Running)
    {
        return false;
    }

    m_Callback = std::move(in_callback);
    if (!m_Callback)
    {
        LOG_ERROR(ZAudio, "WASAPI: Open() called without a render callback");
        return false;
    }

    // MTA is what we want: COM is initialised process-wide, so the render
    // thread does not have to initialise it again. RPC_E_CHANGED_MODE means
    // somebody already picked a different apartment -- not fatal for us.
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_ComInitialized = SUCCEEDED(com_hr);

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&m_Enumerator));
    if (FAILED(hr) || m_Enumerator == nullptr)
    {
        LOG_ERROR(ZAudio, "WASAPI: cannot create MMDeviceEnumerator (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    hr = m_Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_Device);
    if (FAILED(hr) || m_Device == nullptr)
    {
        LOG_ERROR(ZAudio, "WASAPI: no default render endpoint (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    hr = m_Device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_Client));
    if (FAILED(hr) || m_Client == nullptr)
    {
        LOG_ERROR(ZAudio, "WASAPI: IAudioClient activation failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    WAVEFORMATEX requested {};
    requested.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    requested.nChannels = static_cast<WORD>(in_channels);
    requested.nSamplesPerSec = in_sample_rate;
    requested.wBitsPerSample = 32;
    requested.nBlockAlign = static_cast<WORD>(in_channels * sizeof(float));
    requested.nAvgBytesPerSec = in_sample_rate * requested.nBlockAlign;
    requested.cbSize = 0;

    WAVEFORMATEX format = requested;
    WAVEFORMATEX* closest = nullptr;
    hr = m_Client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &requested, &closest);

    if (hr == S_OK)
    {
        format = requested;
    }
    else if (hr == S_FALSE && closest != nullptr)
    {
        // Closest match is usually the device mix format at a supported rate.
        format = *closest;
    }
    else
    {
        // Hard failure: try the mix format explicitly before giving up.
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(m_Client->GetMixFormat(&mix)) && mix != nullptr)
        {
            format = *mix;
            CoTaskMemFree(mix);
        }
    }

    if (closest != nullptr)
    {
        CoTaskMemFree(closest);
        closest = nullptr;
    }

    m_IsFloatFormat = (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (!m_IsFloatFormat && format.wFormatTag != WAVE_FORMAT_PCM)
    {
        LOG_ERROR(ZAudio, "WASAPI: endpoint offers neither IEEE float nor PCM (tag {})", static_cast<uint32_t>(format.wFormatTag));
        Close();
        return false;
    }

    m_SampleRate = format.nSamplesPerSec;
    m_Channels = format.nChannels;
    if (m_SampleRate == 0u || m_Channels == 0u)
    {
        LOG_ERROR(ZAudio, "WASAPI: endpoint reported an invalid format");
        Close();
        return false;
    }
    m_FrameBytes = m_Channels * (m_IsFloatFormat ? sizeof(float) : sizeof(int16_t));

    // Buffer duration in 100-nanosecond units, matching the requested frame
    // count (so AudioSystem's latency budget is honoured when possible).
    double requested_frames = static_cast<double>(in_buffer_frames > 0u ? in_buffer_frames : kWasapiMinBufferFrames);
    REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(requested_frames * 10000000.0 / static_cast<double>(m_SampleRate));
    if (duration <= 0)
    {
        duration = static_cast<REFERENCE_TIME>(kWasapiMinBufferFrames * 10000000.0 / static_cast<double>(m_SampleRate));
    }

    hr = m_Client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, &format, nullptr);
    if (FAILED(hr))
    {
        LOG_ERROR(ZAudio, "WASAPI: IAudioClient::Initialize failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    m_Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_Event == nullptr)
    {
        LOG_ERROR(ZAudio, "WASAPI: cannot create the render event");
        Close();
        return false;
    }

    hr = m_Client->SetEventHandle(m_Event);
    if (FAILED(hr))
    {
        LOG_ERROR(ZAudio, "WASAPI: SetEventHandle failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    hr = m_Client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_RenderClient));
    if (FAILED(hr) || m_RenderClient == nullptr)
    {
        LOG_ERROR(ZAudio, "WASAPI: cannot query IAudioRenderClient (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    hr = m_Client->GetBufferSize(&m_BufferFrameCount);
    if (FAILED(hr) || m_BufferFrameCount == 0u)
    {
        LOG_ERROR(ZAudio, "WASAPI: GetBufferSize failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        Close();
        return false;
    }

    LOG_INFO(ZAudio,
             "WASAPI device opened: {} Hz, {} channel(s), {} frame buffer, {}",
             m_SampleRate,
             m_Channels,
             m_BufferFrameCount,
             m_IsFloatFormat ? "float32" : "pcm16");
    return true;
}

bool AudioDeviceWasapi::Start()
{
    if (m_Running || m_Client == nullptr)
    {
        return false;
    }

    const HRESULT hr = m_Client->Start();
    if (FAILED(hr))
    {
        LOG_ERROR(ZAudio, "WASAPI: IAudioClient::Start failed (hr=0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }

    m_StopRequested = false;
    m_Running = true;
    m_Thread = std::thread(&AudioDeviceWasapi::RenderLoop, this);
    return true;
}

void AudioDeviceWasapi::Stop()
{
    if (m_Client != nullptr && m_Running)
    {
        m_Client->Stop();
    }

    m_StopRequested = true;
    if (m_Event != nullptr)
    {
        // Wake the render thread out of its wait so the join cannot hang.
        SetEvent(m_Event);
    }

    if (m_Thread.joinable())
    {
        m_Thread.join();
    }

    m_Running = false;
}

void AudioDeviceWasapi::Close()
{
    Stop();

    if (m_RenderClient != nullptr)
    {
        m_RenderClient->Release();
        m_RenderClient = nullptr;
    }
    if (m_Client != nullptr)
    {
        m_Client->Release();
        m_Client = nullptr;
    }
    if (m_Device != nullptr)
    {
        m_Device->Release();
        m_Device = nullptr;
    }
    if (m_Enumerator != nullptr)
    {
        m_Enumerator->Release();
        m_Enumerator = nullptr;
    }
    if (m_Event != nullptr)
    {
        CloseHandle(m_Event);
        m_Event = nullptr;
    }

    if (m_ComInitialized)
    {
        CoUninitialize();
        m_ComInitialized = false;
    }
}

void AudioDeviceWasapi::RenderLoop()
{
    while (!m_StopRequested.load())
    {
        const DWORD wait_result = WaitForSingleObject(m_Event, 100u);
        if (m_StopRequested.load())
        {
            break;
        }
        if (wait_result != WAIT_OBJECT_0)
        {
            continue;
        }

        UINT32 padding = 0u;
        if (FAILED(m_Client->GetCurrentPadding(&padding)))
        {
            continue;
        }

        const UINT32 available = (m_BufferFrameCount > padding) ? (m_BufferFrameCount - padding) : 0u;
        if (available == 0u)
        {
            continue;
        }

        BYTE* buffer = nullptr;
        if (FAILED(m_RenderClient->GetBuffer(available, &buffer)) || buffer == nullptr)
        {
            continue;
        }

        const size_t frame_bytes = m_FrameBytes;
        std::memset(buffer, 0, static_cast<size_t>(available) * frame_bytes);

        if (m_IsFloatFormat)
        {
            m_Callback(reinterpret_cast<float*>(buffer), available, m_Channels, m_SampleRate);
        }
        else
        {
            // PCM16 endpoint: mix in float, then convert on the way out.
            m_Scratch.resize(static_cast<size_t>(available) * m_Channels);
            std::fill(m_Scratch.begin(), m_Scratch.end(), 0.0f);
            m_Callback(m_Scratch.data(), available, m_Channels, m_SampleRate);

            int16_t* out_samples = reinterpret_cast<int16_t*>(buffer);
            for (size_t index = 0u; index < m_Scratch.size(); ++index)
            {
                const float clamped = ClampAudioFloat(m_Scratch[index], -1.0f, 1.0f);
                out_samples[index] = static_cast<int16_t>(clamped * 32767.0f);
            }
        }

        m_RenderClient->ReleaseBuffer(available, 0);
    }
}

std::unique_ptr<IAudioDevice> CreateWasapiAudioDevice()
{
    return std::make_unique<AudioDeviceWasapi>();
}

#endif  // defined(_WIN32) && !defined(__EMSCRIPTEN__)
