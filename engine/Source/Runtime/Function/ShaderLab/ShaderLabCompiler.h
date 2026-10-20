#pragma once

#include "Function/Render/Interface/Vulkan/ShaderCompiler.h"
#include "ShaderLabCacheManifest.h"
#include "ShaderLabIncludeProcessor.h"
#include "ShaderLabParser.h"
#include "ShaderLabVariant.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    // 编译后的 Shader Module
    struct CompiledShaderModule
    {
        std::vector<unsigned char> spirv_code;
        std::string entry_point;
        std::string error_message;
        bool success = false;
    };

    // ============= ShaderLab 编译器 =============
    class ShaderLabCompiler
    {
    public:
        ShaderLabCompiler();
        ~ShaderLabCompiler();

        // 从文件加载并解析 ShaderLab
        bool LoadFromFile(const std::string& file_path);

        // 从字符串加载并解析
        bool LoadFromString(const std::string& source);

        // 编译指定 SubShader 和 Pass
        //
        // `extra_include_paths` is appended after the configured include
        // directory. PrecompileAll() forwards its own argument here; it used
        // to be dropped on the floor, so a precompile pass silently resolved
        // includes against a different set of directories than an explicit
        // Compile() call did.
        CompiledShaderModule Compile(size_t subshader_index,
                                     size_t pass_index,
                                     const std::string& program_type = "fragment",
                                     const ShaderVariantKey& variant_key = {},
                                     const std::vector<std::string>& extra_include_paths = {});

        // 编译所有变体（预处理时使用）
        bool PrecompileAll(const std::vector<std::string>& include_paths = {});

        // 获取 ShaderLab 资源
        std::shared_ptr<ShaderLabAsset> GetAsset() const { return m_Asset; }

        // 获取解析错误
        const std::string& GetParseError() const { return m_ParseError; }
        bool HasParseError() const { return !m_ParseError.empty(); }

        // 获取编译错误
        const std::string& GetCompileError() const { return m_CompileError; }
        bool HasCompileError() const { return !m_CompileError.empty(); }

        // 设置 include 目录
        void SetIncludeDirectory(const std::string& dir) { m_IncludeDirectory = dir; }

        // -------------------------------------------------------------------------
        // On-disk SPIR-V cache. When a non-empty directory is set, every
        // successful Compile() persists its SPIR-V blob under
        //   <cache_dir>/<source-hash>_<sub>_<pass>_<variant-hash>_<stage>.spv
        // and subsequent Compile() calls (across runs) skip the heavy slang/dxc
        // path when the blob is still valid.
        //
        // Validity is decided by a dependency manifest written next to the blob
        // (see ShaderLabCacheManifest.h). The input set is the .shader PLUS
        // every header the #include resolver pulled in -- comparing only the
        // .shader's mtime, as this used to do, left every cached blob looking
        // fresh after a shared header was edited.
        //
        // This is the editor-side equivalent of UE's DerivedDataCache for
        // shaders. Pass an empty path to disable caching (default).
        //
        // Pointed at <project>/Intermediate/Shaders/ by ShaderingManager-equivalent
        // boot code; safe to delete the directory between runs to force a full
        // recompile.
        // -------------------------------------------------------------------------
        void SetCacheDirectory(const std::filesystem::path& dir);
        const std::filesystem::path& GetCacheDirectory() const { return m_CacheDirectory; }

    private:
        // Disk-cache helpers (no-ops when m_CacheDirectory is empty).
        std::filesystem::path BuildCacheFilePath(size_t subshader_index,
                                                 size_t pass_index,
                                                 const std::string& program_type,
                                                 const ShaderVariantKey& variant_key) const;
        bool TryLoadCachedSpirv(const std::filesystem::path& cache_file,
                                std::vector<unsigned char>& out_blob) const;

        // `dependencies` is the full input set the blob was built from: the
        // .shader itself, then every file ResolveIncludes() pulled in. It is
        // recorded next to `cache_file` so the next run can tell whether any
        // input changed. Without it the manifest would be missing and every
        // load would be a miss, so passing an empty set is a bug, not an
        // optimisation.
        bool WriteCachedSpirv(const std::filesystem::path& cache_file,
                              const std::vector<unsigned char>& blob,
                              const std::vector<std::filesystem::path>& dependencies) const;

    private:
        std::shared_ptr<ShaderLabAsset> m_Asset;
        std::string m_ParseError;
        std::string m_CompileError;
        std::string m_IncludeDirectory;

        // Absolute path to the .shader source backing m_Asset, if loaded via
        // LoadFromFile. Empty when LoadFromString was used; in that case the
        // disk cache is skipped (we have nothing stable to key against).
        std::filesystem::path m_SourcePath;

        // Disk cache directory. Empty by default = caching disabled. See
        // SetCacheDirectory() for the on-disk layout.
        std::filesystem::path m_CacheDirectory;

        std::unique_ptr<::ShaderCompiler> m_ShaderCompiler;

        // 编译缓存: (subshader_idx, pass_idx, variant_key, stage) -> SPIR-V
        //
        // The stage is part of the key. It used to be
        // (subshader, pass, variant) only, which means "vertex" and
        // "fragment" shared one slot: PrecompileAll() compiles vertex then
        // fragment for the same variant, so the fragment request hit the
        // in-memory entry the vertex compile had just written and returned
        // the VERTEX blob (with a fragment entry point stapled on). The disk
        // cache always keyed on stage, so the two tiers also disagreed.
        std::map<std::tuple<size_t, size_t, std::string, std::string>, std::vector<unsigned char>>
            m_CompileCache;
    };

    // ============= 便捷函数 =============

    inline std::shared_ptr<ShaderLabAsset> LoadShaderLabFromFile(const std::string& file_path)
    {
        return ParseShaderLabFromFile(file_path);
    }

    inline std::shared_ptr<ShaderLabAsset> LoadShaderLabFromString(const std::string& source)
    {
        return ParseShaderLabFromString(source);
    }

}  // namespace ZEngine::ShaderLab
