#pragma once

// ZEngine Insights -- a lightweight, Unreal-Insights-inspired CPU timing trace.
//
// Model (mirrors UE's Trace/TimingInsights at a small scale):
//   * Each OS thread streams hierarchical scope events (begin/end timestamps +
//     nesting depth) into its OWN buffer. Capture is lock-light: only the owning
//     thread appends, guarded by that thread's mutex (contended only by the
//     occasional UI snapshot / frame-prune on the main thread).
//   * Scope names are interned into a global string table so events store a
//     4-byte id instead of a string.
//   * Frame boundaries are recorded once per presented frame; the trace keeps a
//     sliding window of the last N frames (older events are pruned) so memory
//     stays bounded during a long session.
//   * The editor's Insights window pulls a copy via BuildSnapshot() and renders a
//     per-thread flame chart with a frame ruler on top.
//
// Capture is OPT-IN and OFF by default. The Insights window turns it on while
// visible (heartbeat), so a normal editor/runtime session pays only a single
// relaxed-bool check per Z_PROFILE_* scope when nothing is looking -- the
// ProfileScope RAII helper skips BeginScope/EndScope entirely when capture is off.

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZEngine
{
namespace Insights
{

// One hierarchical timing scope. Times are nanoseconds since the trace epoch
// (InsightsTrace start), kept as u64 so pixel math can use doubles without loss
// across a multi-hour session.
struct ScopeEvent
{
    uint32_t name_id {0};
    uint16_t depth {0};
    uint64_t start_ns {0};
    uint64_t end_ns {0};  // 0 while the scope is still open
};

// A read-only copy of one thread's events for the UI. Owned by InsightsSnapshot.
struct TrackSnapshot
{
    std::string thread_name;
    uint64_t thread_id {0};
    uint16_t max_depth {0};
    std::vector<ScopeEvent> events;  // time-ordered by start_ns
};

// A consistent copy of the trace at one instant, for rendering. Cheap to rebuild
// each frame (a few vector copies); the UI never touches live capture buffers.
struct InsightsSnapshot
{
    std::vector<TrackSnapshot> tracks;
    std::vector<uint64_t> frame_starts;  // boundary timestamps, ascending
    std::vector<std::string> names;      // index == ScopeEvent::name_id
    uint64_t min_ns {0};
    uint64_t max_ns {0};

    const std::string& Name(uint32_t id) const
    {
        static const std::string kEmpty;
        return id < names.size() ? names[id] : kEmpty;
    }
};

class InsightsTrace
{
public:
    static InsightsTrace& Get();

    // --- Capture gate -------------------------------------------------------
    // Capture is heartbeat-driven: the Insights window pulses RequestCapture()
    // every visible, non-paused frame; EndFrame() ages the heartbeat and turns
    // capture off automatically a couple frames after the pulses stop (window
    // closed, hidden, or paused). Default is OFF, so a session with the window
    // never opened pays only one relaxed-bool read per Z_PROFILE_* scope.
    void RequestCapture() { m_HeartbeatAge.store(0, std::memory_order_relaxed); }
    bool IsCapturing() const { return m_Capturing.load(std::memory_order_relaxed); }

    // Drop all recorded events + frame marks (keeps thread registration). Safe to
    // call from the main thread; per-thread buffers are cleared under their locks.
    void Clear();

    // --- Hot path (called from ProfileScope) --------------------------------
    void BeginScope(const char* name);
    void EndScope();

    // --- External tracks (GPU, etc.) ----------------------------------------
    // Append a fully-formed, pre-timed span to a track identified by name (not an
    // OS thread). Used by the RHI GPU profiler, whose timestamps are resolved a
    // few frames late and already converted to trace-ns via clock calibration.
    // `depth` is the caller-managed nesting level. Honours the capture gate.
    void PushExternalEvent(const char* track_name, const char* name, uint16_t depth, uint64_t start_ns,
                           uint64_t end_ns);

    // --- Per-frame / setup --------------------------------------------------
    // Records a frame boundary and prunes events older than the retained window.
    void EndFrame();
    // Names the CALLING thread's track (call once near thread entry).
    void SetThreadName(const char* name);
    void SetRetainedFrames(int n);

    // --- Read side (main thread) --------------------------------------------
    void BuildSnapshot(InsightsSnapshot& out);

private:
    InsightsTrace() = default;

    struct ThreadStream
    {
        std::mutex mutex;
        std::string name;
        uint64_t thread_id {0};
        std::vector<ScopeEvent> events;
        // Open-scope stack: index into `events`, or -1 for a scope that began
        // while capture was off (so its EndScope is a no-op). Strictly LIFO.
        std::vector<int64_t> open_stack;
        uint16_t open_traced {0};  // currently-open traced scopes (== next depth)
    };

    ThreadStream* GetTLS();
    ThreadStream* GetOrCreateNamedStream(const char* name);
    uint32_t InternName(const char* name);

    std::atomic<bool> m_Capturing {false};
    std::atomic<int> m_HeartbeatAge {1000};

    // Name table (global, guarded by m_NameMutex). Never shrinks.
    std::mutex m_NameMutex;
    std::vector<std::string> m_Names;
    std::unordered_map<std::string, uint32_t> m_NameLookup;

    // Thread registry (guarded by m_ThreadMutex). Streams live for process life.
    // Named (external) tracks live in m_Threads too so snapshot/prune treat them
    // uniformly; m_NamedTracks just indexes them by name for reuse.
    std::mutex m_ThreadMutex;
    std::vector<ThreadStream*> m_Threads;
    std::unordered_map<std::string, ThreadStream*> m_NamedTracks;

    // Frame ring (guarded by m_FrameMutex).
    std::mutex m_FrameMutex;
    std::deque<uint64_t> m_FrameStarts;
    int m_RetainedFrames {120};
};

// Nanoseconds since the trace epoch.
uint64_t NowNs();

// --- .ztrace file I/O -------------------------------------------------------
// A captured snapshot can be written to a self-contained binary `.ztrace` file
// and reopened (in-editor or by the standalone ZInsights.exe viewer). The format
// is a flat little-endian dump of InsightsSnapshot:
//
//   'ZTRC' (u32 magic) | u32 version | u64 min_ns | u64 max_ns
//   u32 name_count   then [ u32 len | bytes ]                  * name_count
//   u32 frame_count  then [ u64 frame_start ]                  * frame_count
//   u32 track_count  then per track:
//       u32 name_len | name bytes | u64 thread_id | u16 max_depth
//       u32 event_count then [ u32 name_id | u16 depth | u64 start_ns | u64 end_ns ] * event_count
//
// Returns false on any I/O error (the caller logs / reports). Both functions are
// free of engine-system dependencies so the standalone viewer can call them
// without START_SYSTEM.
bool SaveTrace(const std::string& path, const InsightsSnapshot& snapshot);
bool LoadTrace(const std::string& path, InsightsSnapshot& out);

}  // namespace Insights
}  // namespace ZEngine
