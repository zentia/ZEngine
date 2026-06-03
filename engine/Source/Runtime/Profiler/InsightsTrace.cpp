#include "Runtime/Profiler/InsightsTrace.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace ZEngine
{
namespace Insights
{
namespace
{
// Trace epoch, fixed at first use. Times are reported relative to this so the
// numbers stay small enough for lossless double conversion in the UI.
const std::chrono::steady_clock::time_point& Epoch()
{
    static const std::chrono::steady_clock::time_point kEpoch = std::chrono::steady_clock::now();
    return kEpoch;
}

uint64_t CurrentThreadIdValue()
{
    return static_cast<uint64_t>(std::hash<std::thread::id> {}(std::this_thread::get_id()));
}
}  // namespace

uint64_t NowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Epoch()).count());
}

InsightsTrace& InsightsTrace::Get()
{
    static InsightsTrace instance;
    return instance;
}

// thread_local handle so the hot path never touches the global registry after
// the first scope on a thread. The ThreadStream itself is owned by m_Threads and
// lives for the process lifetime (engine threads are long-lived).
InsightsTrace::ThreadStream* InsightsTrace::GetTLS()
{
    thread_local ThreadStream* tls = nullptr;
    if (tls != nullptr)
        return tls;

    ThreadStream* stream = new ThreadStream();
    stream->thread_id = CurrentThreadIdValue();
    {
        std::lock_guard<std::mutex> lock(m_ThreadMutex);
        m_Threads.push_back(stream);
    }
    tls = stream;
    return tls;
}

InsightsTrace::ThreadStream* InsightsTrace::GetOrCreateNamedStream(const char* name)
{
    const std::string key = (name != nullptr) ? name : "";
    std::lock_guard<std::mutex> lock(m_ThreadMutex);
    auto it = m_NamedTracks.find(key);
    if (it != m_NamedTracks.end())
        return it->second;
    ThreadStream* stream = new ThreadStream();
    stream->name = key;
    stream->thread_id = 0;
    m_Threads.push_back(stream);
    m_NamedTracks.emplace(key, stream);
    return stream;
}

void InsightsTrace::PushExternalEvent(const char* track_name, const char* name, uint16_t depth, uint64_t start_ns,
                                      uint64_t end_ns)
{
    if (!IsCapturing())
        return;
    const uint32_t id = InternName(name);  // global lock first, then per-stream
    ThreadStream* stream = GetOrCreateNamedStream(track_name);
    std::lock_guard<std::mutex> lock(stream->mutex);
    ScopeEvent ev;
    ev.name_id = id;
    ev.depth = depth;
    ev.start_ns = start_ns;
    ev.end_ns = (end_ns > start_ns) ? end_ns : start_ns + 1;
    stream->events.push_back(ev);
}

uint32_t InsightsTrace::InternName(const char* name)
{
    const std::string key = (name != nullptr) ? name : "";
    std::lock_guard<std::mutex> lock(m_NameMutex);
    auto it = m_NameLookup.find(key);
    if (it != m_NameLookup.end())
        return it->second;
    const uint32_t id = static_cast<uint32_t>(m_Names.size());
    m_Names.push_back(key);
    m_NameLookup.emplace(key, id);
    return id;
}

void InsightsTrace::BeginScope(const char* name)
{
    if (!IsCapturing())
    {
        // Keep the open/close stack balanced even while paused so a later
        // EndScope matches the right Begin (LIFO). Untraced begins push -1.
        ThreadStream* stream = GetTLS();
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->open_stack.push_back(-1);
        return;
    }

    // Intern OUTSIDE the per-thread lock to keep a single lock order
    // (never thread-then-global; EndFrame/BuildSnapshot take them the other way).
    const uint32_t id = InternName(name);
    const uint64_t now = NowNs();

    ThreadStream* stream = GetTLS();
    std::lock_guard<std::mutex> lock(stream->mutex);
    ScopeEvent ev;
    ev.name_id = id;
    ev.depth = stream->open_traced;
    ev.start_ns = now;
    ev.end_ns = 0;
    const int64_t index = static_cast<int64_t>(stream->events.size());
    stream->events.push_back(ev);
    stream->open_stack.push_back(index);
    ++stream->open_traced;
}

void InsightsTrace::EndScope()
{
    const uint64_t now = NowNs();
    ThreadStream* stream = GetTLS();
    std::lock_guard<std::mutex> lock(stream->mutex);
    if (stream->open_stack.empty())
        return;
    const int64_t index = stream->open_stack.back();
    stream->open_stack.pop_back();
    if (index < 0)
        return;  // began while paused
    if (index < static_cast<int64_t>(stream->events.size()))
        stream->events[static_cast<size_t>(index)].end_ns = now;
    if (stream->open_traced > 0)
        --stream->open_traced;
}

