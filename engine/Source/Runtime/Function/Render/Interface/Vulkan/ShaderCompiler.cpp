#include "ShaderCompiler.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unordered_set>
#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

#ifndef ZENGINE_RUNTIME_GLSLANG_LINKED
    #define ZENGINE_RUNTIME_GLSLANG_LINKED 0
#endif

// Only enable in-process glslang integration when the build system explicitly linked the
// required Vulkan SDK libraries. Header availability alone is not sufficient on Windows/MSVC,
// because the SDK's prebuilt static libraries can be compiled against a newer STL.
#if ZENGINE_RUNTIME_GLSLANG_LINKED
    #ifdef __has_include
        #if __has_include(<glslang/Public/ShaderLang.h>)
            #include <glslang/Public/ResourceLimits.h>
            #include <glslang/Public/ShaderLang.h>
            #include <glslang/SPIRV/GlslangToSpv.h>
            #define GLSLANG_AVAILABLE 1
        #elif __has_include(<ShaderLang.h>)
            #include <GlslangToSpv.h>
            #include <ResourceLimits.h>
            #include <ShaderLang.h>
            #define GLSLANG_AVAILABLE 1
        #else
            #define GLSLANG_AVAILABLE 0
        #endif
    #else
        #ifdef VULKAN_SDK
            #include <glslang/Public/ResourceLimits.h>
            #include <glslang/Public/ShaderLang.h>
            #include <glslang/SPIRV/GlslangToSpv.h>
            #define GLSLANG_AVAILABLE 1
        #else
            #define GLSLANG_AVAILABLE 0
        #endif
    #endif
#else
    #define GLSLANG_AVAILABLE 0
#endif

namespace
{
    // FNV-1a 64. Same hash style as ShaderLab::ShaderLabCompiler and
    // DX12ShaderCompiler so the three caches can coexist on disk without
    // semantic confusion. Cache-key collisions are not a security issue
    // here -- a collided slot fails the mtime check or, in the worst case,
    // produces an unbindable SPIR-V blob, which is recovered by the caller
    // via the source-of-truth recompile.
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

    // Stable canonicalisation of a defines map -> "K1=V1;K2=V2;...". The
    // std::map already iterates in key-sorted order, so this is essentially
    // free.
    std::string stringifyDefines(const ShaderMacros& macros)
    {
        std::string out;
        out.reserve(macros.size() * 16);
        for (const auto& [k, v] : macros)
        {
            out += k;
            out += '=';
            out += v;
            out += ';';
        }
        return out;
    }

    // Lower-case `s` in place. Used to collapse Windows-style path-casing
    // variants (Foo.glsl vs foo.glsl on NTFS) into a single cache slot,
    // matching the ScriptRegistry deterministic-GUID convention
    // (Hash128(rel_path_lower)).
    std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Module-level default cache directory. Set once at engine init from
    // VulkanRHI::Initialize (which has access to ProjectInfo). All
    // default-constructed ShaderCompiler instances pick this up so the
    // runtime VulkanRHI's `m_ShaderCompiler` AND ShaderLab's internal
    // `::ShaderCompiler` both auto-cache without any change at the call
    // site.
    std::filesystem::path g_default_cache_dir;

    // -------------------------------------------------------------------
    // Conservative recursive #include scanner used for cache invalidation.
    // Mirrors the DX12 backend's scanner; see
    // `dx12_shader_compiler.cpp` for the full design rationale. Briefly:
    //
    // - Cache key encodes (file, stage, defines) but NOT include contents.
    // - Without further checks, editing only an `.glsl` / `.h` would leave
    //   every dependent shader's cache entry stale.
    // - On cache hit, recursively walk `#include "..."` and `#include <...>`
    //   from the top-level source, updating a max-mtime accumulator. If any
    //   transitively-included file is newer than the cache blob, treat the
    //   cache as stale; the next compile will overwrite the slot in place
    //   (key didn't change since we don't fold include contents into it).
    // - Blind text scan (no `#if` gating respected) -> conservative
    //   over-counting, never under-counting. False-positive recompiles cost
    //   a few hundred ms; false-negative stale binaries cost hours of
    //   debugging.
    // - Recursion guarded by visited set + 64-deep cap.
    // -------------------------------------------------------------------

