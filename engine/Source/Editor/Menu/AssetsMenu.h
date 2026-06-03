#pragma once

#include <string>

class AssetsMenu
{
public:
    /// Convert (import) a source file -- e.g. .png / .fbx / .wav / .gltf --
    /// into a `.zasset` product. The product ALWAYS lands inside the
    /// project's content directory (`<Project>/Assets/...`) so the
    /// AssetRegistry can pick it up automatically; source files staying
    /// outside `Assets/` is the UE Content Browser model (source files are
    /// invisible to the asset registry by design).
    ///
    /// Output path resolution:
    ///   * If `target_dir` is non-empty AND lies inside `<Project>/Assets/`
    ///     (or equals it), the product goes to
    ///     `<target_dir>/<source-stem>.zasset` -- i.e. wherever the user
    ///     was browsing in the Project window when they triggered Import.
    ///   * Otherwise the product goes to `<Project>/Assets/<source-stem>.zasset`.
    ///
    /// `path` is the absolute on-disk path to the source file. `target_dir`
    /// is optional; pass empty to use the project-content fallback.
    static void ConvertAsset(eastl::string path, eastl::string target_dir = {});
};
