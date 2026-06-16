#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Render/RenderType.h"
#include "Runtime/Function/ShaderLab/ShaderLabHlslExtract.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_set>

#pragma comment(lib, "dxcompiler.lib")

namespace
{
    // Global embed-debug flag for PIX / RenderDoc shader debugging.
    // Controlled by console variable `r.Shaders.EmbedDebugInfo`.
    static bool g_embed_debug_info = false;

    // FNV-1a 64. Same hash style as ShaderLab::ShaderLabCompiler so the two
    // caches can coexist on disk without semantic confusion. Cache-key
    // collisions are not a security issue here -- a collided slot fails the
    // mtime check or, in the worst case, produces an unbindable DXIL blob,
    // which is recovered by the caller via the source-of-truth recompile.
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

    // Stable canonicalisation of a defines map -> "K1=V1;K2=V2;..." with keys
    // sorted lexicographically. std::map already iterates in key order, so this
    // is essentially free. Kept as a free function so the on-disk cache key
    // stays in lock-step with anyone else hashing the same defines (e.g. a
    // future runtime or shader-graph layer).
    std::string stringifyDefines(const std::map<std::string, std::string>& macros)
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

    // Lower-case `s` in place. Used to collapse Windows path-casing variants
    // (Foo.hlsl vs foo.hlsl on NTFS) into a single cache slot, matching the
    // ScriptRegistry deterministic-GUID convention (Hash128(rel_path_lower)).
    std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Module-level default cache directory. Set once at engine init from
    // DX12RHI::Initialize (which has access to ProjectInfo). All
    // default-constructed DX12ShaderCompiler instances pick this up so the
    // preview renderer's `static DX12PreviewRenderer renderer;` and the
    // inspector's local `DX12ShaderCompiler compiler;` both auto-cache without
    // any change at the call site.
    std::filesystem::path g_default_cache_dir;

    // -------------------------------------------------------------------
    // Conservative recursive #include scanner used for cache invalidation.
    //
    // Why this exists: the cache key (file_path + stage + entry + defines)
    // does not encode the contents of any `#include`d header. Without
    // further checks, editing only `Common.hlsli` would leave every
    // dependent shader's cache entry stale -- the user's app would silently
    // run yesterday's code. To plug that hole, on cache lookup we compute
    // the maximum mtime of (main source + every transitively-included
    // file) and compare it against the cache file's mtime. Any header
    // touch invalidates the cache; the next compile rewrites it.
    //
    // Strategy: blind text scan. We look for `#include "..."` and
    // `#include <...>` lines and recurse. We do NOT respect `#if` /
    // `#ifdef` gating, so a header guarded by `#if 0` is still walked. The
    // result is **conservatively over-counted** mtimes -- we may
    // occasionally invalidate a cache entry that the real preprocessor
    // wouldn't have touched. That is the right side of the safety
    // trade-off: false-positive recompiles cost a few hundred ms on
    // startup, false-negative stale binaries cost hours of debugging.
    //
    // Cycles are guarded by a visited set. We cap recursion depth at 64 to
    // catch malformed inputs without spending forever.
    //
    // This is NOT used to compute the cache key, only to compute the
    // freshness comparator. So when a header changes, the cache file name
    // stays the same -- the next compile just overwrites it in place.
    // -------------------------------------------------------------------

    // Read entire file into a string. Returns false on any error.
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

