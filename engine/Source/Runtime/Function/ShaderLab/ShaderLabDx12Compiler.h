#pragma once

#include "ShaderLabParser.h"
#include "ShaderLabVariant.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    struct CompiledShaderModuleDx12
    {
        std::vector<uint8_t> dxil_code;
        std::string entry_point;
        std::string error_message;
        bool success = false;
    };

    /// ShaderLab -> DXIL compiler (Windows / DX12 path). Mirrors ShaderLabCompiler
    /// but uses DX12ShaderCompiler + ShaderLabHlslExtract instead of Vulkan SPIR-V.
    class ShaderLabDx12Compiler
    {
    public:
        ShaderLabDx12Compiler();
        ~ShaderLabDx12Compiler();

        bool LoadFromFile(const std::string& file_path);
        bool LoadFromString(const std::string& source);

        CompiledShaderModuleDx12 Compile(size_t subshader_index,
                                         size_t pass_index,
                                         const std::string& program_type = "fragment",
                                         const ShaderVariantKey& variant_key = {});

        /// Precompile variant combinations (vertex + fragment) into the DXIL cache.
        /// When `strip_unused_shader_features` is true, `#pragma multi_compile` lines get a
        /// full Cartesian product; `#pragma shader_feature` variants are limited to keys
        /// listed in `material_variant_keys` (Unity build strip). Pass nullptr/empty when
        /// no materials reference this shader yet.
        bool PrecompileAll(const std::vector<std::string>& include_paths = {},
                           const std::vector<ShaderVariantKey>* material_variant_keys = nullptr,
                           bool strip_unused_shader_features = true);

        std::shared_ptr<ShaderLabAsset> GetAsset() const { return m_Asset; }
        const std::string& GetParseError() const { return m_ParseError; }
        bool HasParseError() const { return !m_ParseError.empty(); }

        void SetCacheDirectory(const std::filesystem::path& dir);
        const std::filesystem::path& GetCacheDirectory() const { return m_CacheDirectory; }

        /// Precompile every `.shader` under `shaders_root` (editor startup / reimport).
        using MaterialVariantKeysByShader = std::map<std::string, std::vector<ShaderVariantKey>>;

        static int PrecompileProjectShaders(const std::filesystem::path& shaders_root,
                                            const std::filesystem::path& cache_directory,
                                            size_t max_variants_per_pass = 64,
                                            const MaterialVariantKeysByShader* material_variant_keys = nullptr);

    private:
        std::shared_ptr<ShaderLabAsset> m_Asset;
        std::string m_ParseError;
        std::string m_SourceText;
        std::filesystem::path m_SourcePath;
        std::filesystem::path m_CacheDirectory;
    };

}  // namespace ZEngine::ShaderLab
