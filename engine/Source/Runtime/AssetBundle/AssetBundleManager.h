#pragma once

// =============================================================================
// AssetBundleManager -- mounted-bundle registry (PR-PAK1).
//
// The single place that answers "does any mounted bundle have this asset?",
// which is what lets a player build run without an `Assets/` tree:
// `SerializedFile::InitializeRead()` asks here first and, on a hit, reads the
// asset out of the bundle instead of the filesystem. Everything above
// (AssetManager::loadAsset, PPtr resolution, Level::load) keeps working
// unchanged because it all funnels through that one call.
//
// Deliberately NOT done:
//   * reference counting per asset (Unity's `AssetBundle.Unload(bool)`).
//     Unloading a bundle drops every blob it cached; callers must drop the
//     assets first. See AssetBundle::Unload().
//   * async / streaming loads. Blobs are read synchronously on first use.
// =============================================================================

#include "Runtime/AssetBundle/AssetBundle.h"
#include "Runtime/AssetBundle/AssetBundleManifest.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Base/Macro.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class AssetBundleManager : public IEngineSystem
{
public:
    std::string GetName() const override { return GET_CLASS_NAME(AssetBundleManager); }
    bool Initialize() override { return true; }
    void Shutdown() override { UnmountAll(); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Resource; }

    /// Parses `<path>.zbundle` and, when a manifest is loaded, mounts its
    /// dependency bundles first (recursively, cycle-guarded). Returns false if
    /// the file cannot be parsed or a dependency is missing.
    bool MountBundle(const std::filesystem::path& bundle_path);

    /// Loads the build manifest (a normal `.zasset`). Bundles mounted after
    /// this get their dependency list from it, and `MountDirectory` can order
    /// mounts correctly.
    bool LoadManifest(const std::filesystem::path& manifest_path);

    /// Mounts every `*.zbundle` in `dir`. When `manifest_path` is non-empty it
    /// is loaded first so dependencies mount before dependents.
    int MountDirectory(const std::filesystem::path& dir, const std::filesystem::path& manifest_path = {});

    bool Unmount(const std::string& bundle_name);
    void UnmountAll();

    bool IsMounted(const std::string& bundle_name) const;
    AssetBundle* FindBundle(const std::string& bundle_name) const;
    std::vector<std::string> GetMountedBundleNames() const;

    /// True when ANY mounted bundle contains `asset_path`.
    bool ContainsAsset(const std::string& asset_path) const;

    /// Copies the asset's bytes out of the first mounted bundle that has it.
    /// `out_bytes` is only written on success.
    bool TryReadAssetBytes(const std::filesystem::path& asset_path, std::vector<uint8_t>& out_bytes) const;

    const AssetBundleManifest* GetManifest() const { return m_Manifest; }

private:
    bool MountBundleInternal(const std::filesystem::path& bundle_path, std::vector<std::string>& in_out_visited);

    mutable std::mutex m_Mutex;
    /// bundle name -> bundle. Stable heap addresses (never erased except by
    /// Unmount*/Shutdown) so callers may hold the raw pointer.
    std::unordered_map<std::string, AssetBundle*> m_Bundles;
    /// Owned by ObjectManager (loaded through AssetManager); not freed here.
    AssetBundleManifest* m_Manifest {nullptr};
};
