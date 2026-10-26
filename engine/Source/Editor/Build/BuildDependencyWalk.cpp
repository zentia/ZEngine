#include "Editor/Build/BuildDependencyWalk.h"

#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Runtime/Core/Base/Macro.h"

#include <deque>
#include <system_error>
#include <unordered_set>

namespace BuildDependencyWalk
{
    std::string NormaliseKey(const std::string& key)
    {
        std::string out;
        out.reserve(key.size());
        for (const char c : key)
        {
            out.push_back(c == '\\' ? '/' : c);
        }
        return out;
    }

    std::vector<std::string> WalkFrom(const std::string& root, const DependencyFn& dependencies)
    {
        std::vector<std::string> reachable;
        std::unordered_set<std::string> visited;
        std::deque<std::string> pending;

        visited.insert(root);
        pending.push_back(root);

        while (!pending.empty())
        {
            const std::string current = pending.front();
            pending.pop_front();
            reachable.push_back(current);

            if (!dependencies)
            {
                continue;
            }
            for (const std::string& raw_dependency : dependencies(current))
            {
                const std::string dependency = NormaliseKey(raw_dependency);
                if (dependency.empty() || visited.count(dependency) != 0)
                {
                    continue;
                }
                visited.insert(dependency);
                pending.push_back(dependency);
            }
        }

        return reachable;
    }

    SceneCookResult CookSceneIfStale(const std::filesystem::path& source, const std::filesystem::path& cooked,
                                     bool* cooked_now)
    {
        if (cooked_now != nullptr)
        {
            *cooked_now = false;
        }

        std::error_code ec;
        bool stale = !std::filesystem::exists(cooked, ec);
        if (!stale)
        {
            std::error_code source_ec;
            std::error_code cooked_ec;
            const auto source_time = std::filesystem::last_write_time(source, source_ec);
            const auto cooked_time = std::filesystem::last_write_time(cooked, cooked_ec);
            stale = source_ec || cooked_ec || cooked_time < source_time;
        }
        if (!stale)
        {
            SceneCookResult fresh;
            fresh.ok = true;
            return fresh;
        }

        const SceneCookResult cook = SceneCook::CookSceneFile(source, cooked);
        if (cooked_now != nullptr)
        {
            *cooked_now = cook.ok;
        }
        if (!cook.ok)
        {
            LOG_WARNING(ZPak, "BuildDependencyWalk: scene cook failed ({}); the editor scene is used", cook.error);
        }
        else if (cook.removed_objects > 0)
        {
            LOG_INFO(ZPak, "BuildDependencyWalk: {}: cooked out {} editor-only object(s), {} external(s)",
                     source.generic_string(), cook.removed_objects, cook.removed_externals);
        }
        return cook;
    }

    std::vector<std::string> PlayerDependencies(const AssetRegistry& registry, const std::string& key,
                                                const std::filesystem::path& content_root,
                                                const std::filesystem::path& cooked_platform_root)
    {
        const std::filesystem::path relative(key);
        if (relative.extension() == ".scene" && !cooked_platform_root.empty() && !content_root.empty())
        {
            const std::filesystem::path cooked = cooked_platform_root / relative;
            const SceneCookResult cook = CookSceneIfStale(content_root / relative, cooked);
            std::error_code ec;
            if (cook.ok && std::filesystem::exists(cooked, ec))
            {
                return registry.GetDependenciesOfFile(cooked, key);
            }
        }
        return registry.GetDependencies(key);
    }
}  // namespace BuildDependencyWalk
