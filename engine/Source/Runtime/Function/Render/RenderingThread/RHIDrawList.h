#pragma once

#include <functional>
#include <string>
#include <vector>

// Deferred GPU work built on the render thread, executed on the RHI thread.
class RHIDrawList
{
public:
    using SubmitFn = std::function<void()>;

    void Clear();

    void Add(const char* debug_name, SubmitFn submit_fn);

    void ExecuteAll() const;

    bool IsEmpty() const { return m_Entries.empty(); }

private:
    struct Entry
    {
        std::string debug_name;
        SubmitFn submit_fn;
    };

    std::vector<Entry> m_Entries;
};
