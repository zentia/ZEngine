#include "Profiler.h"

#include "ProfilerSharedMemory.h"

#include <algorithm>

Profiler::Profiler()
    : m_SharedMemoryEnabled(false)
{
    m_FrameStartTime = std::chrono::high_resolution_clock::now();
}

Profiler::~Profiler() = default;

void Profiler::BeginSample(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto& sample = m_ActiveSamples[name];
    sample.start_time = std::chrono::high_resolution_clock::now();
}

void Profiler::EndSample(const std::string& name)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_ActiveSamples.find(name);
    if (it == m_ActiveSamples.end())
    {
        return;  // Sample not started
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - it->second.start_time);
    float duration_ms = duration.count() / 1000.0f;

    // Store per-frame data (accumulate for multiple calls within the same frame)
    auto& frame_sample = m_FrameSamples[name];
    frame_sample.total_time_ms += duration_ms;
    frame_sample.call_count++;

    m_ActiveSamples.erase(it);
}

std::vector<ProfileSample> Profiler::GetResults() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<ProfileSample> results;
    results.reserve(m_FrameSamples.size());

    // Return current frame data only (not accumulated across frames)
    for (const auto& [name, data] : m_FrameSamples)
    {
        ProfileSample sample;
        sample.name = name;
        // Return total time for this frame (accumulated for multiple calls in same frame)
        sample.duration_ms = data.total_time_ms;
        // Return call count for this frame
        sample.call_count = data.call_count;
        results.push_back(sample);
    }

    // Sort by duration (descending)
    std::sort(results.begin(), results.end(), [](const ProfileSample& a, const ProfileSample& b) {
        return a.duration_ms > b.duration_ms;
    });

    return results;
}

void Profiler::clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ActiveSamples.clear();
    m_FrameSamples.clear();
    m_LastFrameTimeMs = 0.0f;
    m_FrameNumber = 0;
}

void Profiler::NextFrame()
{
    // Calculate frame time
    auto current_time = std::chrono::high_resolution_clock::now();
    auto frame_duration = std::chrono::duration_cast<std::chrono::microseconds>(current_time - m_FrameStartTime);
    const float last_frame_time_ms = frame_duration.count() / 1000.0f;

    std::vector<ProfileSample> results;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_LastFrameTimeMs = last_frame_time_ms;

        // Write to shared memory if enabled
        if (m_SharedMemoryEnabled && m_SharedMemory)
        {
            results.reserve(m_FrameSamples.size());
            for (const auto& [name, data] : m_FrameSamples)
            {
                ProfileSample sample;
                sample.name = name;
                sample.duration_ms = data.total_time_ms;
                sample.call_count = data.call_count;
                results.push_back(std::move(sample));
            }
            std::sort(results.begin(), results.end(), [](const ProfileSample& a, const ProfileSample& b) {
                return a.duration_ms > b.duration_ms;
            });
        }

        // Increment frame number for next frame
        m_FrameNumber++;

        // Clear frame samples for next frame
        m_FrameSamples.clear();
    }

    if (m_SharedMemoryEnabled && m_SharedMemory)
    {
        m_SharedMemory->WriteData(results, m_LastFrameTimeMs, m_FrameNumber - 1);
    }

    // Reset frame start time for next frame
    m_FrameStartTime = current_time;
}

void Profiler::EnableSharedMemory(bool enable)
{
    if (enable == m_SharedMemoryEnabled)
    {
        return;  // Already in desired state
    }

    m_SharedMemoryEnabled = enable;

    if (enable)
    {
        // Initialize shared memory as writer
        m_SharedMemory = std::make_unique<ProfilerSharedMemory>();
        if (!m_SharedMemory->InitializeAsWriter())
        {
            m_SharedMemory.reset();
            m_SharedMemoryEnabled = false;
        }
    }
    else
    {
        // Shutdown shared memory
        if (m_SharedMemory)
        {
            m_SharedMemory->Shutdown();
            m_SharedMemory.reset();
        }
    }
}