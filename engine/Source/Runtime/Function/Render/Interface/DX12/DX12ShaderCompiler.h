#pragma once

#include "Runtime/Function/Render/RenderType.h"

#include <d3d12.h>
#include <dxcapi.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Shader compilation result
struct DX12ShaderCompileResult
{
    bool success = false;
    std::vector<uint8_t> dxil_code;  // Compiled DXIL bytecode
    std::string error_message;
};

// Custom include handler for DXC
class DX12ShaderIncluder : public IDxcIncludeHandler
{
public:
    DX12ShaderIncluder(ComPtr<IDxcLibrary> library, const std::vector<std::string>& include_paths);
    ~DX12ShaderIncluder() = default;

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

private:
    ComPtr<IDxcLibrary> m_Library;
    std::vector<std::string> m_IncludePaths;
    ULONG m_RefCount = 1;
};

// Runtime shader compiler using DXC (DirectX Shader Compiler)
class DX12ShaderCompiler
{
public:
    DX12ShaderCompiler();
    ~DX12ShaderCompiler();

    // Compile HLSL shader from file path.
    //
    // `target_profile` overrides the stage-derived default profile
    // (`vs_6_0`, `ps_6_0`, ...). Empty string preserves legacy SM 6.0
    // behaviour. Pass e.g. `"ps_6_6"` for bindless-aware shaders that
    // need SM 6.6 `ResourceDescriptorHeap[]`.
    //
    // `hlsl_version` selects the `-HV <year>` flag fed to DXC. Empty
    // string omits the flag (DXC default = HV 2018). Pass `"2021"`
    // alongside SM 6.6 for HLSL 2021 features.
    //
    // Both new parameters default to "" so every existing call site
    // (preview, inspector, RHI shader-module loader, ...) continues
    // to compile against SM 6.0 / HV 2018 unchanged.
    DX12ShaderCompileResult CompileFromFile(const std::string& file_path,
                                            ShaderStage shader_stage,
                                            const std::vector<std::string>& include_paths = {},
                                            const std::map<std::string, std::string>& macros = {},
                                            const std::string& entry_point = "main",
                                            const std::string& target_profile = "",
                                            const std::string& hlsl_version = "");

    // Compile HLSL shader from source code string. See compileFromFile
    // for the `target_profile` / `hlsl_version` semantics.
    DX12ShaderCompileResult CompileFromSource(const std::string& hlsl_source,
                                              ShaderStage shader_stage,
                                              const std::string& shader_name = "",
                                              const std::vector<std::string>& include_paths = {},
                                              const std::map<std::string, std::string>& macros = {},
                                              const std::string& entry_point = "main",
                                              const std::string& target_profile = "",
                                              const std::string& hlsl_version = "");

    // Set shader include directory (for #include directives)
    void SetIncludeDirectory(const std::string& include_dir);

    // -------------------------------------------------------------------
    // On-disk DXIL cache (mtime-keyed).
    //
    // The cache is opt-in and is keyed off (file_path, entry_point,
    // shader_stage, defines-hash). It is ONLY consulted when a real source
    // file is provided (i.e. compileFromFile, or compileFromSource called
    // with `shader_name` set to an existing-on-disk file path); pure
    // in-memory sources from compileFromSource skip the disk cache.
    //
    // Invalidation: if the cache entry's mtime is older than the source
    // file's mtime, the entry is considered stale and a fresh compile is
    // performed. Note: included headers are not currently tracked, so
    // touching only an .hlsli will not invalidate the cache. Touch the
    // top-level .hlsl/.shader to force a recompile, or wipe the cache
    // directory.
    //
    // Mirror of `ShaderLab::ShaderLabCompiler::SetCacheDirectory` for the
    // DX12 backend; intended to be pointed at
    // `ProjectInfo::GetIntermediateShadersRoot()` from `DX12RHI::Initialize`.
    // -------------------------------------------------------------------

    // Per-instance cache directory override. Empty disables caching for this
    // instance (overrides the default). Directory will be created if missing.
    void SetCacheDirectory(const std::filesystem::path& dir);
    const std::filesystem::path& getCacheDirectory() const { return m_CacheDirectory; }