    // Try to resolve `header` against (a) the directory of `current_file`,
    // then (b) each entry in `include_paths`. Returns the first existing
    // resolved path, or an empty path on miss.
    std::filesystem::path resolveIncludePath(const std::string& header,
                                             const std::filesystem::path& current_file,
                                             const std::vector<std::string>& include_paths)
    {
        std::error_code ec;

        // (a) relative to the current file's directory
        if (!current_file.empty())
        {
            std::filesystem::path candidate = current_file.parent_path() / header;
            if (std::filesystem::exists(candidate, ec) && !ec)
            {
                return candidate;
            }
        }

        // (b) each include search root
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

    // Extract every `#include "..."` and `#include <...>` from `source`,
    // resolve each against the search roots, and recursively walk into
    // resolved files. Updates `latest_mtime` to the max of (current value,
    // every visited file's last_write_time). The caller seeds
    // `latest_mtime` with the mtime of the top-level source.
    //
    // `current_file` is empty for compileFromSource calls where the
    // "source path" is synthetic; in that case only include_paths are
    // consulted for resolution.
    void scanIncludesRecursive(const std::string& source,
                               const std::filesystem::path& current_file,
                               const std::vector<std::string>& include_paths,
                               std::unordered_set<std::string>& visited,
                               std::filesystem::file_time_type& latest_mtime,
                               int depth)
    {
        if (depth > 64)
        {
            // Pathological input; bail out -- cache will be conservative.
            return;
        }

        // Scan line-by-line. We don't use a real preprocessor; we just
        // look for the textual pattern  `^\s*#\s*include\s*("..."|<...>)`.
        size_t pos = 0;
        while (pos < source.size())
        {
            const size_t line_end = source.find('\n', pos);
            const size_t line_stop = (line_end == std::string::npos) ? source.size() : line_end;

            // Trim leading whitespace.
            size_t i = pos;
            while (i < line_stop && (source[i] == ' ' || source[i] == '\t'))
            {
                ++i;
            }

            // Match `#`.
            if (i < line_stop && source[i] == '#')
            {
                ++i;
                // Optional whitespace between `#` and `include`.
                while (i < line_stop && (source[i] == ' ' || source[i] == '\t'))
                {
                    ++i;
                }
                // Match `include`.
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
                                    std::string key = ec
                                                          ? resolved.generic_string()
                                                          : canon.generic_string();
                                    std::transform(key.begin(),
                                                   key.end(),
                                                   key.begin(),
                                                   [](unsigned char c) {
                                                       return static_cast<char>(std::tolower(c));
                                                   });
                                    if (visited.insert(key).second)
                                    {
                                        const auto mtime =
                                            std::filesystem::last_write_time(resolved, ec);
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

    bool IsShaderLabSourcePath(const std::string& file_path)
    {
        std::string ext = std::filesystem::path(file_path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".shader";
    }

}  // namespace

// DX12ShaderIncluder implementation
DX12ShaderIncluder::DX12ShaderIncluder(ComPtr<IDxcLibrary> library, const std::vector<std::string>& include_paths)
    : m_Library(library), m_IncludePaths(include_paths)
{
}

HRESULT STDMETHODCALLTYPE DX12ShaderIncluder::LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource)
{
    // Convert wide string to narrow string
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, pFilename, -1, NULL, 0, NULL, NULL);
    std::string filename(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, pFilename, -1, &filename[0], size_needed, NULL, NULL);

    // Try to find the file in include paths
    std::ifstream file;
    std::string full_path;

    // First try the include paths
    for (const auto& include_path : m_IncludePaths)
    {
        full_path = include_path + "/" + filename;
        file.open(full_path, std::ios::binary);
        if (file.is_open())
        {
            break;
        }
    }

    // If not found, try the filename directly
    if (!file.is_open())
    {
        file.open(filename, std::ios::binary);
        if (file.is_open())
        {
            full_path = filename;
        }
    }

    if (!file.is_open())
    {
        return E_FAIL;
    }

    // Read file contents
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();
    file.close();

    // Create blob from source. MUST copy onto the heap (OnHeapCopy), NOT pin
    // (FromPinned): DXC reads the returned blob lazily, after LoadSource has already
    // returned and the local `source` string is destroyed. A pinned blob would point
    // at that freed memory, so DXC would see whatever overwrote it -- in practice
    // leading NUL bytes -- corrupting every included .hlsli (e.g. the `#ifndef` guard
    // and `static const float PI` declarations get zeroed), making any multi-include
    // shader fail to compile. A failed compile never populates the DXIL cache, so the
    // PSO build retries the compile every frame (~hundreds of ms each) and tanks the
    // frame rate. OnHeapCopy gives the blob its own owned copy that outlives this call.
    ComPtr<IDxcBlobEncoding> blob;
    HRESULT hr = m_Library->CreateBlobWithEncodingOnHeapCopy(
        source.data(), static_cast<UINT32>(source.size()), CP_UTF8, blob.GetAddressOf());

    if (SUCCEEDED(hr))
    {
        *ppIncludeSource = blob.Detach();
        return S_OK;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE DX12ShaderIncluder::QueryInterface(REFIID riid, void** ppvObject)
{
    if (riid == __uuidof(IDxcIncludeHandler))
    {
        *ppvObject = this;
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DX12ShaderIncluder::AddRef()
{
    return ++m_RefCount;
}

ULONG STDMETHODCALLTYPE DX12ShaderIncluder::Release()
{
    ULONG ref = --m_RefCount;
    if (ref == 0)
    {
        delete this;
    }
    return ref;
}

// DX12ShaderCompiler implementation
DX12ShaderCompiler::DX12ShaderCompiler()
{
    // Pick up the engine-wide default cache dir set by DX12RHI::Initialize.
    // Empty path means "caching disabled" -- compileInternal will short-
    // circuit the cache lookup/write paths.
    m_CacheDirectory = g_default_cache_dir;

    // Initialize DXC
    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&m_Library));
    if (FAILED(hr))
    {
        LOG_ERROR(ZShader, "Failed to create DXC library");
        return;
    }

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_Compiler));
    if (FAILED(hr))
    {
        LOG_ERROR(ZShader,
                  "Failed to create DXC compiler (HRESULT=0x{:08X}). "
                  "Copy dxcompiler.dll and dxil.dll next to the executable or add Vulkan SDK Bin to PATH.",
                  static_cast<uint32_t>(hr));
        return;
    }

    hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&m_Utils));
    if (FAILED(hr))
    {
        // Utils is optional, just log a warning
        LOG_WARNING(ZShader, "Failed to create DXC utils (optional)");
    }

    // Register console variable for PIX / RenderDoc shader debugging.
    // g_embed_debug_info is a static bool in this translation unit's
    // anonymous namespace (line ~24).  When the user sets
    //   r.Shaders.EmbedDebugInfo 1
    // in the console, DXC will receive -Zi -Qembed_debug -Od
    // on the NEXT recompile of any shader compiled through this
    // DX12ShaderCompiler instance (or any instance that shares
    // the same static flag -- they don't, each TU has its own;
    // but the console variable writes to g_embed_debug_info
    // which CompileInternal reads).
    if (auto* console = GET_SYSTEM(ConsoleManager))
    {
        console->RegisterBoolVariable(
            "r.Shaders.EmbedDebugInfo",
            "Embed PIX/RenderDoc debug info in DX12 shaders.\n"
            "When true, -Zi -Qembed_debug -Od are passed to DXC.\n"
            "Affected shaders must be recompiled to pick up the change\n"
            "(delete Intermediate/Shaders/ cache or touch the .hlsl file).",
            false,
            &g_embed_debug_info);
    }
}

DX12ShaderCompiler::~DX12ShaderCompiler() {}

std::string DX12ShaderCompiler::GetShaderProfile(ShaderStage shader_stage) const
{
    switch (shader_stage)
    {
        case ShaderStage::Vertex:
            return "vs_6_0";
        case ShaderStage::Fragment:
            return "ps_6_0";
        case ShaderStage::Geometry:
            return "gs_6_0";
        case ShaderStage::TessellationControl:
            return "hs_6_0";
        case ShaderStage::TessellationEvaluation:
            return "ds_6_0";
        case ShaderStage::Compute:
            return "cs_6_0";
        case ShaderStage::Mesh:
            return "ms_6_5";
        case ShaderStage::Task:
            return "as_6_5";
        default:
            LOG_WARNING(ZShader, "Unknown shader stage {}, defaulting to vs_6_0", static_cast<int>(shader_stage));
            return "vs_6_0";
    }
}

std::string DX12ShaderCompiler::BuildDefinesString(const std::map<std::string, std::string>& macros) const
{
    std::string defines;
    for (const auto& [key, value] : macros)
    {
        defines += "#define " + key;
        if (!value.empty())
        {
            defines += " " + value;
        }
        defines += "\n";
    }
    return defines;
}

DX12ShaderCompileResult DX12ShaderCompiler::CompileFromFile(const std::string& file_path,
                                                            ShaderStage shader_stage,
                                                            const std::vector<std::string>& include_paths,
                                                            const std::map<std::string, std::string>& macros,
                                                            const std::string& entry_point,
                                                            const std::string& target_profile,
                                                            const std::string& hlsl_version,
                                                            bool embed_debug)

{
    DX12ShaderCompileResult result;

    if (IsShaderLabSourcePath(file_path))
    {
        const ZEngine::ShaderLab::ShaderLabHlslExtractResult extracted =
            ZEngine::ShaderLab::ExtractHlslFromShaderLabFile(file_path, shader_stage, entry_point);
        if (!extracted.ok)
        {
            result.error_message = extracted.error_message.empty()
                                       ? ("Failed to extract HLSL from ShaderLab file: " + file_path)
                                       : extracted.error_message;
            return result;
        }

        std::string resolved_entry = entry_point;
        if (resolved_entry.empty() || resolved_entry == "main")
        {
            resolved_entry = extracted.entry_point;
        }

        std::vector<std::string> merged_includes = include_paths;
        const std::filesystem::path parent_dir = std::filesystem::path(file_path).parent_path();
        if (!parent_dir.empty())
        {
            const std::string parent_generic = parent_dir.generic_string();
            if (std::find(merged_includes.begin(), merged_includes.end(), parent_generic) == merged_includes.end())
            {
                merged_includes.push_back(parent_generic);
            }
        }

        return CompileFromSource(extracted.hlsl_source,
                                 shader_stage,
                                 file_path,
                                 merged_includes,
                                 macros,
                                 resolved_entry,
                                 target_profile,
                                 hlsl_version,
                                 embed_debug);
    }

    // Read plain HLSL file
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

    return CompileFromSource(source_code,
                             shader_stage,
                             file_path,
                             include_paths,
                             macros,
                             entry_point,
                             target_profile,
                             hlsl_version,
                             embed_debug);
}

DX12ShaderCompileResult DX12ShaderCompiler::CompileFromSource(const std::string& hlsl_source,
                                                              ShaderStage shader_stage,
                                                              const std::string& shader_name,
                                                              const std::vector<std::string>& include_paths,
                                                              const std::map<std::string, std::string>& macros,
                                                              const std::string& entry_point,
                                                              const std::string& target_profile,
                                                              const std::string& hlsl_version,
                                                              bool embed_debug)
{
    return CompileInternal(hlsl_source,
                           shader_stage,
                           shader_name,
                           include_paths,
                           macros,
                           entry_point,
                           target_profile,
                           hlsl_version,
                           embed_debug);
}

void DX12ShaderCompiler::SetIncludeDirectory(const std::string& include_dir)
{
    m_DefaultIncludeDir = include_dir;
}

void DX12ShaderCompiler::SetCacheDirectory(const std::filesystem::path& dir)
{
    if (dir.empty())
    {
        m_CacheDirectory.clear();
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Failing to create the cache dir is not fatal: we just disable caching
    // for this instance rather than aborting compilation.
    if (ec)
    {
        LOG_WARNING(ZShader,
                    "DX12ShaderCompiler: cannot create cache dir {} ({}); caching disabled",
                    dir.generic_string(),
                    ec.message());
        m_CacheDirectory.clear();
        return;
    }
    m_CacheDirectory = dir;
}

void DX12ShaderCompiler::SetDefaultCacheDirectory(const std::filesystem::path& dir)
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
                    "DX12ShaderCompiler: cannot create default cache dir {} ({}); caching disabled",
                    dir.generic_string(),
                    ec.message());
        g_default_cache_dir.clear();
        return;
    }
    g_default_cache_dir = dir;
}