    bool readFileToString(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs.is_open())
        {
            return false;
        }
        const std::streamsize size = ifs.tellg();
        if (size < 0)
        {
            return false;
        }
        out.resize(static_cast<size_t>(size));
        ifs.seekg(0, std::ios::beg);
        if (size == 0)
        {
            return true;
        }
        return ifs.read(out.data(), size).good();
    }

    std::filesystem::path resolveIncludePath(const std::string& header,
                                             const std::filesystem::path& current_file,
                                             const std::vector<std::string>& include_paths)
    {
        std::error_code ec;

        if (!current_file.empty())
        {
            std::filesystem::path candidate = current_file.parent_path() / header;
            if (std::filesystem::exists(candidate, ec) && !ec)
            {
                return candidate;
            }
        }

        for (const auto& root : include_paths)
        {
            if (root.empty())
            {
                continue;
            }
            std::filesystem::path candidate = std::filesystem::path(root) / header;
            if (std::filesystem::exists(candidate, ec) && !ec)
            {
                return candidate;
            }
        }

        return {};
    }

    void scanIncludesRecursive(const std::string& source,
                               const std::filesystem::path& current_file,
                               const std::vector<std::string>& include_paths,
                               std::unordered_set<std::string>& visited,
                               std::filesystem::file_time_type& latest_mtime,
                               int depth)
    {
        if (depth > 64)
        {
            return;
        }

        size_t pos = 0;
        while (pos < source.size())
        {
            const size_t line_end = source.find('\n', pos);
            const size_t line_stop = (line_end == std::string::npos) ? source.size() : line_end;

            size_t i = pos;
            while (i < line_stop && (source[i] == ' ' || source[i] == '\t'))
            {
                ++i;
            }

            if (i < line_stop && source[i] == '#')
            {
                ++i;
                while (i < line_stop && (source[i] == ' ' || source[i] == '\t'))
                {
                    ++i;
                }
                static constexpr char kKeyword[] = "include";
                constexpr size_t kKeywordLen = sizeof(kKeyword) - 1;
                if (i + kKeywordLen <= line_stop &&
                    std::memcmp(source.data() + i, kKeyword, kKeywordLen) == 0)
                {
                    i += kKeywordLen;
                    while (i < line_stop && (source[i] == ' ' || source[i] == '\t'))
                    {
                        ++i;
                    }
                    if (i < line_stop && (source[i] == '"' || source[i] == '<'))
                    {
                        const char open = source[i];
                        const char close = (open == '"') ? '"' : '>';
                        ++i;
                        const size_t name_begin = i;
                        while (i < line_stop && source[i] != close)
                        {
                            ++i;
                        }
                        if (i < line_stop)
                        {
                            const std::string header(source.data() + name_begin, i - name_begin);
                            if (!header.empty())
                            {
                                const std::filesystem::path resolved =
                                    resolveIncludePath(header, current_file, include_paths);
                                if (!resolved.empty())
                                {
                                    std::error_code ec;
                                    const std::filesystem::path canon =
                                        std::filesystem::weakly_canonical(resolved, ec);
                                    std::string key = ec ? resolved.generic_string() : canon.generic_string();
                                    std::transform(key.begin(),
                                                   key.end(),
                                                   key.begin(),
                                                   [](unsigned char c) {
                                                       return static_cast<char>(std::tolower(c));
                                                   });
                                    if (visited.insert(key).second)
                                    {
                                        const auto mtime = std::filesystem::last_write_time(resolved, ec);
                                        if (!ec && mtime > latest_mtime)
                                        {
                                            latest_mtime = mtime;
                                        }

                                        std::string nested_source;
                                        if (readFileToString(resolved, nested_source))
                                        {
                                            scanIncludesRecursive(nested_source,
                                                                  resolved,
                                                                  include_paths,
                                                                  visited,
                                                                  latest_mtime,
                                                                  depth + 1);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (line_end == std::string::npos)
            {
                break;
            }
            pos = line_end + 1;
        }
    }

    std::string quoteShellArgument(std::string value)
    {
        for (char& ch : value)
        {
            if (ch == '\\')
            {
                ch = '/';
            }
        }

        return '"' + value + '"';
    }

    std::string quoteShellArgument(const std::filesystem::path& value)
    {
        return quoteShellArgument(value.generic_string());
    }

    const char* getGlslangValidatorStageArgument(ShaderStage stage)
    {
        switch (stage)
        {
            case ShaderStage::Vertex:
                return "vert";
            case ShaderStage::Fragment:
                return "frag";
            case ShaderStage::Geometry:
                return "geom";
            case ShaderStage::TessellationControl:
                return "tesc";
            case ShaderStage::TessellationEvaluation:
                return "tese";
            case ShaderStage::Compute:
                return "comp";
            case ShaderStage::Mesh:
                return "mesh";
            case ShaderStage::Task:
                return "task";
            case ShaderStage::RayGen:
                return "rgen";
            case ShaderStage::RayClosestHit:
                return "rchit";
            case ShaderStage::RayMiss:
                return "rmiss";
            case ShaderStage::RayCallable:
                return "rcall";
            default:
                return "vert";
        }
    }

    std::filesystem::path makeTemporaryShaderPath(const std::string& shader_name, ShaderStage stage)
    {
        std::filesystem::path base_dir = std::filesystem::temp_directory_path();
        if (!shader_name.empty())
        {
            std::filesystem::path shader_path(shader_name);
            if (shader_path.has_parent_path() && std::filesystem::exists(shader_path.parent_path()))
            {
                base_dir = shader_path.parent_path();
            }
        }

        std::string stem = "zengine_runtime_shader";
        if (!shader_name.empty())
        {
            std::string preferred_stem = std::filesystem::path(shader_name).stem().string();
            if (!preferred_stem.empty())
            {
                stem = preferred_stem;
            }
        }

        const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return base_dir / (stem + "_" + std::to_string(timestamp) + "." + getGlslangValidatorStageArgument(stage));
    }

    std::string readTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return {};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    bool readBinaryFile(const std::filesystem::path& path, std::vector<unsigned char>& out_data)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            return false;
        }

        const std::streamsize size = file.tellg();
        if (size < 0)
        {
            return false;
        }

        out_data.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        if (size == 0)
        {
            return true;
        }

        return file.read(reinterpret_cast<char*>(out_data.data()), size).good();
    }

#ifdef Z_GLSLANG_VALIDATOR_EXECUTABLE
    ShaderCompileResult compileWithExternalGlslangValidator(const std::string& source_code,
                                                            ShaderStage stage,
                                                            const std::string& shader_name,
                                                            const std::vector<std::string>& include_paths,
                                                            const ShaderMacros& macros)
    {
        ShaderCompileResult result;

        const std::filesystem::path validator_path(Z_GLSLANG_VALIDATOR_EXECUTABLE);
        if (!std::filesystem::exists(validator_path))
        {
            result.error_message = "glslangValidator executable not found: " + validator_path.generic_string();
            return result;
        }

        std::filesystem::path shader_path;
        bool remove_shader_source = false;
        if (!shader_name.empty())
        {
            const std::filesystem::path candidate_path(shader_name);
            if (std::filesystem::exists(candidate_path))
            {
                shader_path = candidate_path;
            }
        }

        if (shader_path.empty())
        {
            shader_path = makeTemporaryShaderPath(shader_name, stage);
            std::ofstream temp_shader_file(shader_path, std::ios::binary);
            if (!temp_shader_file.is_open())
            {
                result.error_message = "Failed to create temporary shader source file: " + shader_path.generic_string();
                return result;
            }

            temp_shader_file.write(source_code.data(), static_cast<std::streamsize>(source_code.size()));
            temp_shader_file.close();
            remove_shader_source = true;
        }

        const std::filesystem::path spirv_path = shader_path.generic_string() + ".spv";
        const std::filesystem::path log_path = shader_path.generic_string() + ".log";

        std::stringstream command;
        command << quoteShellArgument(validator_path) << " -V100 -S " << getGlslangValidatorStageArgument(stage)
                << " -o " << quoteShellArgument(spirv_path);

        for (const auto& include_path : include_paths)
        {
            if (!include_path.empty())
            {
                command << ' ' << quoteShellArgument("-I" + std::filesystem::path(include_path).generic_string());
            }
        }

        for (const auto& [name, value] : macros)
        {
            if (!name.empty())
            {
                command << ' ' << quoteShellArgument(value.empty() ? "-D" + name : "-D" + name + "=" + value);
            }
        }

        command << ' ' << quoteShellArgument(shader_path) << " > " << quoteShellArgument(log_path) << " 2>&1";

    #if defined(__APPLE__) && TARGET_OS_IPHONE
        result.error_message = "Runtime shader compilation via system() is unavailable on iOS";
    #else
        // std::system() on Windows forwards to `cmd.exe /c <cmd>`. cmd.exe applies
        // a quote-stripping rule when /S is not set: if the command starts with a
        // double quote AND ends with a double quote, the outer pair is dropped.
        // Our command starts with the quoted validator path but ends with `2>&1`,
        // which trips an inconsistent path inside cmd.exe's parser on some
        // configurations -- glslangValidator gets re-tokenized, the `>` redirect
        // is silently dropped, and we end up with an empty .log file plus exit
        // code 1 with no diagnostic. Wrapping the entire command in an outer pair
        // of quotes triggers the documented strip-and-run path uniformly across
        // builds. On POSIX std::system passes the string straight to /bin/sh
        // which doesn't care about the wrap, so the same string works everywhere.
        std::string command_str = command.str();
        #if defined(_WIN32)
        command_str = "\"" + command_str + "\"";
        #endif
        const int exit_code = std::system(command_str.c_str());
        if (exit_code != 0)
        {
            result.error_message = readTextFile(log_path);
            if (result.error_message.empty())
            {
                // The redirected log file itself is missing/empty -- this usually
                // means cmd.exe could not even spawn glslangValidator (path quoting
                // issue, missing exe at the recorded location, or the > redirect
                // failed because the temp dir is not writable by cmd's shell).
                // Surface the exact command string so the failure is debuggable
                // without re-attaching a debugger.
                result.error_message = "glslangValidator failed with exit code " + std::to_string(exit_code) +
                                       "\n  command: " + command_str +
                                       "\n  shader_path: " + shader_path.generic_string() +
                                       "\n  log_path: " + log_path.generic_string() +
                                       "\n  validator_exists: " +
                                       (std::filesystem::exists(validator_path) ? "yes" : "no") +
                                       "\n  shader_path_exists: " +
                                       (std::filesystem::exists(shader_path) ? "yes" : "no") +
                                       "\n  log_path_exists: " +
                                       (std::filesystem::exists(log_path) ? "yes" : "no");
            }
        }
        else if (!readBinaryFile(spirv_path, result.spirv_code))
        {
            result.error_message = "glslangValidator succeeded but failed to read generated SPIR-V: " +
                                   spirv_path.generic_string();
        }
        else
        {
            result.success = true;
        }
    #endif

        std::error_code cleanup_error;
        std::filesystem::remove(log_path, cleanup_error);
        std::filesystem::remove(spirv_path, cleanup_error);
        if (remove_shader_source)
        {
            std::filesystem::remove(shader_path, cleanup_error);
        }

        return result;
    }
#endif
}  // namespace

#if GLSLANG_AVAILABLE
// Custom include handler for glslang
class ShaderIncluder : public glslang::TShader::Includer
{
public:
    ShaderIncluder(const std::vector<std::string>& include_paths)
        : m_IncludePaths(include_paths) {}

    // For system includes (<header>)
    virtual IncludeResult*
    includeSystem(const char* header_name, const char* includer_name, size_t inclusion_depth) override
    {
        return findInclude(header_name, includer_name);
    }

    // For local includes ("header")
    virtual IncludeResult*
    includeLocal(const char* header_name, const char* includer_name, size_t inclusion_depth) override
    {
        return findInclude(header_name, includer_name);
    }

    virtual void releaseInclude(IncludeResult* result) override
    {
        if (result)
        {
            delete[] result->headerData;
            delete result;
        }
    }

private:
    IncludeResult* findInclude(const char* header_name, const char* includer_name)
    {
        std::string header_str(header_name);

        // Try to find the include file in the include paths
        for (const auto& include_path : m_IncludePaths)
        {
            std::filesystem::path full_path = std::filesystem::path(include_path) / header_str;

            if (std::filesystem::exists(full_path))
            {
                return loadIncludeFile(full_path.string(), header_str);
            }
        }

        // If includer_name is provided, try relative to the includer file
        if (includer_name && strlen(includer_name) > 0)
        {
            std::filesystem::path includer_path(includer_name);
            std::filesystem::path relative_path = includer_path.parent_path() / header_str;

            if (std::filesystem::exists(relative_path))
            {
                return loadIncludeFile(relative_path.string(), header_str);
            }
        }

        // Include not found
        std::string error = "Cannot find include file: " + header_str;
        char* error_data = new char[error.length() + 1];
        std::strcpy(error_data, error.c_str());

        return new IncludeResult("", error_data, error.length(), nullptr);
    }

    IncludeResult* loadIncludeFile(const std::string& file_path, const std::string& header_name)
    {
        std::ifstream file(file_path);
        if (!file.is_open())
        {
            std::string error = "Failed to open include file: " + file_path;
            char* error_data = new char[error.length() + 1];
            std::strcpy(error_data, error.c_str());
            return new IncludeResult("", error_data, error.length(), nullptr);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();

        // Allocate memory for the content
        char* content_data = new char[content.length() + 1];
        std::memcpy(content_data, content.c_str(), content.length());
        content_data[content.length()] = '\0';

        return new IncludeResult(file_path, content_data, content.length(), nullptr);
    }

    std::vector<std::string> m_IncludePaths;
};
#endif
ShaderCompiler::ShaderCompiler()
{
    // Pick up the engine-wide default cache dir set by
    // VulkanRHI::Initialize. Empty path means "caching disabled" --
    // compileInternal will short-circuit the cache lookup/write paths.
    m_CacheDirectory = g_default_cache_dir;

#if GLSLANG_AVAILABLE
    // Initialize glslang
    glslang::InitializeProcess();
#endif
}

ShaderCompiler::~ShaderCompiler()
{
#if GLSLANG_AVAILABLE
    // Finalize glslang
    glslang::FinalizeProcess();
#endif
}

int ShaderCompiler::GetGlslangShaderStage(ShaderStage stage) const
{
#if GLSLANG_AVAILABLE
    switch (stage)
    {
        case ShaderStage::Vertex:
            return EShLangVertex;
        case ShaderStage::Fragment:
            return EShLangFragment;
        case ShaderStage::Geometry:
            return EShLangGeometry;
        case ShaderStage::TessellationControl:
            return EShLangTessControl;
        case ShaderStage::TessellationEvaluation:
            return EShLangTessEvaluation;
        case ShaderStage::Compute:
            return EShLangCompute;
        case ShaderStage::Mesh:
            return EShLangMeshNV;  // or EShLangMeshEXT
        case ShaderStage::Task:
            return EShLangTaskNV;  // or EShLangTaskEXT
        case ShaderStage::RayGen:
            return EShLangRayGen;
        case ShaderStage::RayClosestHit:
            return EShLangClosestHit;
        case ShaderStage::RayMiss:
            return EShLangMiss;
        case ShaderStage::RayCallable:
            return EShLangCallable;
        default:
            return EShLangVertex;
    }
#else
    (void)stage;
    return 0;
#endif
}

ShaderCompileResult ShaderCompiler::CompileFromFile(const std::string& file_path,
                                                    ShaderStage stage,
                                                    const std::vector<std::string>& include_paths,
                                                    const ShaderMacros& macros)
{
    ShaderCompileResult result;

    // Read shader file
    std::ifstream file(file_path);
    if (!file.is_open())
    {
        result.error_message = "Failed to open shader file: " + file_path;
        return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source_code = buffer.str();
    file.close();

    return CompileFromSource(source_code, stage, file_path, include_paths, macros);
}

ShaderCompileResult ShaderCompiler::CompileFromSource(const std::string& source_code,
                                                      ShaderStage stage,
                                                      const std::string& shader_name,
                                                      const std::vector<std::string>& include_paths,
                                                      const ShaderMacros& macros)
{
    return CompileInternal(source_code, stage, shader_name, include_paths, macros);
}

std::string ShaderCompiler::BuildDefinesString(const ShaderMacros& macros) const
{
    if (macros.empty())
    {
        return "";
    }

    std::stringstream defines;
    for (const auto& [name, value] : macros)
    {
        defines << "#define " << name;
        if (!value.empty())
        {
            defines << " " << value;
        }
        defines << "\n";
    }
    return defines.str();
}

ShaderCompileResult ShaderCompiler::CompileInternal(const std::string& source_code,
                                                    ShaderStage stage,
                                                    const std::string& shader_name,
                                                    const std::vector<std::string>& include_paths,
                                                    const ShaderMacros& macros)
{
    ShaderCompileResult result;

    // ----- On-disk cache lookup -----
    // The cache is consulted only when (a) caching is enabled for this
    // instance and (b) `shader_name` resolves to an existing on-disk
    // file (i.e. compileFromFile or compileFromSource-with-real-path).
    // Pure in-memory sources can't be safely cached: there's no stable
    // mtime ground truth, so any cache hit would be unverifiable.
    std::filesystem::path cache_file;
    bool cache_eligible = false;
    if (!m_CacheDirectory.empty() && !shader_name.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(shader_name, ec) && !ec)
        {
            cache_file = BuildCacheFilePath(shader_name, stage, macros);
            cache_eligible = !cache_file.empty();
        }
    }

    if (cache_eligible)
    {
        std::vector<unsigned char> cached;
        if (TryLoadCachedSpirv(cache_file, shader_name, cached))
        {
            // Second-stage freshness check: tryLoadCachedSpirv only
            // compared the cache file's mtime against the top-level
            // source file's mtime, so editing only an `.glsl` / `.h`
            // include wouldn't have invalidated the entry. Compute the
            // recursive max(include-mtime) and bail if any included
            // file is newer than the cache blob; the next compile path
            // overwrites this slot in place. See the DX12 backend's
            // matching block for the full design rationale.
            std::error_code ec;
            const auto cache_time = std::filesystem::last_write_time(cache_file, ec);
            bool include_fresh = !ec;
            if (include_fresh)
            {
                std::vector<std::string> all_include_paths_for_scan = include_paths;
                if (!m_DefaultIncludeDir.empty())
                {
                    all_include_paths_for_scan.push_back(m_DefaultIncludeDir);
                }

                std::string top_source;
                if (readFileToString(shader_name, top_source))
                {
                    std::unordered_set<std::string> visited;
                    auto latest = cache_time;
                    const auto src_mt = std::filesystem::last_write_time(shader_name, ec);
                    if (!ec && src_mt > latest)
                    {
                        latest = src_mt;
                    }
                    scanIncludesRecursive(top_source,
                                          std::filesystem::path(shader_name),
                                          all_include_paths_for_scan,
                                          visited,
                                          latest,
                                          /*depth=*/0);
                    if (latest > cache_time)
                    {
                        include_fresh = false;
                    }
                }
                else
                {
                    include_fresh = false;
                }
            }

            if (include_fresh)
            {
                result.spirv_code = std::move(cached);
                result.success = true;
                return result;
            }
        }
    }

#if GLSLANG_AVAILABLE
    // Build include paths list (add default include dir if set)
    std::vector<std::string> all_include_paths = include_paths;
    if (!m_DefaultIncludeDir.empty())
    {
        all_include_paths.push_back(m_DefaultIncludeDir);
    }

    // Create include handler
    ShaderIncluder includer(all_include_paths);

    // Build preprocessor defines string
    std::string defines_str = BuildDefinesString(macros);

    // Prepend defines to source code
    std::string final_source = defines_str + source_code;

    // Get shader stage
    EShLanguage shader_lang = static_cast<EShLanguage>(GetGlslangShaderStage(stage));

    // Create shader object
    glslang::TShader shader(shader_lang);

    const char* source_cstr = final_source.c_str();
    shader.setStrings(&source_cstr, 1);

    // Set entry point (default is "main")
    shader.setEntryPoint("main");

    // Set source file name for error reporting
    if (!shader_name.empty())
    {
        shader.setSourceFile(shader_name.c_str());
    }

    // Set client target (Vulkan)
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    // Parse shader with include handler
    // Use default resource limits - these are defined in glslang/Public/ResourceLimits.h
    const TBuiltInResource* default_resources = GetDefaultResources();
    TBuiltInResource resources = *default_resources;
    EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.Parse(&resources, 100, false, messages, includer))
    {
        result.error_message = "Shader compilation failed:\n";
        result.error_message += shader.getInfoLog();
        result.error_message += "\n";
        result.error_message += shader.getInfoDebugLog();
        return result;
    }

    // Link shader (for multi-stage shaders, but we compile single stages here)
    glslang::TProgram program;
    program.addShader(&shader);

    if (!program.link(messages))
    {
        result.error_message = "Shader linking failed:\n";
        result.error_message += program.getInfoLog();
        result.error_message += "\n";
        result.error_message += program.getInfoDebugLog();
        return result;
    }

    // Generate SPIR-V
    glslang::SpvOptions spv_options;
    spv_options.generateDebugInfo = false;
    spv_options.disableOptimizer = false;
    spv_options.optimizeSize = false;

    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(shader_lang), spirv, &spv_options);

    // Convert to byte vector
    result.spirv_code.resize(spirv.size() * sizeof(unsigned int));
    std::memcpy(result.spirv_code.data(), spirv.data(), result.spirv_code.size());

    result.success = true;
#else
    #ifdef Z_GLSLANG_VALIDATOR_EXECUTABLE
    result = compileWithExternalGlslangValidator(source_code, stage, shader_name, include_paths, macros);
    #else
    result.error_message = "Runtime glslang libraries are disabled or unavailable. Reconfigure with "
                           "-DZENGINE_LINK_VULKAN_SDK_GLSLANG_LIBS=ON, or ensure glslangValidator is "
                           "available in the Vulkan SDK bin directory.";
    result.success = false;
    #endif
#endif

    // ----- On-disk cache write-back -----
    // Best-effort. Failure here is logged once but never propagated to
    // the caller -- the compile succeeded, the cache miss next run is
    // the only observable consequence.
    if (cache_eligible && result.success && !result.spirv_code.empty())
    {
        if (!WriteCachedSpirv(cache_file, result.spirv_code))
        {
            LOG_WARNING(ZShader,
                        "VulkanShaderCompiler: failed to write cache file {}",
                        cache_file.generic_string());
        }
    }

    return result;
}

void ShaderCompiler::SetIncludeDirectory(const std::string& include_dir)
{
    m_DefaultIncludeDir = include_dir;
}

void ShaderCompiler::SetCacheDirectory(const std::filesystem::path& dir)
{
    if (dir.empty())
    {
        m_CacheDirectory.clear();
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Failing to create the cache dir is not fatal: we just disable
    // caching for this instance rather than aborting compilation.
    if (ec)
    {
        LOG_WARNING(ZShader,
                    "VulkanShaderCompiler: cannot create cache dir {} ({}); caching disabled",
                    dir.generic_string(),
                    ec.message());
        m_CacheDirectory.clear();
        return;
    }
    m_CacheDirectory = dir;
}

void ShaderCompiler::SetDefaultCacheDirectory(const std::filesystem::path& dir)
{
    if (dir.empty())
    {
        g_default_cache_dir.clear();
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        LOG_WARNING(ZShader,
                    "VulkanShaderCompiler: cannot create default cache dir {} ({}); caching disabled",
                    dir.generic_string(),
                    ec.message());
        g_default_cache_dir.clear();
        return;
    }
    g_default_cache_dir = dir;
}

const std::filesystem::path& ShaderCompiler::GetDefaultCacheDirectory()
{
    return g_default_cache_dir;
}

std::filesystem::path ShaderCompiler::BuildCacheFilePath(const std::string& source_file,
                                                         ShaderStage stage,
                                                         const ShaderMacros& macros) const
{
    if (m_CacheDirectory.empty() || source_file.empty())
    {
        return {};
    }

    // Source key: lowered absolute path. Lowering matches the
    // ScriptRegistry deterministic-GUID convention so renaming
    // Foo.glsl -> foo.glsl on a case-insensitive FS doesn't double the
    // cache entries.
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(source_file, ec);
    if (ec || abs.empty())
    {
        abs = std::filesystem::path(source_file);
    }
    const std::string src_hash = toHex16(fnv1a64(toLower(abs.generic_string())));

    // Variant key: stage + sorted defines. Entry point is hard-coded to
    // "main" by the glslang path (shader.setEntryPoint("main") in
    // compileInternal), so it doesn't need to participate in the key.
    const int stage_int = static_cast<int>(stage);
    std::string variant_str = std::to_string(stage_int);
    variant_str += '|';
    variant_str += stringifyDefines(macros);
    const std::string variant_hash = toHex16(fnv1a64(variant_str));

    // Filename layout: <src>_<stage>_<variant>.spv
    char filename[256];
    std::snprintf(filename,
                  sizeof(filename),
                  "%s_%d_%s.spv",
                  src_hash.c_str(),
                  stage_int,
                  variant_hash.c_str());
    return m_CacheDirectory / filename;
}

bool ShaderCompiler::TryLoadCachedSpirv(const std::filesystem::path& cache_file,
                                        const std::string& source_file,
                                        std::vector<unsigned char>& out_spirv) const
{
    if (cache_file.empty() || source_file.empty())
    {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cache_file, ec) || ec)
    {
        return false;
    }
    if (!std::filesystem::exists(source_file, ec) || ec)
    {
        // Source vanished but cache exists: prefer to recompile (which
        // will also fail) so the user sees a coherent error rather than
        // a stale success.
        return false;
    }

    const auto src_time = std::filesystem::last_write_time(source_file, ec);
    if (ec)
    {
        return false;
    }
    const auto cache_time = std::filesystem::last_write_time(cache_file, ec);
    if (ec)
    {
        return false;
    }
    if (cache_time < src_time)
    {
        // Source has been edited since this cache entry was written.
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
    out_spirv.resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(out_spirv.data()), size))
    {
        out_spirv.clear();
        return false;
    }
    return true;
}

bool ShaderCompiler::WriteCachedSpirv(const std::filesystem::path& cache_file,
                                      const std::vector<unsigned char>& spirv) const
{
    if (cache_file.empty() || spirv.empty())
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(cache_file.parent_path(), ec);
    // create_directories failure is non-fatal -- write may still
    // succeed if the directory already existed via a TOCTOU race, and
    // if it doesn't, the ofstream below will fail and we'll just skip
    // caching.

    std::ofstream ofs(cache_file, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open())
    {
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(spirv.data()),
              static_cast<std::streamsize>(spirv.size()));
    return ofs.good();
}