    // Default cache directory for ALL future-default-constructed
    // DX12ShaderCompiler instances. Existing instances are unaffected unless
    // they call setCacheDirectory explicitly. Empty disables caching.
    //
    // Typical caller: DX12RHI::Initialize -> SetDefaultCacheDirectory(
    //     GET_SYSTEM(ProjectInfo)->GetIntermediateShadersRoot()).
    static void SetDefaultCacheDirectory(const std::filesystem::path& dir);
    static const std::filesystem::path& GetDefaultCacheDirectory();

    // PR-AI2: drop every cached DXIL blob whose filename starts with the
    // src-path-hash for `source_file`. The hash matches `buildCacheFilePath`
    // exactly (FNV-1a 64 over `toLower(absolute(source_file).generic_string())`),
    // so this nukes ALL stage / entry / variant slots derived from that
    // source in one pass. Operates on the **default cache directory**
    // because that's where the engine-wide `DX12ShaderCompiler` (lazy
    // member of `DX12RHI`) and every default-constructed instance stash
    // their blobs; per-instance overrides via `SetCacheDirectory()` are
    // out of scope here -- they're only used by isolated unit tests.
    //
    // Returns the number of cache files actually deleted (0 == miss /
    // disabled / source never compiled). Errors during deletion are
    // logged once per file and counted as not-deleted.
    //
    // Why a static method on the compiler instead of a free function in
    // EditorAssetManager: the FNV-1a / toLower / toHex16 helpers are in
    // an anonymous namespace inside dx12_shader_compiler.cpp on purpose
    // (no header pollution); duplicating them at the call site would
    // silently drift if the cache key ever changes. Centralising the
    // invalidation here keeps cache-key + cache-invalidate in lock-step
    // by construction.
    static int InvalidateCacheForSource(const std::filesystem::path& source_file);

private:
    // Get DXC shader profile (e.g., "vs_6_0", "ps_6_0", "cs_6_0")
    std::string GetShaderProfile(ShaderStage shader_stage) const;

    // Build preprocessor defines string from macros
    std::string BuildDefinesString(const std::map<std::string, std::string>& macros) const;

    // Internal helper to compile HLSL to DXIL. `target_profile` and
    // `hlsl_version` may be empty -- empty preserves the legacy stage-
    // derived profile / HV-2018-default behaviour.
    DX12ShaderCompileResult CompileInternal(const std::string& hlsl_source,
                                            ShaderStage shader_stage,
                                            const std::string& shader_name,
                                            const std::vector<std::string>& include_paths,
                                            const std::map<std::string, std::string>& macros,
                                            const std::string& entry_point,
                                            const std::string& target_profile,
                                            const std::string& hlsl_version);

    // Build an absolute on-disk cache filename for (source-file, stage,
    // entry, defines, target_profile, hlsl_version). Returns an empty
    // path when caching is disabled or when `source_file` is not a real
    // on-disk file. `target_profile` / `hlsl_version` participate in
    // the variant key so SM 6.0 vs SM 6.6 / HV 2018 vs HV 2021 cache
    // entries don't collide on the same source file.
    std::filesystem::path BuildCacheFilePath(const std::string& source_file,
                                             ShaderStage shader_stage,
                                             const std::string& entry_point,
                                             const std::map<std::string, std::string>& macros,
                                             const std::string& target_profile,
                                             const std::string& hlsl_version) const;

    // Try to load a cached DXIL blob into out_dxil. Returns true iff the
    // cache file exists, is non-empty, and is newer than `source_file`.
    bool TryLoadCachedDxil(const std::filesystem::path& cache_file,
                           const std::string& source_file,
                           std::vector<uint8_t>& out_dxil) const;

    // Best-effort write of `dxil` to `cache_file`. Failure (e.g. read-only
    // filesystem, race) is logged once and otherwise ignored -- a missed
    // write just means the next run pays the recompile cost.
    bool WriteCachedDxil(const std::filesystem::path& cache_file,
                         const std::vector<uint8_t>& dxil) const;

    // DXC components
    ComPtr<IDxcLibrary> m_Library;
    ComPtr<IDxcCompiler> m_Compiler;
    ComPtr<IDxcUtils> m_Utils;

    std::string m_DefaultIncludeDir;

    // On-disk DXIL cache root; empty disables caching for this instance.
    std::filesystem::path m_CacheDirectory;
};