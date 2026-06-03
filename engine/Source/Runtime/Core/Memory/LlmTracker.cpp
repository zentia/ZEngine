#include "LlmTracker.h"

#include <algorithm>
#include <cstring>

// Thread-local tag stack
thread_local LLMTracker::TagStack* LLMTracker::s_TagStack = nullptr;

SystemInitPhase LLMTracker::GetInitPhase() const
{
    return SystemInitPhase::PreInit;
}

LLMTracker::LLMTracker() = default;

LLMTracker::~LLMTracker() = default;

bool LLMTracker::Initialize()
{
    m_Enabled = true;
    return true;
}

void LLMTracker::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    m_TagStats.clear();
    m_Enabled = false;
}

LLMTracker::TagStack& LLMTracker::GetTagStack()
{
    if (s_TagStack == nullptr)
    {
        s_TagStack = new TagStack();
    }
    return *s_TagStack;
}

void LLMTracker::PushTag(const char* tag_name)
{
    if (!m_Enabled || tag_name == nullptr)
    {
        return;
    }

    TagStack& stack = GetTagStack();
    stack.tags.push_back(tag_name);
}

void LLMTracker::PopTag()
{
    if (!m_Enabled)
    {
        return;
    }

    TagStack& stack = GetTagStack();
    if (!stack.tags.empty())
    {
        stack.tags.pop_back();
    }
}

const char* LLMTracker::GetCurrentTag() const
{
    if (!m_Enabled || s_TagStack == nullptr)
    {
        return nullptr;
    }

    const TagStack& stack = *s_TagStack;
    if (stack.tags.empty())
    {
        return nullptr;
    }

    return stack.tags.back();
}

void LLMTracker::TrackAllocation(void* ptr, size_t size)
{
    if (!m_Enabled || ptr == nullptr || size == 0)
    {
        return;
    }

    const char* tag = GetCurrentTag();
    if (tag == nullptr)
    {
        tag = "Untagged";
    }

    UpdateStats(tag, size, true);
}

void LLMTracker::TrackDeallocation(void* ptr, size_t size)
{
    if (!m_Enabled || ptr == nullptr || size == 0)
    {
        return;
    }

    // For deallocation, we need to find which tag this allocation belonged to
    // Since we don't store the tag with each allocation, we'll use the current tag
    // or "Untagged" if no tag is active
    // Note: This is a limitation - for accurate tracking, we'd need to store
    // the tag with each allocation, which has overhead
    const char* tag = GetCurrentTag();
    if (tag == nullptr)
    {
        tag = "Untagged";
    }

    UpdateStats(tag, size, false);
}

void LLMTracker::UpdateStats(const char* tag_name, size_t size, bool is_allocation)
{
    if (tag_name == nullptr)
    {
        return;
    }

    std::string tag_str(tag_name);

    std::lock_guard<std::mutex> lock(m_StatsMutex);

    LLMTagStats& stats = m_TagStats[tag_str];

    if (is_allocation)
    {
        stats.tag_name = tag_str;
        stats.current_bytes += size;
        stats.total_allocated += size;
        stats.allocation_count++;

        if (stats.current_bytes > stats.peak_bytes)
        {
            stats.peak_bytes = stats.current_bytes;
        }
    }
    else
    {
        // For deallocation, we can't be 100% accurate without storing tags per allocation
        // But we'll still update the stats to track deallocation patterns
        if (stats.current_bytes >= size)
        {
            stats.current_bytes -= size;
        }
        else
        {
            // Underflow protection - this can happen if deallocation tag doesn't match allocation tag
            stats.current_bytes = 0;
        }
        stats.total_freed += size;
        stats.deallocation_count++;
    }
}

LLMTagStats LLMTracker::GetTagStats(const std::string& tag_name) const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);

    auto it = m_TagStats.find(tag_name);
    if (it != m_TagStats.end())
    {
        return it->second;
    }

    // Return empty stats if tag not found
    LLMTagStats empty_stats;
    empty_stats.tag_name = tag_name;
    return empty_stats;
}

std::vector<LLMTagStats> LLMTracker::GetAllTagStats() const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);

    std::vector<LLMTagStats> result;
    result.reserve(m_TagStats.size());

    for (const auto& pair : m_TagStats)
    {
        result.push_back(pair.second);
    }

    // Sort by current bytes (descending)
    std::sort(result.begin(), result.end(), [](const LLMTagStats& a, const LLMTagStats& b) {
        return a.current_bytes > b.current_bytes;
    });

    return result;
}

size_t LLMTracker::GetTotalCurrentBytes() const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);

    size_t total = 0;
    for (const auto& pair : m_TagStats)
    {
        total += pair.second.current_bytes;
    }
    return total;
}

size_t LLMTracker::GetTotalPeakBytes() const
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);

    size_t total = 0;
    for (const auto& pair : m_TagStats)
    {
        total += pair.second.peak_bytes;
    }
    return total;
}

void LLMTracker::ClearStats()
{
    std::lock_guard<std::mutex> lock(m_StatsMutex);
    m_TagStats.clear();
}

// LLMScope implementation
LLMScope::LLMScope(const char* tag_name)
    : m_TagName(tag_name)
{
    GET_SYSTEM(LLMTracker)->PushTag(m_TagName);
}

LLMScope::~LLMScope()
{
    GET_SYSTEM(LLMTracker)->PopTag();
}