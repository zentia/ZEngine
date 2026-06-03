#pragma once

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration
class LLMTracker;

// Memory statistics for a specific tag
struct LLMTagStats
{
    std::string tag_name;
    size_t current_bytes {0};       // Current allocated bytes
    size_t peak_bytes {0};          // Peak allocated bytes
    size_t total_allocated {0};     // Total bytes allocated (cumulative)
    size_t total_freed {0};         // Total bytes freed (cumulative)
    size_t allocation_count {0};    // Number of allocations
    size_t deallocation_count {0};  // Number of deallocations
};

// RAII class for LLM scope tracking
class LLMScope
{
public:
    explicit LLMScope(const char* tag_name);
    ~LLMScope();

    // Non-copyable, non-movable
    LLMScope(const LLMScope&) = delete;
    LLMScope& operator=(const LLMScope&) = delete;
    LLMScope(LLMScope&&) = delete;
    LLMScope& operator=(LLMScope&&) = delete;

private:
    const char* m_TagName;
    LLMScope* m_PreviousScope;
};

// Low-Level Memory Tracker
class LLMTracker : public IEngineSystem
{
public:
    LLMTracker();
    ~LLMTracker();
    SystemInitPhase GetInitPhase() const override;
    // Initialize/Shutdown
    bool Initialize() override;
    void Shutdown() override;

    // Push/Pop tag scope (called by LLMScope)
    void PushTag(const char* tag_name);
    void PopTag();

    // Get current active tag
    const char* GetCurrentTag() const;

    // Track memory allocation
    void TrackAllocation(void* ptr, size_t size);

    // Track memory deallocation
    void TrackDeallocation(void* ptr, size_t size);

    // Get statistics for a specific tag
    LLMTagStats GetTagStats(const std::string& tag_name) const;

    // Get all tag statistics
    std::vector<LLMTagStats> GetAllTagStats() const;

    // Get total memory usage across all tags
    size_t GetTotalCurrentBytes() const;
    size_t GetTotalPeakBytes() const;

    // Clear all statistics
    void ClearStats();

    // Enable/Disable tracking
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    bool isEnabled() const { return m_Enabled; }

private:
    LLMTracker(const LLMTracker&) = delete;
    LLMTracker& operator=(const LLMTracker&) = delete;

    // Thread-local tag stack
    struct TagStack
    {
        std::vector<const char*> tags;
    };

    // Get thread-local tag stack
    TagStack& GetTagStack();

    // Update statistics (thread-safe)
    void UpdateStats(const char* tag_name, size_t size, bool is_allocation);

    mutable std::mutex m_StatsMutex;
    std::unordered_map<std::string, LLMTagStats> m_TagStats;
    std::atomic<bool> m_Enabled {true};
    static thread_local TagStack* s_TagStack;
};

// Macro for LLM scope tracking (similar to Unreal's LLM_SCOPE_BYNAME)
#define LLM_SCOPE_BYNAME(tag_name) Engine::LLMScope _llm_scope(tag_name)
