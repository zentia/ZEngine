#include "Runtime/Function/ShaderLab/ShaderLabCompiler.h"

#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <execution>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ZEngine::ShaderLab
{

    namespace
    {

        // FNV-1a 64. Tiny, dependency-free, plenty for cache-key collision resistance
        // across at most a few thousand shader variants per project. We don't need
        // cryptographic strength here -- the worst-case effect of a collision is
        // loading the wrong cache entry, which fails verification (mtime check) or
        // fails to bind at runtime; we always have the source as ground truth.
        uint64_t fnv1a64(const std::string& s)
        {
            uint64_t h = 0xcbf29ce484222325ULL;
            for (unsigned char c : s)
            {
                h ^= static_cast<uint64_t>(c);
                h *= 0x100000001b3ULL;
            }
            return h;
        }

        std::string toHex16(uint64_t v)
        {
            char buf[17];
            std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
            return std::string(buf, 16);
        }

    }  // namespace

    ShaderLabCompiler::ShaderLabCompiler()
    {
        m_ShaderCompiler = std::make_unique<::ShaderCompiler>();
    }

    ShaderLabCompiler::~ShaderLabCompiler() = default;

    bool ShaderLabCompiler::LoadFromFile(const std::string& file_path)
    {
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            m_ParseError = "Failed to open file: " + file_path;
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();
        file.close();

        // Remember the source path so the disk cache can be keyed against it
        // and invalidated on mtime change. We deliberately do NOT canonical()
        // here -- if the user passes a relative path, we hash that as-is so a
        // workspace move still hits the same cache slot.
        std::error_code ec;
        m_SourcePath = std::filesystem::absolute(file_path, ec);
        if (ec)
        {
            m_SourcePath = std::filesystem::path(file_path);
        }

        return LoadFromString(source);
    }

    bool ShaderLabCompiler::LoadFromString(const std::string& source)
    {
        ShaderLabParser parser(source);
        if (!parser.Parse())
        {
            m_ParseError = parser.GetError();
            return false;
        }

        m_Asset = parser.GetAsset();

        if (parser.HasError())
        {
            m_ParseError = parser.GetError();
            return false;
        }

        return true;
    }

    void ShaderLabCompiler::SetCacheDirectory(const std::filesystem::path& dir)
    {
        if (dir.empty())
        {
            m_CacheDirectory.clear();
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // Failing to create the cache dir is not fatal: we just disable caching
        // for this run rather than aborting compilation.
        if (ec)
        {
            m_CacheDirectory.clear();
            return;
        }
        m_CacheDirectory = dir;
    }

    std::filesystem::path ShaderLabCompiler::BuildCacheFilePath(size_t subshader_index,
                                                                size_t pass_index,
                                                                const std::string& program_type,
                                                                const ShaderVariantKey& variant_key) const
    {
        if (m_CacheDirectory.empty() || m_SourcePath.empty())
        {
            return {};
        }

        // Source key: lowered absolute path. Lowering matches the
        // ScriptRegistry deterministic-GUID convention (Hash128(rel_path_lower))
        // so renaming Foo.shader -> foo.shader on a case-insensitive FS doesn't
        // double the cache entries.
        std::string src_key = m_SourcePath.generic_string();
        std::transform(src_key.begin(), src_key.end(), src_key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string src_hash = toHex16(fnv1a64(src_key));

        const std::string variant_hash = toHex16(fnv1a64(StringifyVariantKey(variant_key)));

        // Filename layout: <src>_<sub>_<pass>_<variant>_<stage>.spv
        // - All components are fixed-width except the indices, which are tiny.
        // - .spv extension is purely informational; the file is just raw bytes.
        char filename[256];
        std::snprintf(filename,
                      sizeof(filename),
                      "%s_%zu_%zu_%s_%s.spv",
                      src_hash.c_str(),
                      subshader_index,
                      pass_index,
                      variant_hash.c_str(),
                      program_type.c_str());
        return m_CacheDirectory / filename;
    }

    bool ShaderLabCompiler::TryLoadCachedSpirv(const std::filesystem::path& cache_file,
                                               std::vector<unsigned char>& out_blob) const
    {
        if (cache_file.empty() || m_SourcePath.empty())
        {
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::exists(cache_file, ec) || ec)
        {
            return false;
        }
        // Dependency manifest: the blob is only reusable when the .shader AND
        // every header it included are byte-for-byte as they were when the
        // blob was produced. Comparing only m_SourcePath against the blob's
        // own mtime (the previous rule) missed edits to included headers --
        // the single most common edit in a real shader project.
        //
        // The manifest check subsumes the old source-mtime check because
        // m_SourcePath is always recorded as the first dependency.
        std::string stale_reason;
        if (!ShouldReuseBlob(cache_file, &stale_reason))
        {
            return false;
        }

        std::ifstream ifs(cache_file, std::ios::binary | std::ios::ate);
        if (!ifs.is_open())
        {
            return false;
        }
        const std::streamsize size = ifs.tellg();
        if (size <= 0)
        {
            return false;
        }
        ifs.seekg(0, std::ios::beg);
        out_blob.resize(static_cast<size_t>(size));
        if (!ifs.read(reinterpret_cast<char*>(out_blob.data()), size))
        {
            out_blob.clear();
            return false;
        }
        return true;
    }

    bool ShaderLabCompiler::WriteCachedSpirv(
        const std::filesystem::path& cache_file,
        const std::vector<unsigned char>& blob,
        const std::vector<std::filesystem::path>& dependencies) const
    {
        if (cache_file.empty() || blob.empty())
        {
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(cache_file.parent_path(), ec);
        // create_directories failure is non-fatal -- write may still succeed if
        // the directory already existed via a TOCTOU race, and if it doesn't,
        // the ofstream below will fail and we'll just skip caching.

        std::ofstream ofs(cache_file, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open())
        {
            return false;
        }
        ofs.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        if (!ofs.good())
        {
            return false;
        }

        // Blob is on disk; now record what it was built from. Writing the
        // manifest AFTER the blob matters: a crash between the two leaves a
        // blob without a manifest, which reads as a miss (safe -- recompile),
        // whereas the reverse order can leave a Fresh manifest pointing at an
        // older blob, which reads as a hit (unsafe -- stale SPIR-V).
        std::string manifest_error;
        if (!WriteCacheManifest(cache_file, dependencies, &manifest_error))
        {
            // The blob is still valid, but the next run will treat it as a
            // miss. That is the safe direction; report and continue.
            std::fprintf(stderr, "[ShaderLab] SPIR-V cache manifest not written (%s)\n",
                         manifest_error.c_str());
            return false;
        }

        return true;
    }

    CompiledShaderModule ShaderLabCompiler::Compile(
        size_t subshader_index,
        size_t pass_index,
        const std::string& program_type,
        const ShaderVariantKey& variant_key,
        const std::vector<std::string>& extra_include_paths)
    {
        CompiledShaderModule result;

        if (!m_Asset)
        {
            result.error_message = "No shader loaded";
            return result;
        }

        if (subshader_index >= m_Asset->subshaders.size())
        {
            result.error_message = "SubShader index out of range: " + std::to_string(subshader_index);
            return result;
        }

        const auto& subshader = m_Asset->subshaders[subshader_index];

        if (pass_index >= subshader.passes.size())
        {
            result.error_message = "Pass index out of range: " + std::to_string(pass_index);
            return result;
        }

        const auto& pass = subshader.passes[pass_index];

        if (pass.programs.empty())
        {
            result.error_message = "No programs in pass";
            return result;
        }

        const auto& program = pass.programs[0];  // 目前只取第一个 program

        // 构建缓存键 (in-memory, per-instance). The variant string is
        // canonicalised via StringifyVariantKey() so the key matches what the
        // disk-cache helper produces. The stage is part of the key so vertex
        // and fragment blobs for the same pass/variant cannot share a slot.
        const std::string variant_key_str = StringifyVariantKey(variant_key);
        auto cache_key = std::make_tuple(subshader_index, pass_index, variant_key_str, program_type);

        // 检查内存缓存（同一进程内最快）
        auto it = m_CompileCache.find(cache_key);
        if (it != m_CompileCache.end())
        {
            result.spirv_code = it->second;
            result.success = true;
            // entry_point still has to be derived from the requested stage --
            // it is not stored in the cache, only the blob is.
            if (program_type == "vertex")
            {
                result.entry_point = program.vertex_entry;
            }
            else if (program_type == "compute")
            {
                result.entry_point = program.compute_entry;
            }
            else
            {
                result.entry_point = program.fragment_entry;
            }
            return result;
        }

        // 检查磁盘缓存（跨进程 / 跨启动）。Only attempted when both a cache
        // directory and a source path are known; LoadFromString-only flows
        // intentionally bypass the disk cache (no stable invalidation key).
        const std::filesystem::path cache_file =
            BuildCacheFilePath(subshader_index, pass_index, program_type, variant_key);
        if (!cache_file.empty())
        {
            std::vector<unsigned char> cached_blob;
            if (TryLoadCachedSpirv(cache_file, cached_blob))
            {
                // Populate entry_point from the parsed asset (same logic as the
                // miss-path below) and warm the in-memory cache so subsequent
                // calls in this run skip the file I/O.
                if (program_type == "vertex")
                {
                    result.entry_point = program.vertex_entry;
                }
                else if (program_type == "compute")
                {
                    result.entry_point = program.compute_entry;
                }
                else
                {
                    result.entry_point = program.fragment_entry;
                }
                result.spirv_code = std::move(cached_blob);
                result.success = true;
                m_CompileCache[cache_key] = result.spirv_code;
                return result;
            }
        }

        // 处理 #include
        //
        // Search order: the .shader's own directory, then the configured
        // include directory, then any caller-supplied extras. A missing or
        // cyclic include is a hard error -- the resolver never silently
        // comments the directive out, because doing so hands glslang a source
        // the author never wrote and produces a diagnostic (or, worse, a
        // success) that has nothing to do with the real problem.
        std::vector<std::string> include_paths;
        if (!m_IncludeDirectory.empty())
        {
            include_paths.push_back(m_IncludeDirectory);
        }
        for (const auto& extra : extra_include_paths)
        {
            if (!extra.empty())
            {
                include_paths.push_back(extra);
            }
        }

        IncludeResolution resolved =
            ResolveIncludes(program.source_code, include_paths, m_SourcePath.parent_path());
        if (!resolved.success)
        {
            result.error_message = resolved.error_message;
            return result;
        }

        std::string processed_code = std::move(resolved.code);

        // 编译
        ShaderStage stage = ShaderStage::Fragment;
        if (program_type == "vertex")
        {
            stage = ShaderStage::Vertex;
            result.entry_point = program.vertex_entry;
        }
        else if (program_type == "compute")
        {
            stage = ShaderStage::Compute;
            result.entry_point = program.compute_entry;
        }
        else
        {
            result.entry_point = program.fragment_entry;
        }

        // Unity/Cg pixel shaders end with `: COLOR`, which no modern target
        // accepts. Rewrite only the entry function's own output semantic --
        // a blanket `#define COLOR SV_Target` would also rewrite the input
        // semantics Spine uses for vertex colour and break them instead
        // (see ShaderLabSemanticRewrite.h).
        processed_code =
            RewriteLegacyOutputSemantics(processed_code, result.entry_point, stage).code;

        // 构建宏定义
        const std::map<std::string, std::string> variant_macros = VariantKeyToMacros(variant_key);
        ShaderMacros macros(variant_macros.begin(), variant_macros.end());

        // 编译 shader
        auto compile_result = m_ShaderCompiler->CompileFromSource(
            processed_code,
            stage,
            result.entry_point,
            include_paths,
            macros);

        if (compile_result.success)
        {
            result.spirv_code = compile_result.spirv_code;
            result.success = true;

            // 缓存结果（内存 + 磁盘）。WriteCachedSpirv silently no-ops on
            // empty cache_file, which is what we want when the cache is
            // disabled or no source path is known.
            //
            // The dependency set is the .shader plus every header the resolver
            // inlined. Recording it is what makes an edit to a shared header
            // invalidate the blob; before this, only the .shader's own mtime
            // was looked at.
            m_CompileCache[cache_key] = result.spirv_code;
            if (!cache_file.empty())
            {
                std::vector<std::filesystem::path> dependencies;
                dependencies.reserve(resolved.included_files.size() + 1);
                dependencies.push_back(m_SourcePath);
                for (const auto& included : resolved.included_files)
                {
                    dependencies.push_back(included);
                }
                WriteCachedSpirv(cache_file, result.spirv_code, dependencies);
            }
        }
        else
        {
            result.error_message = compile_result.error_message;
        }

        return result;
    }

    bool ShaderLabCompiler::PrecompileAll(const std::vector<std::string>& include_paths)
    {
        if (!m_Asset)
        {
            m_CompileError = "No shader loaded";
            return false;
        }

        for (size_t subshader_idx = 0; subshader_idx < m_Asset->subshaders.size(); ++subshader_idx)
        {
            const auto& subshader = m_Asset->subshaders[subshader_idx];

            for (size_t pass_idx = 0; pass_idx < subshader.passes.size(); ++pass_idx)
            {
                const auto& pass = subshader.passes[pass_idx];

                if (pass.programs.empty())
                    continue;

                const auto& program = pass.programs[0];

                std::vector<MultiCompileLine> lines;
                ExtractMultiCompileLines(program.source_code, lines);

                bool truncated = false;
                auto variants = GenerateVariantCombinations(lines, 64, &truncated);
                (void)truncated;

                // 编译每个变体
                for (const auto& variant : variants)
                {
                    // Vertex shader
                    Compile(subshader_idx, pass_idx, "vertex", variant, include_paths);

                    // Fragment shader
                    Compile(subshader_idx, pass_idx, "fragment", variant, include_paths);
                }
            }
        }

        return true;
    }

}  // namespace ZEngine::ShaderLab
