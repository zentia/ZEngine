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

    /// Locate the packaged content of a finished build under `build_root`
    /// (`<build_root>/<Platform>/`), i.e. the `Bundles/` directory and the
    /// `*.manifest.zasset` that sits next to it.
    ///
    /// The platform sub-directory is named after the editor's BuildTarget
    /// display tag, which a player cannot compute (that enum is editor-only),
    /// so the content is FOUND rather than constructed: any directory holding
    /// both a `Bundles/` and a `.manifest.zasset` is a packaged build. Returns
    /// false when `build_root` holds no packaged content, which is the normal
    /// case for a launch out of the source tree.
    bool FindPackagedBuild(const std::filesystem::path& build_root,
                           std::filesystem::path& out_bundles_dir,
                           std::filesystem::path& out_manifest_path) const;

    /// Mount everything a packaged build shipped: `FindPackagedBuild()` +
    /// `MountDirectory()`. Returns the number of bundles mounted, and 0 when
    /// there is no packaged content -- that is a source-tree launch, not an
    /// error, so the caller must not treat 0 as failure.
    int MountBuild(const std::filesystem::path& build_root);

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

    /// One compiled script module, by MODULE ID ("foo/Bar", no extension).
    ///
    /// `EnsureBundleMountedForAsset` + `TryReadAssetBytes` behind the entry-key
    /// convention, so the loader does not have to know the prefix. False means
    /// "no mounted bundle carries it", which is the normal case in the editor
    /// (bundles are never mounted there) and the caller then falls back to the
    /// filesystem -- it is NOT an error.
    bool TryReadScriptModule(const std::string& module_id, std::string& out_source);

    /// GUID -> registry-relative path, built from the entries of every mounted
    /// bundle. This is the runtime stand-in for AssetRegistry's guid->path
    /// index: a player build ships no registry, and without this every external
    /// PPtr whose file slot carries a GUID instead of a path resolves to null.
    bool TryResolveGuid(const std::string& guid, std::string& out_path) const;

    /// Mount the bundle that owns `asset_path`, when the loaded manifest says
    /// which one that is and it is not mounted yet. True when the asset is
    /// reachable afterwards -- already mounted counts as success, and so does
    /// "no manifest / no mounted bundle to derive a directory from" being
    /// answered by a plain containment check.
    ///
    /// This is the FileID -> bundle mapping the build manifest makes possible:
    /// `FileIdentifier` carries a GUID and a path but never a bundle name, so
    /// without this a player has to mount everything up front or lose every
    /// reference that crosses a bundle boundary.
    bool EnsureBundleMountedForAsset(const std::string& asset_path);

    const AssetBundleManifest* GetManifest() const { return m_Manifest; }

private:
    bool MountBundleInternal(const std::filesystem::path& bundle_path, std::vector<std::string>& in_out_visited);

    /// Caller holds m_Mutex.
    void IndexBundleGuidsLocked(const AssetBundle& bundle);
    void RebuildGuidIndexLocked();

    mutable std::mutex m_Mutex;
    /// bundle name -> bundle. Stable heap addresses (never erased except by
    /// Unmount*/Shutdown) so callers may hold the raw pointer.
    std::unordered_map<std::string, AssetBundle*> m_Bundles;
    /// Owned by ObjectManager (loaded through AssetManager); not freed here.
    AssetBundleManifest* m_Manifest {nullptr};
    /// guid -> registry-relative path. Rebuilt on unmount (an erase cannot be
    /// attributed back to a single guid->path pair without a second index).
    std::unordered_map<std::string, std::string> m_GuidToPath;
    /// Directory of the first bundle mounted. Bundles named in the manifest but
    /// not yet mounted are looked up here; it is the only place we can learn
    /// where sibling bundles live.
    std::filesystem::path m_BundleDirectory;
};
