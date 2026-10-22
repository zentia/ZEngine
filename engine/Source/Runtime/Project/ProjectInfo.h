#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Command/CommandSystem.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include "ProjectInfo.generated.h"

/**
 * @brief Project information structure
 * Similar to Unreal Engine's .uproject file
 */
ZCLASS()
class ProjectInfo : public Object, public IEngineSystem
{
    ZGENERATED_BODY(ProjectInfo)

public:
    std::string GetName() const override { return GET_CLASS_NAME(ProjectInfo); }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PreInit; }
    std::vector<std::type_index> GetDependencies() const override { return {GET_SYSTEM_TYPE(CommandSystem)}; }
    bool Initialize() override;
    void Shutdown() override {}
    // Project metadata
    ZPROPERTY()
    eastl::string name;                  // Project name
    std::string description;             // Project description
    std::string version;                 // Project version (e.g., "1.0.0")
    std::filesystem::path project_path;  // Full path to project directory
    std::filesystem::path project_file;  // Full path to .zproject file

    // Engine version
    std::string engine_version;  // Engine version used by this project

    // Project settings
    std::vector<std::string> tags;  // Project tags

    // Timestamps
    time_t created_time;
    time_t last_opened_time;

    // Project directories (relative to project root)
    //
    // 这四个字段**必须**带 ZPROPERTY + eastl::string：.zproject 里写的是
    // `content_dir: asset`，而引擎的 Transfer 只对 eastl::string 有
    // SerializeTraits 特化（见 SerializeTraits.h）。以前它们是裸 std::string 且
    // 没有宏 ⇒ 生成代码里根本没有它们 ⇒ .zproject 里的值被静默忽略，
    // GetProjectContent() 永远返回 <project>/Assets。
    //
    // 症状链（macOS 上文件名区分大小写，所以暴露得最彻底）：
    //   content_dir 恒为 "Assets" → 扫描/导入/cook 全走空目录
    //   → 9 个 .tga 一个都没被导入成 Texture2D .zasset
    //   → Material 的 legacy 贴图路径解析不到 → 采样占位白图。
    // 而且没有任何一条日志说"我找错目录了"。
    ZPROPERTY()
    eastl::string content_dir {"Assets"};  // asset directory (Unity-style default: "Assets")
    ZPROPERTY()
    eastl::string config_dir {"config"};   // config directory (default: "config")
    ZPROPERTY()
    eastl::string saved_dir {"saved"};     // saved directory (default: "saved")
    ZPROPERTY()
    eastl::string source_dir {"source"};   // source directory (default: "source")

    // Scripting directories:
    //   <project>/<scripts_dir>/**.ts            -> user-authored TypeScript sources
    //   <project>/<intermediate_dir>/Scripts/*.js -> tsc compile output (gitignored)
    //
    // We mirror Unreal's `<Project>/Source/` placement (peer of the asset folder)
    // rather than Unity's `<Project>/Assets/Scripts/`. Rationale:
    //   1. Existing ZEngine demo projects already have <project>/Scripts/ at the
    //      top level. Users expect that layout.
    //   2. Keeping .ts out of <project>/Assets/ means the Project window's
    //      asset-tree code (which assumes everything under Assets/ is a .zasset)
    //      doesn't have to special-case .ts/.tsx; Phase 2's ScriptAssetImporter
    //      will register Scripts/ as an additional scan root.
    //
    // Typed as eastl::string (not std::string) because these fields participate
    // in Transfer-based serialisation, and SerializeTraits is specialised for
    // eastl::string only - see Runtime/Core/Serialize/SerializeTraits.h.
    // New scripting fields. Persisted so users can rename Scripts/ → Code/ etc.
    // and have it survive a reload. Defaulted in the header, so existing
    // .zproject files without these keys still load correctly.
    ZPROPERTY()
    eastl::string scripts_dir {"Scripts"};            // relative to project root
    ZPROPERTY()
    eastl::string intermediate_dir {"Intermediate"};  // relative to project root (gitignored)

    // Shader directory (peer of Assets/ and Scripts/). Mirrors the Scripts
    // layout 1:1 - see AGENTS.md 2.2. The compiled SPIR-V/DXIL artifacts go
    // to <project>/<intermediate_dir>/Shaders/ (gitignored). Typed as
    // eastl::string for the same reason scripts_dir is (Transfer-based
    // serialisation only supports eastl::string).
    ZPROPERTY()
    eastl::string shaders_dir {"Shaders"};  // relative to project root

    // Data directory (peer of Assets/, Scripts/, Shaders/). Holds user-authored
    // CSV/XLSX source tables that are compiled at editor startup (and on file
    // change) into binary .zasset DataTable instances. Source files (.csv) are
    // checked into VCS; the compiled artifacts go to
    // <project>/<content_dir>/_Generated/Data/  -- placed UNDER Assets/ on
    // purpose so the existing AssetRegistry scan picks them up with zero
    // additional code (the _Generated subtree itself is gitignored). See
    // AGENTS.md "Data pipeline" section.
    ZPROPERTY()
    eastl::string data_dir {"Data"};  // relative to project root

    // Texture source directory (peer of Assets/, Scripts/, Shaders/, Data/).
    // Holds user-authored image sources imported into Texture2D .zasset files
    // under Assets/. **Checked into VCS.** See TEXTURE_COOK_PIPELINE.md 1.1.
    ZPROPERTY()
    eastl::string textures_dir {"Textures"};

    // Model source directory (peer of the other source roots). Holds .fbx/.obj/
    // /.gltf/.glb sources imported into mesh .zasset files under Assets/.
    // **Checked into VCS.**
    ZPROPERTY()
    eastl::string models_dir {"Models"};

    // Declared after the source-root dirs: serialized order is name, the
    // *_dir fields, then startup_level.
    // Startup level (UE's GameDefaultMap). Relative to the content dir, e.g.
    // "asset/level/1-1.scene". Empty means "no level is loaded on launch".
    // Typed eastl::string because Transfer()-based serialisation is only
    // specialised for eastl::string (see SerializeTraits.h); std::string would
    // silently fail to compile into the generated Transfer.
    ZPROPERTY()
    eastl::string startup_level;  // Default level loaded by ZGame on launch

    // Other
    std::filesystem::path m_WorkingDir;
    /**
     * @brief Load project info from .zproject file
     */
    bool LoadFromFile(const std::filesystem::path& project_file_path);

    /**
     * @brief Save project info to .zproject file
     */
    bool SaveToFile() const;

    /**
     * @brief Check if project is valid
     */
    bool IsValid() const;

    /**
     * @brief Get project root directory
     */
    std::filesystem::path GetProjectRoot() const;

    /**
     * @brief Get project content directory
     */
    std::filesystem::path GetProjectContent() const;

    // -------------------------------------------------------------------------
    // Scripting paths (TypeScript). All return absolute paths.
    // Empty `project_path` -> empty result (caller must check, mirrors content APIs).
    // -------------------------------------------------------------------------

    /// User-authored TypeScript source root: <project>/<content>/<scripts>.
    /// Equivalent to Unity's `Assets/Scripts/`.
    std::filesystem::path GetScriptsRoot() const;

    /// Compiled JavaScript output root: <project>/<intermediate>/Scripts/.
    /// Both Editor and Runtime load .js modules from here at startup.
    /// The directory is excluded from version control.
    std::filesystem::path GetIntermediateScriptsRoot() const;

    /// `<project>/AssetRegistry/` - **checked into VCS** (NOT under
    /// Intermediate/). Holds GUID-bearing registry JSONs that are the source
    /// of truth for text-source assets (no .meta sidecars; see AGENTS.md
    /// 2.1). Currently only `script_registry.json` lives here; future text
    /// asset kinds (.glsl, .ts.d.ts) will share this directory.
    std::filesystem::path GetAssetRegistryRoot() const;

    /// `<project>/AssetRegistry/script_registry.json` - the path<->GUID map
    /// for all .ts/.js files under GetScriptsRoot(). See ScriptRegistry.
    std::filesystem::path GetScriptRegistryPath() const;

    /// `<project>/AssetRegistry/shader_registry.json` - path<->GUID map for
    /// `.shader` sources under GetShadersRoot(). See ShaderRegistry.
    std::filesystem::path GetShaderRegistryPath() const;

    // -------------------------------------------------------------------------
    // Shader paths. All return absolute paths.
    // Empty `project_path` -> empty result (caller must check).
    // -------------------------------------------------------------------------

    /// User-authored shader source root: <project>/<shaders_dir>/.
    /// Peer of Assets/ and Scripts/; .shader/.hlsl/.compute/.raytrace files
    /// live here. Equivalent to UE's per-Plugin `Shaders/` directory.
    std::filesystem::path GetShadersRoot() const;

    /// Compiled shader cache root: <project>/<intermediate_dir>/Shaders/.
    /// On-disk cache of SPIR-V / DXIL blobs keyed by source mtime + variant
    /// hash. Excluded from version control; safe to delete to force a full
    /// recompile on next launch. ShaderLabCompiler::SetCacheDirectory() is
    /// pointed here at boot.
    std::filesystem::path GetIntermediateShadersRoot() const;

    /// Derived Data Cache root: <project>/<intermediate_dir>/DDC/.
    /// LMDB-backed store of platform-specific cooked artifacts (compressed
    /// texture mip blobs, ...) keyed by (asset GUID, platform, settings hash,
    /// encoder version). Excluded from version control; safe to delete to
    /// force a recook on next launch. See DerivedDataCacheAccessor.
    std::filesystem::path GetIntermediateDDCRoot() const;

    /// Per-platform cooked output root: <project>/<intermediate_dir>/Cooked/.
    /// Cook writes <Cooked>/<Platform>/<rel>.zasset variants here, reusing the
    /// SOURCE asset GUID so references resolve in a player build. Gitignored.
    std::filesystem::path GetIntermediateCookedRoot() const;

    /// Packaged-build output root: <project>/Build/. AssetBundleBuilder writes
    /// one sub-directory per platform in here
    /// (`<Build>/<Platform>/Bundles/*.zbundle` + the manifest next to it).
    ///
    /// The sub-directory name is a *display* tag ("Standalone", "Android", ...)
    /// minted by the editor's BuildTarget enum, which lives in the editor and
    /// is therefore invisible to a player. A player locates its content by
    /// looking for the manifest inside Build/, not by naming the platform --
    /// see AssetBundleManager::MountBuild().
    std::filesystem::path GetBuildRoot() const;

    // -------------------------------------------------------------------------
    // Data paths. All return absolute paths.
    // Empty `project_path` -> empty result (caller must check).
    // -------------------------------------------------------------------------

    /// User-authored data-table source root: <project>/<data_dir>/.
    /// Peer of Assets/, Scripts/, Shaders/; .csv (V1) and .xlsx (V2) files
    /// live here. **Checked into VCS.** Equivalent to UE's
    /// Content/DataTables source step before the .uasset bake.
    std::filesystem::path GetDataRoot() const;

    // -------------------------------------------------------------------------
    // Texture / model source paths. All return absolute paths.
    // Empty `project_path` -> empty result (caller must check).
    // -------------------------------------------------------------------------

    /// User-authored texture source root: <project>/<textures_dir>/.
    /// Peer of Assets/, Scripts/, Shaders/, Data/; .png/.jpg/.tga/... live here.
    /// **Checked into VCS.** Imported Texture2D .zasset products land under
    /// Assets/ (editor-platform variant) and Intermediate/Cooked/ (player).
    std::filesystem::path GetTexturesRoot() const;

    /// User-authored mesh source root: <project>/<models_dir>/.
    /// Peer of the other source roots; .fbx/.obj/.gltf/.glb live here.
    /// **Checked into VCS.** Imported mesh .zasset products land under Assets/.
    std::filesystem::path GetModelsRoot() const;

    /// Generated-asset bucket: <project>/<content_dir>/_Generated/.
    /// Lives UNDER Assets/ on purpose -- the editor's AssetRegistry already
    /// scans the whole content directory, so any .zasset emitted here is
    /// picked up with zero extra wiring. The whole subtree is gitignored
    /// via the ZEngine scaffolding marker block in .gitignore (see
    /// ensureScriptsScaffold). DO NOT hand-edit anything in here -- it is
    /// rebuilt deterministically from sources under Data/, Shaders/, etc.
    std::filesystem::path GetGeneratedAssetsRoot() const;

    /// Compiled DataTable bucket: <project>/<content_dir>/_Generated/Data/.
    /// One .zasset per input .csv (path mirroring; only the extension
    /// changes). Gitignored. Recreated on every editor launch by
    /// DataTableImporter::CompileProject().
    std::filesystem::path GetGeneratedDataRoot() const;

    /// Generated ShaderRes bucket: <project>/<content_dir>/_Generated/Shaders/.
    /// Generated Texture2D bucket: <content>/_Generated/Textures/.
    /// Same pattern as _Generated/Data/ and _Generated/Shaders/: placed UNDER
    /// Assets/ so AssetRegistry picks the products up with zero extra wiring;
    /// the whole _Generated/ subtree is gitignored. Only the extension changes,
    /// and the source image's path under the content root is mirrored.
    std::filesystem::path GetGeneratedTexturesRoot() const;

    /// Where the editor-platform Texture2D .zasset for `source_file` lives.
    ///
    /// `<content>/_Generated/Textures/<source rel path>` with `.zasset` swapped
    /// in, e.g. `asset/objects/_textures/gold.tga` ->
    /// `asset/_Generated/Textures/objects/_textures/gold.zasset`.
    ///
    /// Import products are **derived data** and must not sit next to the
    /// VCS-tracked source image. Returns an empty path when no project is
    /// loaded, so callers can bail.
    std::filesystem::path GetGeneratedTexturePath(const std::filesystem::path& source_file) const;

    /// One .zasset per input .shader (same stem, new extension). Lives
    /// UNDER Assets/ so AssetRegistry picks it up with zero extra wiring;
    /// the whole _Generated/ subtree is gitignored. Recreated on every
    /// editor launch by ShaderImporter::ImportProjectShaders(). Mirrors
    /// GetGeneratedDataRoot() 1:1.
    std::filesystem::path GetGeneratedShadersRoot() const;

    /// `<project>/tsconfig.json` - the per-project TypeScript build config.
    std::filesystem::path GetTsConfigPath() const;

    /// `<project>/package.json` - devDependency manifest (typescript, @types).
    std::filesystem::path GetPackageJsonPath() const;

    /// `<project>/Packages/` - ZPM project manifest root (Unity Packages/ analogue).
    std::filesystem::path GetPackagesRoot() const;

    /// `<project>/Packages/manifest.json` - direct ZPM dependencies.
    std::filesystem::path GetProjectPackagesManifestPath() const;

    /**
     * @brief Idempotently create the project scaffolding on disk.
     *
     * Despite the historical name, this scaffolds **all** per-project source
     * roots that have to exist before the editor opens (Scripts, Shaders,
     * Data, Textures, Models) plus the always-present supporting directories.
     *
     * On first project open we create:
     *   - <content>/<scripts>/         (empty user-source folder)
     *   - <intermediate>/Scripts/      (empty compile-output folder)
     *   - <project>/<shaders_dir>/     (empty user-source folder)
     *   - <intermediate>/Shaders/      (empty compile-cache folder)
     *   - <project>/<data_dir>/        (empty user-source folder)
     *   - <project>/<textures_dir>/    (empty texture-source folder)
     *   - <project>/<models_dir>/      (empty model-source folder)
     *   - <content>/_Generated/Data/   (empty compile-output folder, under Assets/)
     *   - <project>/AssetRegistry/     (VCS-tracked, holds path<->GUID maps)
     *   - <project>/tsconfig.json      (only if missing)
     *   - <project>/package.json       (only if missing)
     *   - <project>/Packages/manifest.json (only if missing; ZPM)
     *   - <project>/.gitignore         (append /<intermediate>/ + /Assets/_Generated/ rules if missing)
     *
     * Safe to call repeatedly; will never overwrite an existing tsconfig/package.
     * Called automatically from Initialize() once the project root is known.
     */
    bool EnsureScriptsScaffold();
};