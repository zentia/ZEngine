#pragma once

#include "Runtime/Function/Render/RenderType.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// ShaderStage enum is defined in render_type.h (shared across all RHI backends).

// Shader macro definition for variants
// Example: {"ENABLE_SHADOWS", "1"} or {"MAX_LIGHTS", "4"}
using ShaderMacros = std::map<std::string, std::string>;

// Shader compilation result
struct ShaderCompileResult
{
    bool success = false;
    std::vector<unsigned char> spirv_code;  // Compiled SPIR-V bytecode
    std::string error_message;              // Error message if compilation failed
};

// Runtime shader compiler using glslang
class ShaderCompiler

{
public:
    ShaderCompiler();
    ~ShaderCompiler();

    // Compile GLSL shader from file path
    ShaderCompileResult CompileFromFile(const std::string& file_path,
                                        ShaderStage stage,
                                        const std::vector<std::string>& include_paths = {},
                                        const ShaderMacros& macros = {});

    // Compile GLSL shader from source code string
    ShaderCompileResult CompileFromSource(const std::string& source_code,
                                          ShaderStage stage,
                                          const std::string& shader_name = "",
                                          const std::vector<std::string>& include_paths = {},
                                          const ShaderMacros& macros = {});

    // Set shader include directory (for #include directives)
    void SetIncludeDirectory(const std::string& include_dir);

    // -------------------------------------------------------------------
    // On-disk SPIR-V cache (mtime-keyed). Mirror of
    // `DX12ShaderCompiler::setCacheDirectory` for the Vulkan/glslang
    // backend; intended to be pointed at
    // `ProjectInfo::GetIntermediateShadersRoot()` from
    // `VulkanRHI::Initialize`.
    //
    // The cache is opt-in and is keyed off (file_path, stage, defines).
    // It is ONLY consulted when a real source file is provided (i.e.
    // compileFromFile, or compileFromSource called with `shader_name`
    // pointing at an existing on-disk file); pure in-memory sources
    // skip the disk cache because there is no stable mtime ground truth.
    //
    // Invalidation: if the cache entry's mtime is older than the source
    // file's mtime, the entry is considered stale and a fresh compile
    // is performed. Note: included headers are not currently tracked,
    // so touching only an `.glsl` / `.h` include will not invalidate
    // the cache. Touch the top-level shader (or wipe the cache
    // directory) to force a recompile.
    // -------------------------------------------------------------------

    // Per-instance cache directory override. Empty disables caching for
    // this instance (overrides the default). Directory will be created
    // if missing.
    void SetCacheDirectory(const std::filesystem::path& dir);
    const std::filesystem::path& getCacheDirectory() const { return m_CacheDirectory; }

    // Default cache directory for ALL future-default-constructed
    // ShaderCompiler instances. Existing instances are unaffected
    // unless they call setCacheDirectory explicitly. Empty disables
    // caching.
    //
    // Typical caller: VulkanRHI::Initialize ->
    //     SetDefaultCacheDirectory(GET_SYSTEM(ProjectInfo)
    //         ->GetIntermediateShadersRoot()).
    // Also benefits the ShaderLab compiler, which holds an internal
    // `::ShaderCompiler` for its SPIR-V emit stage.
    static void SetDefaultCacheDirectory(const std::filesystem::path& dir);
    static const std::filesystem::path& GetDefaultCacheDirectory();

private:
    // Internal helper to determine glslang shader stage
    int GetGlslangShaderStage(ShaderStage stage) const;

    // Internal helper to compile GLSL to SPIR-V
    ShaderCompileResult CompileInternal(const std::string& source_code,
                                        ShaderStage stage,
                                        const std::string& shader_name,
                                        const std::vector<std::string>& include_paths,
                                        const ShaderMacros& macros);

    // Helper to build preprocessor defines string from macros
    std::string BuildDefinesString(const ShaderMacros& macros) const;

    // Build an absolute on-disk cache filename for (source-file, stage,
    // defines). Returns an empty path when caching is disabled or when
    // `source_file` is not a real on-disk file.
    std::filesystem::path BuildCacheFilePath(const std::string& source_file,
                                             ShaderStage stage,
                                             const ShaderMacros& macros) const;

    // Try to load a cached SPIR-V blob into out_spirv. Returns true iff
    // the cache file exists, is non-empty, and is newer than
    // `source_file`.
    bool TryLoadCachedSpirv(const std::filesystem::path& cache_file,
                            const std::string& source_file,
                            std::vector<unsigned char>& out_spirv) const;

    // Best-effort write of `spirv` to `cache_file`. Failure (read-only
    // FS, race) is logged once and otherwise ignored -- a missed write
    // just means the next run pays the recompile cost.
    bool WriteCachedSpirv(const std::filesystem::path& cache_file,
                          const std::vector<unsigned char>& spirv) const;

    std::string m_DefaultIncludeDir;

    // On-disk SPIR-V cache root; empty disables caching for this
    // instance.
    std::filesystem::path m_CacheDirectory;
};
