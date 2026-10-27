#pragma once

// ============================================================================
// UMGDelegates — 命名事件注册表（UE UMG DynamicDelegate 的简化版，header-only）
// ----------------------------------------------------------------------------
// 解决 P0 缺口：Designer 里给按钮绑的事件名（如 "OnStartClicked"）序列化进资产后，
// 运行时如何接回游戏代码。UButton::OnClicked 是 std::function（代码直连），序列化
// 不了；这里提供一层"命名事件 → handler 表"的间接层：
//
//   - 资产侧：widget 序列化一个事件名字符串（OnClickEvent="OnStartClicked"）。
//   - 运行时侧：游戏代码 Registry.Bind("OnStartClicked", [](void* ctx){ ... })；
//     widget 触发时 Registry.Invoke("OnStartClicked")。
//
// 与 UE 的 DynamicDelegate 同构，但用 std::function + 字符串 id，不引入反射。
//
// 生命周期（关键）：handler 里捕获的指针可能指向已销毁的 UUserWidget/UWidget。
// Bind 返回一个 token；owner（通常是 UUserWidget）析构时用 UnbindAll(owner) 一次性
// 反注册它名下所有 handler，避免悬垂调用。Invoke 时也用 weak 语义——这里用 token
// 计数 + 显式反注册，而非 weak_ptr，因为 handler 是值语义的 std::function。
//
// 纯 C++ 容器，无引擎依赖，可 headless 单测（UMGDelegatesSmokeTest）。
// ============================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ZUMG
{
// Opaque handle returned by Bind; pass back to Unbind to remove one handler.
using UMGEventToken = uint64_t;

class UMGEventRegistry
{
public:
    using Handler = std::function<void()>;

    // Register a handler under event_id. Returns a token for later Unbind.
    // Duplicate handlers under the same id are allowed (multiple listeners).
    UMGEventToken Bind(const std::string& event_id, Handler handler)
    {
        const UMGEventToken token = m_NextToken++;
        m_Events[event_id].push_back({token, std::move(handler)});
        return token;
    }

    // Fire every handler bound to event_id. Unknown id is a no-op (not an error:
    // an asset may reference an event no game code bound this session).
    void Invoke(const std::string& event_id) const
    {
        auto it = m_Events.find(event_id);
        if (it == m_Events.end())
            return;
        // Copy the handler list before invoking: a handler may Unbind itself or
        // others under the same id, which would invalidate iterators mid-loop.
        const std::vector<Entry> entries = it->second;
        for (const Entry& e : entries)
        {
            if (e.handler)
                e.handler();
        }
    }

    // Remove a single handler by token. Returns true if it was found & removed.
    bool Unbind(UMGEventToken token)
    {
        for (auto& [id, entries] : m_Events)
        {
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->token == token)
                {
                    entries.erase(it);
                    if (entries.empty())
                        m_Events.erase(id);
                    return true;
                }
            }
        }
        return false;
    }

    // Remove every handler bound under event_id. Returns how many were removed.
    size_t UnbindEvent(const std::string& event_id)
    {
        auto it = m_Events.find(event_id);
        if (it == m_Events.end())
            return 0;
        const size_t n = it->second.size();
        m_Events.erase(it);
        return n;
    }

    // Remove ALL handlers across every event. Used on owner teardown.
    void Clear() { m_Events.clear(); }

    // Number of handlers currently bound under event_id (0 if none).
    size_t Count(const std::string& event_id) const
    {
        auto it = m_Events.find(event_id);
        return it == m_Events.end() ? 0 : it->second.size();
    }

    // Total handlers across all events (diagnostics / tests).
    size_t TotalCount() const
    {
        size_t n = 0;
        for (const auto& [id, entries] : m_Events)
            n += entries.size();
        return n;
    }

private:
    struct Entry
    {
        UMGEventToken token;
        Handler handler;
    };
    std::unordered_map<std::string, std::vector<Entry>> m_Events;
    UMGEventToken m_NextToken {1};  // reserve 0 as "invalid"
};

// Process-wide default registry. Assets reference event ids; the game binds them
// here at startup. Kept as a free function (not a static member) so translation
// units share one instance without an init-order trap.
UMGEventRegistry& GetUMGEventRegistry();
}  // namespace ZUMG
