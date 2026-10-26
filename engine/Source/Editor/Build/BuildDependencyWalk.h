#pragma once

#include "Editor/Build/SceneCook.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class AssetRegistry;

// =====================================================================================
// BuildDependencyWalk -- which assets a packaged root pulls in.
// -------------------------------------------------------------------------------------
// Split out of AssetBundleBuilder so the walk can be tested without a project, a
// texture cook or a bundle writer.
//
// The edges are the ones the PLAYER follows. For most assets that is the registry's
// index of the source file. A YAML scene is different: the player gets its cooked copy
// (SceneCook strips editor-only objects such as PrefabInstance), so its edges are read
// from that copy -- otherwise a .prefab referenced only by a PrefabInstance (the
// bookkeeping, not the GameObjects) would still be packed.
// =====================================================================================
namespace BuildDependencyWalk
{
    /// Registry key spelling: forward slashes.
    std::string NormaliseKey(const std::string& key);

    using DependencyFn = std::function<std::vector<std::string>(const std::string& key)>;

    /// BFS from `root` over `dependencies`. The root is part of its own result (a level
    /// is an asset that has to be packed too). Order: discovery order, root first.
    std::vector<std::string> WalkFrom(const std::string& root, const DependencyFn& dependencies);

    /// Cooks `source` into `cooked` when the cooked copy is missing or older. Returns the
    /// cook result, or {ok = true, stripped = false} with nothing done when fresh.
    SceneCookResult CookSceneIfStale(const std::filesystem::path& source, const std::filesystem::path& cooked,
                                     bool* cooked_now = nullptr);

    /// The edges of one asset as the player sees them (see the header comment). A
    /// `.scene` is cooked (if stale) into `cooked_platform_root / key` first; when that
    /// root is empty or the cook failed, the source scene's edges are used.
    std::vector<std::string> PlayerDependencies(const AssetRegistry& registry, const std::string& key,
                                                const std::filesystem::path& content_root,
                                                const std::filesystem::path& cooked_platform_root);
}  // namespace BuildDependencyWalk