const std::filesystem::path& DX12ShaderCompiler::GetDefaultCacheDirectory()
{
    return g_default_cache_dir;
}

int DX12ShaderCompiler::InvalidateCacheForSource(const std::filesystem::path& source_file)
{
    // The default cache directory is the only one in production use --
    // DX12RHI::Initialize wires it from ProjectInfo::GetIntermediateShadersRoot,
    // and every default-constructed DX12ShaderCompiler picks it up. Per-instance
    // overrides via SetCacheDirectory() are isolated test paths only.
    if (g_default_cache_dir.empty())
    {
        return 0;
    }

    std::error_code ec;
    if (!std::filesystem::exists(g_default_cache_dir, ec) || ec)
    {
        return 0;
    }

    // MUST mirror buildCacheFilePath's src-key derivation EXACTLY. Any
    // drift between these two and PR-AI2's invalidation silently misses
    // the cache and we'd ship stale DXIL. Copy the same FNV-1a / toLower
    // / generic_string / absolute incantation:
    std::error_code abs_ec;
    std::filesystem::path abs = std::filesystem::absolute(source_file, abs_ec);
    if (abs_ec || abs.empty())
    {
        abs = source_file;
    }
    const std::string src_hash = toHex16(fnv1a64(toLower(abs.generic_string())));
    // Filename layout produced by buildCacheFilePath is
    // `<src_hash>_<stage>_<variant_hash>.dxil`. We match by prefix so a
    // single source change wipes every (stage, entry, defines, profile,
    // hlsl_version) variant in one call.
    const std::string prefix = src_hash + "_";

    int deleted = 0;
    for (auto it = std::filesystem::directory_iterator(g_default_cache_dir, ec);
         !ec && it != std::filesystem::directory_iterator();
         it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (!it->is_regular_file(ec) || ec)
        {
            continue;
        }
        const std::string fn = it->path().filename().string();
        if (fn.size() <= prefix.size() || fn.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        // Defensive ext check so we never delete unrelated files that
        // happen to start with the same 16-char hex prefix.
        if (it->path().extension() != ".dxil")
        {
            continue;
        }
        std::error_code rm_ec;
        std::filesystem::remove(it->path(), rm_ec);
        if (rm_ec)
        {
            LOG_WARNING(ZShader,
                        "DX12 shader cache: failed to remove '{}': {}",
                        it->path().string(),
                        rm_ec.message());
            continue;
        }
        ++deleted;
    }

    if (deleted > 0)
    {
        LOG_INFO(ZShader,
                 "DX12 shader cache: invalidated {} variant(s) for source '{}'",
                 deleted,
                 abs.generic_string());
    }
    return deleted;
}

std::filesystem::path
DX12ShaderCompiler::BuildCacheFilePath(const std::string& source_file,
                                       ShaderStage shader_stage,
                                       const std::string& entry_point,
                                       const std::map<std::string, std::string>& macros,
                                       const std::string& target_profile,
                                       const std::string& hlsl_version) const
{
    if (m_CacheDirectory.empty() || source_file.empty())
    {
        return {};
    }

    // Source key: lowered absolute path. Lowering matches the
    // ScriptRegistry deterministic-GUID convention so renaming
    // Foo.hlsl -> foo.hlsl on a case-insensitive FS doesn't double the
    // cache entries.
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(source_file, ec);
    if (ec || abs.empty())
    {
        abs = std::filesystem::path(source_file);
    }
    const std::string src_hash = toHex16(fnv1a64(toLower(abs.generic_string())));

    // Variant key: stage + entry + sorted defines + target_profile +
    // hlsl_version. Profile and HLSL year are fed in explicitly because
    // PR5b lets callers override the stage-derived default profile
    // (e.g. "ps_6_6" for bindless materials, "" for legacy SM 6.0). Two
    // compiles of the same source under different profiles MUST NOT
    // collide on the same cache slot -- different DXIL bytecodes,
    // different shader-stage entry tables, etc.
    std::string variant_str = entry_point;
    variant_str += '|';
    variant_str += std::to_string(static_cast<int>(shader_stage));
    variant_str += '|';
    variant_str += stringifyDefines(macros);
    variant_str += '|';
    variant_str += target_profile;  // empty == "use stage default"
    variant_str += '|';
    variant_str += hlsl_version;  // empty == "DXC default (HV 2018)"
    const std::string variant_hash = toHex16(fnv1a64(variant_str));

    // Filename layout: <src>_<stage>_<variant>.dxil
    // - .dxil extension is purely informational; the file is just raw bytes.
    // - Stage is written explicitly into the filename for ease of debugging
    //   (you can `ls *_<stage>_*.dxil` to find all PS-stage cache hits).
    char filename[256];
    std::snprintf(filename,
                  sizeof(filename),
                  "%s_%d_%s.dxil",
                  src_hash.c_str(),
                  static_cast<int>(shader_stage),
                  variant_hash.c_str());
    return m_CacheDirectory / filename;
}

bool DX12ShaderCompiler::TryLoadCachedDxil(const std::filesystem::path& cache_file,
                                           const std::string& source_file,
                                           std::vector<uint8_t>& out_dxil) const
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
        // Source vanished but cache exists: prefer to recompile (which will
        // also fail) so the user sees a coherent error rather than a stale
        // success.
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
    out_dxil.resize(static_cast<size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(out_dxil.data()), size))
    {
        out_dxil.clear();
        return false;
    }
    return true;
}