void InsightsTrace::EndFrame()
{
    const uint64_t now = NowNs();

    // Resolve the capture state from the window heartbeat: ON while pulses are
    // fresh, OFF (frozen view) a couple frames after they stop.
    const int age = m_HeartbeatAge.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool capturing = age <= 2;
    m_Capturing.store(capturing, std::memory_order_relaxed);

    uint64_t cutoff = 0;
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        m_FrameStarts.push_back(now);
        const int retained = m_RetainedFrames > 1 ? m_RetainedFrames : 1;
        while (static_cast<int>(m_FrameStarts.size()) > retained + 1)
            m_FrameStarts.pop_front();
        cutoff = m_FrameStarts.front();
    }

    if (!capturing)
        return;

    // Snapshot the thread list under the registry lock, then prune each stream
    // under its own lock (never both locks at once -- matches BeginScope order).
    std::vector<ThreadStream*> threads;
    {
        std::lock_guard<std::mutex> lock(m_ThreadMutex);
        threads = m_Threads;
    }

    for (ThreadStream* stream : threads)
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        std::vector<ScopeEvent>& events = stream->events;
        // Erase only a leading run of fully-closed, old events. Stop at the first
        // event that is still open or newer than the cutoff so we never reorder.
        size_t prune = 0;
        while (prune < events.size())
        {
            const ScopeEvent& ev = events[prune];
            if (ev.end_ns == 0 || ev.end_ns >= cutoff)
                break;
            ++prune;
        }
        if (prune == 0)
            continue;
        events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(prune));
        // Open-stack indices point at surviving (open) events, all >= prune.
        const int64_t shift = static_cast<int64_t>(prune);
        for (int64_t& idx : stream->open_stack)
        {
            if (idx >= 0)
                idx -= shift;
        }
    }
}

void InsightsTrace::SetThreadName(const char* name)
{
    ThreadStream* stream = GetTLS();
    std::lock_guard<std::mutex> lock(stream->mutex);
    stream->name = (name != nullptr) ? name : "";
}

void InsightsTrace::SetRetainedFrames(int n)
{
    std::lock_guard<std::mutex> lock(m_FrameMutex);
    m_RetainedFrames = n > 1 ? n : 1;
}

void InsightsTrace::Clear()
{
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        m_FrameStarts.clear();
    }
    std::vector<ThreadStream*> threads;
    {
        std::lock_guard<std::mutex> lock(m_ThreadMutex);
        threads = m_Threads;
    }
    for (ThreadStream* stream : threads)
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->events.clear();
        stream->open_stack.clear();
        stream->open_traced = 0;
    }
}

void InsightsTrace::BuildSnapshot(InsightsSnapshot& out)
{
    out.tracks.clear();
    out.frame_starts.clear();
    out.names.clear();
    out.min_ns = 0;
    out.max_ns = 0;

    {
        std::lock_guard<std::mutex> lock(m_NameMutex);
        out.names = m_Names;
    }
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        out.frame_starts.assign(m_FrameStarts.begin(), m_FrameStarts.end());
    }

    std::vector<ThreadStream*> threads;
    {
        std::lock_guard<std::mutex> lock(m_ThreadMutex);
        threads = m_Threads;
    }

    bool has_range = false;
    uint64_t lo = 0;
    uint64_t hi = 0;
    for (ThreadStream* stream : threads)
    {
        TrackSnapshot track;
        {
            std::lock_guard<std::mutex> lock(stream->mutex);
            track.thread_name = stream->name;
            track.thread_id = stream->thread_id;
            track.events = stream->events;  // copy
        }
        if (track.thread_name.empty())
            track.thread_name = "Thread " + std::to_string(track.thread_id & 0xffffu);

        uint16_t max_depth = 0;
        for (const ScopeEvent& ev : track.events)
        {
            if (ev.depth > max_depth)
                max_depth = ev.depth;
            const uint64_t e = (ev.end_ns != 0) ? ev.end_ns : ev.start_ns;
            if (!has_range)
            {
                lo = ev.start_ns;
                hi = e;
                has_range = true;
            }
            else
            {
                if (ev.start_ns < lo)
                    lo = ev.start_ns;
                if (e > hi)
                    hi = e;
            }
        }
        track.max_depth = max_depth;
        if (!track.events.empty())
            out.tracks.push_back(std::move(track));
    }

    for (uint64_t f : out.frame_starts)
    {
        if (!has_range)
        {
            lo = f;
            hi = f;
            has_range = true;
        }
        else
        {
            if (f < lo)
                lo = f;
            if (f > hi)
                hi = f;
        }
    }

    out.min_ns = has_range ? lo : 0;
    out.max_ns = has_range ? hi : 0;
}

