#pragma once

// =============================================================================
// AssetBundleBuilder -- the packaging pass (PR-PAK1).
//
// This is the missing half of the pipeline. `Runtime/AssetBundle/` can already
// READ a `.zbundle` (25 smoke-test checks cover it, including "the asset still
// loads after the Assets/ tree is deleted"), and `AssetRegistry` already knows
// the dependency graph -- but nothing ever produced a bundle. Every packaging
// entry point (the Build menu, `bundle.build`) therefore referenced a class
// that had never existed, which is also why ZEditor stopped compiling.
//
// What one Build() does:
//
//   1. Pick the roots. By default every LevelRes / WorldRes / PrefabAsset the
//      registry indexes -- the "entry points" a player can be asked to open.
//      Explicit `roots` override that.
//   2. BFS each root over AssetRegistry's dependency graph.
//   3. Partition: an asset reached by >= 2 roots goes into a single `shared`
//      bundle (otherwise it would be duplicated once per root, and the copy
//      the runtime happened to mount would decide which one wins); everything
//      else goes into its root's bundle.
//   4. Cook first (textures only today), then pack COOKED bytes when the
//      platform has them and source bytes otherwise.
//   5. Pack the compiled script modules from <project>/Intermediate/Scripts/
//      into their own `scripts` bundle. They are not part of the dependency
//      walk (a ScriptAsset names a module id, and the .js lives outside the
//      content directory the registry indexes), so they are collected
//      separately and every other bundle declares `scripts` as a dependency.
//   6. Write `<output_dir>/<name>.zbundle` per bundle plus
//      `<output_dir>/AssetBundleManifest.zasset`.
//
// The manifest is what makes a bundle mountable in isolation: it records each
// bundle's dependencies so AssetBundleManager can mount them first. Without it
// a player would have to mount everything or fail at the first cross-asset
// reference.
// =============================================================================

#include "Editor/AssetPipeline/TextureImporter/TextureImporterSettings.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

/// One packaging pass.
struct AssetBundleBuildOptions
{
    /// Platform to cook for. Decides both the cooked-bytes directory that is
    /// consulted and the `<project>/Build/<Platform>/` default output dir.
    TextureImporterSettings::BuildTarget target {TextureImporterSettings::BuildTarget::Standalone};

    /// Explicit roots, registry-relative with forward slashes
    /// ("level/1-1.scene"). Empty means "discover them" -- every LevelRes /
    /// WorldRes / PrefabAsset the registry indexes.
    std::vector<std::string> roots;

    /// Empty means `<project>/Build/<Platform>/`.
    std::filesystem::path output_dir;

    /// Run the texture cook before packing. Without it the bundles carry the
    /// editor-platform texture bytes, which is fine on desktop and wrong on
    /// mobile (BC where ASTC is expected).
    bool cook_first {true};
};

struct AssetBundleBuildResult
{
    bool succeeded {false};
    /// Empty on success. Human-readable, goes straight into a log line.
    std::string error;
    std::size_t bundle_count {0};
    std::size_t asset_count {0};
    /// Compiled script modules packed into the `scripts` bundle. Counted
    /// separately from `asset_count` because a module is neither a registry
    /// asset nor an Object -- `asset_count` is what a "did the dependency walk
    /// find everything" check should look at.
    std::size_t script_count {0};
    /// Sum of the written .zbundle file sizes (the manifest is not counted --
    /// it is metadata about the bundles, not one of them).
    std::size_t total_bytes {0};
    std::filesystem::path output_dir;
};

class AssetBundleBuilder
{
public:
    /// Never throws and never asserts: every failure is reported through
    /// `result.error`, because this runs on the editor's UI thread from a menu
    /// item that has no way to catch anything.
    static AssetBundleBuildResult Build(const AssetBundleBuildOptions& options);

    /// `<project>/Build/<Platform>/`. Empty when no project is open. Exposed so
    /// a caller can tell the user where the output landed without duplicating
    /// the path arithmetic.
    static std::filesystem::path DefaultOutputDir(TextureImporterSettings::BuildTarget target);
};