bool DX12ShaderCompiler::WriteCachedDxil(const std::filesystem::path& cache_file,
                                         const std::vector<uint8_t>& dxil) const
{
    if (cache_file.empty() || dxil.empty())
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
    ofs.write(reinterpret_cast<const char*>(dxil.data()),
              static_cast<std::streamsize>(dxil.size()));
    return ofs.good();
}

DX12ShaderCompileResult DX12ShaderCompiler::CompileInternal(const std::string& hlsl_source,
                                                            ShaderStage shader_stage,
                                                            const std::string& shader_name,
                                                            const std::vector<std::string>& include_paths,
                                                            const std::map<std::string, std::string>& macros,
                                                            const std::string& entry_point,
                                                            const std::string& target_profile,
                                                            const std::string& hlsl_version,
                                                            bool embed_debug)
{
    DX12ShaderCompileResult result;

    if (!m_Library || !m_Compiler)
    {
        result.error_message = "DXC library or compiler not initialized";
        return result;
    }

    const std::string resolved_entry_point = entry_point.empty() ? "main" : entry_point;

    // ----- On-disk cache lookup -----
    // The cache is consulted only when (a) caching is enabled for this
    // instance and (b) `shader_name` resolves to an existing on-disk file
    // (i.e. compileFromFile or compileFromSource-with-real-path). Pure
    // in-memory sources can't be safely cached: there's no stable mtime
    // ground truth, so any cache hit would be unverifiable.
    std::filesystem::path cache_file;
    bool cache_eligible = false;
    if (!m_CacheDirectory.empty() && !shader_name.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(shader_name, ec) && !ec)
        {
            cache_file = BuildCacheFilePath(shader_name,
                                            shader_stage,
                                            resolved_entry_point,
                                            macros,
                                            target_profile,
                                            hlsl_version);
            cache_eligible = !cache_file.empty();
        }
    }

    if (cache_eligible)
    {
        std::vector<uint8_t> cached;
        if (TryLoadCachedDxil(cache_file, shader_name, cached))
        {
            // Second-stage freshness check: tryLoadCachedDxil only compared
            // the cache file's mtime against the top-level source file's
            // mtime, so editing only an `.hlsli` (or any other transitively
            // included header) wouldn't have invalidated the entry. To plug
            // that hole, compute the recursive max(include-mtime) and bail
            // out of the cache if any included file is newer than the cache
            // blob. The next compile path will overwrite this slot in place
            // (no key change required since the key doesn't depend on
            // include contents).
            //
            // We pay the include-scan cost ONLY on cache hit -- on cache
            // miss the cost is dwarfed by DXC compilation, and on this
            // hot-path the scan is just a few file reads.
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
                    auto latest = cache_time;  // seed; we only widen.
                    // Re-seed with top-level source's own mtime so we
                    // never wrongly down-grade. (Already covered by the
                    // first-stage check, but kept explicit for clarity.)
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
                // If we couldn't read the top-level source (just deleted?
                // Permission flap?) treat as not-fresh -- the recompile
                // path will surface a meaningful error.
                else
                {
                    include_fresh = false;
                }
            }

            if (include_fresh)
            {
                result.dxil_code = std::move(cached);
                result.success = true;
                return result;
            }
        }
    }

    // Build include paths list (add default include dir if set)
    std::vector<std::string> all_include_paths = include_paths;
    if (!m_DefaultIncludeDir.empty())
    {
        all_include_paths.push_back(m_DefaultIncludeDir);
    }

    // Create include handler
    DX12ShaderIncluder includer(m_Library, all_include_paths);

    // Build preprocessor defines string
    std::string defines_str = BuildDefinesString(macros);

    // Prepend defines to source code
    std::string final_source = defines_str + hlsl_source;

    // Get shader profile.
    //
    // PR5b: a non-empty `target_profile` overrides the stage-derived
    // default. Used for SM 6.6 bindless-aware shaders (`*_6_6`) without
    // forcing every legacy SM 6.0 shader through the new pipeline.
    // Empty string preserves the legacy stage-derived default
    // (`vs_6_0`, `ps_6_0`, ...) so existing call sites are byte-for-
    // byte unchanged.
    std::string profile = target_profile.empty() ? GetShaderProfile(shader_stage) : target_profile;

    // Create source blob
    ComPtr<IDxcBlobEncoding> source_blob;
    HRESULT hr = m_Library->CreateBlobWithEncodingFromPinned(
        (LPBYTE)final_source.c_str(), static_cast<UINT32>(final_source.size()), CP_UTF8, source_blob.GetAddressOf());

    if (FAILED(hr))
    {
        result.error_message = "Failed to create source blob";
        return result;
    }

    // Prepare arguments
    std::vector<LPCWSTR> arguments;

    // Convert profile to wide string
    std::wstring profile_wide(profile.begin(), profile.end());
    std::wstring entry_point_wide(resolved_entry_point.begin(), resolved_entry_point.end());

    // Entry point and target profile are passed through IDxcCompiler::Compile parameters below.

    // Optimization / debug info for PIX / RenderDoc source mapping.
    // Check both the per-call parameter and the global console variable.
    if (embed_debug || g_embed_debug_info)
    {
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(L"-Od");
    }
    else
    {
        arguments.push_back(L"-O3");
    }

    // PR5b: HLSL language version override.
    //
    // DXC defaults to HLSL 2018 when `-HV` is omitted. Bindless-aware
    // shaders that use `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`
    // -- a HLSL 2021 feature -- pass `hlsl_version="2021"` to opt in.
    // Empty string => no `-HV` flag => DXC's default (HV 2018).
    //
    // The wide-string buffer must outlive the `Compile()` call below
    // because `arguments` only stores raw pointers, so it is declared
    // here at function scope, not inside the if-block.
    std::wstring hv_arg_wide;
    if (!hlsl_version.empty())
    {
        hv_arg_wide.assign(hlsl_version.begin(), hlsl_version.end());
        arguments.push_back(L"-HV");
        arguments.push_back(hv_arg_wide.c_str());
    }

    // Enable warnings as errors in debug