// --- .ztrace file I/O -------------------------------------------------------
namespace
{
// Explicit byte sequence 'Z','T','R','C' so the magic is stable on disk
// regardless of how an int literal would pack.
constexpr uint32_t kZtrcMagic = ('Z') | ('T' << 8) | ('R' << 16) | (static_cast<uint32_t>('C') << 24);
constexpr uint32_t kZtrcVersion = 1u;

template<typename T>
void WritePod(std::ostream& os, const T& value)
{
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteString(std::ostream& os, const std::string& s)
{
    const uint32_t len = static_cast<uint32_t>(s.size());
    WritePod(os, len);
    if (len > 0)
        os.write(s.data(), static_cast<std::streamsize>(len));
}

template<typename T>
bool ReadPod(std::istream& is, T& value)
{
    is.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(is);
}

bool ReadString(std::istream& is, std::string& out, uint32_t max_len)
{
    uint32_t len = 0;
    if (!ReadPod(is, len) || len > max_len)
        return false;
    out.resize(len);
    if (len > 0)
        is.read(out.data(), static_cast<std::streamsize>(len));
    return static_cast<bool>(is);
}
}  // namespace

bool SaveTrace(const std::string& path, const InsightsSnapshot& snapshot)
{
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os.is_open())
        return false;

    WritePod(os, kZtrcMagic);
    WritePod(os, kZtrcVersion);
    WritePod(os, snapshot.min_ns);
    WritePod(os, snapshot.max_ns);

    WritePod(os, static_cast<uint32_t>(snapshot.names.size()));
    for (const std::string& name : snapshot.names)
        WriteString(os, name);

    WritePod(os, static_cast<uint32_t>(snapshot.frame_starts.size()));
    for (uint64_t f : snapshot.frame_starts)
        WritePod(os, f);

    WritePod(os, static_cast<uint32_t>(snapshot.tracks.size()));
    for (const TrackSnapshot& track : snapshot.tracks)
    {
        WriteString(os, track.thread_name);
        WritePod(os, track.thread_id);
        WritePod(os, track.max_depth);
        WritePod(os, static_cast<uint32_t>(track.events.size()));
        for (const ScopeEvent& ev : track.events)
        {
            WritePod(os, ev.name_id);
            WritePod(os, ev.depth);
            WritePod(os, ev.start_ns);
            WritePod(os, ev.end_ns);
        }
    }

    return static_cast<bool>(os);
}

bool LoadTrace(const std::string& path, InsightsSnapshot& out)
{
    out.tracks.clear();
    out.frame_starts.clear();
    out.names.clear();
    out.min_ns = 0;
    out.max_ns = 0;

    std::ifstream is(path, std::ios::binary);
    if (!is.is_open())
        return false;

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!ReadPod(is, magic) || magic != kZtrcMagic)
        return false;
    if (!ReadPod(is, version) || version != kZtrcVersion)
        return false;
    if (!ReadPod(is, out.min_ns) || !ReadPod(is, out.max_ns))
        return false;

    // Sanity caps to avoid pathological allocations on a corrupt file.
    constexpr uint32_t kMaxCount = 50u * 1000u * 1000u;
    constexpr uint32_t kMaxStrLen = 1u << 20;

    uint32_t name_count = 0;
    if (!ReadPod(is, name_count) || name_count > kMaxCount)
        return false;
    out.names.reserve(name_count);
    for (uint32_t i = 0; i < name_count; ++i)
    {
        std::string name;
        if (!ReadString(is, name, kMaxStrLen))
            return false;
        out.names.push_back(std::move(name));
    }

    uint32_t frame_count = 0;
    if (!ReadPod(is, frame_count) || frame_count > kMaxCount)
        return false;
    out.frame_starts.reserve(frame_count);
    for (uint32_t i = 0; i < frame_count; ++i)
    {
        uint64_t f = 0;
        if (!ReadPod(is, f))
            return false;
        out.frame_starts.push_back(f);
    }

    uint32_t track_count = 0;
    if (!ReadPod(is, track_count) || track_count > kMaxCount)
        return false;
    out.tracks.reserve(track_count);
    for (uint32_t t = 0; t < track_count; ++t)
    {
        TrackSnapshot track;
        if (!ReadString(is, track.thread_name, kMaxStrLen))
            return false;
        if (!ReadPod(is, track.thread_id) || !ReadPod(is, track.max_depth))
            return false;
        uint32_t event_count = 0;
        if (!ReadPod(is, event_count) || event_count > kMaxCount)
            return false;
        track.events.reserve(event_count);
        for (uint32_t e = 0; e < event_count; ++e)
        {
            ScopeEvent ev;
            if (!ReadPod(is, ev.name_id) || !ReadPod(is, ev.depth) || !ReadPod(is, ev.start_ns) ||
                !ReadPod(is, ev.end_ns))
                return false;
            track.events.push_back(ev);
        }
        out.tracks.push_back(std::move(track));
    }

    return true;
}

}  // namespace Insights
}  // namespace ZEngine
