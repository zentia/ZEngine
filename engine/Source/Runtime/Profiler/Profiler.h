#pragma once

#include "Runtime/Profiler/InsightsTrace.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ProfileSample
{
    std::string name;
    float duration_ms;
    int call_count;
};

// Forward declaration
class ProfilerSharedMemory;

class Profiler
{
public:
    static Profiler& GetInstance()
    {
        static Profiler instance;
        return instance;
    }

    // Start profiling a scope
    void BeginSample(const std::string& name);

    // End profiling a scope
    void EndSample(const std::string& name);

    // Get profiling results
    std::vector<ProfileSample> GetResults() const;

    // Clear all profiling data
    void clear();

    // Get the last frame's total time
    float getLastFrameTime() const { return m_LastFrameTimeMs; }

    // Update frame (call this once per frame)
    void NextFrame();

    // Enable/disable shared memory communication
    void EnableSharedMemory(bool enable);

private:
    Profiler();
    ~Profiler();
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    struct SampleData
    {
        std::chrono::high_resolution_clock::time_point start_time;
        float total_time_ms {0.0f};
        int call_count {0};
    };

    std::unordered_map<std::string, SampleData> m_ActiveSamples;
    std::unordered_map<std::string, SampleData> m_FrameSamples;
    mutable std::mutex m_Mutex;
    float m_LastFrameTimeMs {0.0f};
    std::chrono::high_resolution_clock::time_point m_FrameStartTime;
    std::uint32_t m_FrameNumber {0};  // Frame counter

    // Shared memory for inter-process communication
    std::unique_ptr<ProfilerSharedMemory> m_SharedMemory;
    bool m_SharedMemoryEnabled;
};

// RAII helper for always-on instrumentation. Feeds ONLY the ZEngine Insights
// timeline trace (the Unreal-Insights-style flame chart). When the Insights
// window is closed (capture off) the entire scope is a single relaxed-bool read
// in the constructor and a branch in the destructor -- no string copy, no lock,
// no clock read -- so leaving Z_PROFILE_* macros compiled in costs nothing.
//
// The legacy `Profiler` aggregator is intentionally NOT fed here anymore: nothing
// consumes its results (the old ImGui status bar is gone) and its single global
// mutex serialized every profiled scope across the main + render threads, which
// dominated frame time. It is kept as a class only for any external callers.
//
// `m_Traced` snapshots the capture state at construction so an EndScope is issued
// iff the matching BeginScope recorded an event; this keeps the per-thread
// open-scope stack balanced even if capture toggles mid-scope.
class ProfileScope
{
public:
    explicit ProfileScope(const char* name)
        : m_Traced(ZEngine::Insights::InsightsTrace::Get().IsCapturing())
    {
        if (m_Traced)
            ZEngine::Insights::InsightsTrace::Get().BeginScope(name);
    }

    explicit ProfileScope(const std::string& name)
        : ProfileScope(name.c_str())
    {
    }

    ~ProfileScope()
    {
        if (m_Traced)
            ZEngine::Insights::InsightsTrace::Get().EndScope();
    }

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    bool m_Traced;
};

// Macro for easy profiling
#define Z_PROFILE_SCOPE(name) ProfileScope _profile_scope(name)
#define Z_PROFILE_FUNCTION()  ProfileScope _profile_scope(__FUNCTION__)