#ifdef _DEBUG
    arguments.push_back(L"-WX");
#endif

    // Compile shader
    ComPtr<IDxcOperationResult> compile_result;
    hr = m_Compiler->Compile(source_blob.Get(),
                             shader_name.empty() ? nullptr
                                                 : std::wstring(shader_name.begin(), shader_name.end()).c_str(),
                             entry_point_wide.c_str(),
                             profile_wide.c_str(),
                             arguments.data(),
                             static_cast<UINT32>(arguments.size()),
                             nullptr,
                             0,
                             &includer,
                             compile_result.GetAddressOf());

    if (FAILED(hr))
    {
        result.error_message = "Failed to compile shader: HRESULT = " + std::to_string(hr);
        return result;
    }

    // Check compilation status
    HRESULT compile_status;
    hr = compile_result->GetStatus(&compile_status);
    if (FAILED(hr))
    {
        result.error_message = "Failed to get compilation status";
        return result;
    }

    if (FAILED(compile_status))
    {
        // Get error messages
        ComPtr<IDxcBlobEncoding> errors;
        compile_result->GetErrorBuffer(errors.GetAddressOf());

        if (errors)
        {
            result.error_message = std::string((char*)errors->GetBufferPointer(), errors->GetBufferSize());
        }
        else
        {
            result.error_message = "Shader compilation failed (unknown error)";
        }
        return result;
    }

    // Get compiled shader blob
    ComPtr<IDxcBlob> shader_blob;
    hr = compile_result->GetResult(shader_blob.GetAddressOf());
    if (FAILED(hr) || !shader_blob)
    {
        result.error_message = "Failed to get compiled shader blob";
        return result;
    }

    // Copy DXIL bytecode to result
    result.dxil_code.resize(shader_blob->GetBufferSize());
    std::memcpy(result.dxil_code.data(), shader_blob->GetBufferPointer(), shader_blob->GetBufferSize());

    result.success = true;

    // ----- On-disk cache write-back -----
    // Best-effort. Failure here is logged once but never propagated to the
    // caller -- the compile succeeded, the cache miss next run is the only
    // observable consequence.
    if (cache_eligible && result.success && !result.dxil_code.empty())
    {
        if (!WriteCachedDxil(cache_file, result.dxil_code))
        {
            LOG_WARNING(ZShader,
                        "DX12ShaderCompiler: failed to write cache file {}",
                        cache_file.generic_string());
        }
    }

    return result;
}