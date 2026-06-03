#pragma once

#include "Function/Render/Interface/Vulkan/ShaderCompiler.h"
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
        CompiledShaderModule Compile(size_t subshader_index,
                                     size_t pass_index,
                                     const std::string& program_type = "fragment",
                                     const ShaderVariantKey& variant_key = {});

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
        // path when the cached blob is newer than the .shader source on disk.
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
        // 处理 #include 指令
        std::string ProcessIncludes(const std::string& hlsl_code,
                                    const std::vector<std::string>& include_paths);

        // Disk-cache helpers (no-ops when m_CacheDirectory is empty).
        std::filesystem::path BuildCacheFilePath(size_t subshader_index,
                                                 size_t pass_index,
                                                 const std::string& program_type,
                                                 const ShaderVariantKey& variant_key) const;
        bool TryLoadCachedSpirv(const std::filesystem::path& cache_file,
                                std::vector<unsigned char>& out_blob) const;
        bool WriteCachedSpirv(const std::filesystem::path& cache_file,
                              const std::vector<unsigned char>& blob) const;

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

        // 编译缓存: (subshader_idx, pass_idx, variant_key) -> SPIR-V
        std::map<std::tuple<size_t, size_t, std::string>, std::vector<unsigned char>> m_CompileCache;
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
