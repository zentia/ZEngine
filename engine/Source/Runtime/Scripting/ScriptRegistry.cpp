#include "ScriptRegistry.h"

#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Platform/Encoding/EncodingUtils.h"
#include "Runtime/Project/ProjectInfo.h"
#include "core/Log/LogSystem.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <regex>

namespace
{
    // FNV-1a 64-bit constants (FNV reference parameters).
    constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

    // 64-bit FNV-1a over arbitrary bytes. Used twice with different seeds to
    // produce a deterministic 128-bit hash for GUID derivation.
    uint64_t fnv1a64(const uint8_t* data, size_t len, uint64_t seed)
    {
        uint64_t h = seed;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= static_cast<uint64_t>(data[i]);
            h *= kFnvPrime;
        }
        return h;
    }

    eastl::string toHex64(uint64_t v)
    {
        static const char* digits = "0123456789abcdef";
        char buf[16];
        for (int i = 15; i >= 0; --i)
        {
            buf[i] = digits[v & 0xF];
            v >>= 4;
        }
        return eastl::string(buf, 16);
    }

    // Read up to N bytes of a text file. Returns empty string on failure.
    std::string readSmallTextFile(const std::filesystem::path& abs_path, size_t max_bytes = 8 * 1024)
    {
        FILE* f = fopen(abs_path.string().c_str(), "rb");
        if (!f)
        {
            return {};
        }
        std::string buf;
        buf.resize(max_bytes);
        size_t n = fread(buf.data(), 1, max_bytes, f);
        buf.resize(n);
        fclose(f);
        return buf;
    }

    // Read the WHOLE file (no size cap) into a buffer. Used by content-hash
    // rename detection - capping would lump together any two scripts whose
    // first 8 KB happen to match (which is exactly the kind of head-similarity
    // real codebases exhibit through copy-paste / template imports).
    std::string readWholeFile(const std::filesystem::path& abs_path)
    {
        FILE* f = fopen(abs_path.string().c_str(), "rb");
        if (!f)
        {
            return {};
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        if (fsz <= 0)
        {
            fclose(f);
            return {};
        }
        std::string buf(static_cast<size_t>(fsz), '\0');
        fseek(f, 0, SEEK_SET);
        fread(buf.data(), 1, static_cast<size_t>(fsz), f);
        fclose(f);
        return buf;
    }

    // 64-bit FNV-1a -> 16-char lowercase hex. Used as a content fingerprint.
    // We use the same FNV-1a primitive as the GUID derivation so we keep one
    // hash-flavour in the codebase; cryptographic strength isn't needed.
    eastl::string contentHashHex(const std::string& bytes)
    {
        if (bytes.empty())
        {
            return {};
        }
        uint64_t h = fnv1a64(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), kFnvOffsetBasis);
        return toHex64(h);
    }

    // Forward-slash, plus on Windows lower-case (NTFS is case-insensitive in
    // practice; a stable lookup key avoids "Player.ts" vs "player.ts" surprises).
    eastl::string normaliseInternal(const std::filesystem::path& rel_path)
    {
        std::string s = rel_path.generic_string();
#if defined(_WIN32)
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        return eastl::string(s.c_str(), s.size());
    }

    bool hasSupportedScriptExtension(const std::filesystem::path& p)
    {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".ts" || ext == ".tsx" || ext == ".js";
    }

    eastl::string toEastl(const std::string& s)
    {
        return eastl::string(s.c_str(), s.size());
    }

    std::string toStd(const eastl::string& s)
    {
        return std::string(s.c_str(), s.size());
    }
}  // namespace

// =============================================================================
// IEngineSystem
// =============================================================================

std::vector<std::type_index> ScriptRegistry::GetDependencies() const
{
    // We resolve project paths via ProjectInfo, so it must be ready first.
    return {GET_SYSTEM_TYPE(ProjectInfo)};
}

bool ScriptRegistry::Initialize()
{
    auto project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->GetProjectRoot().empty())
    {
        // No project loaded - the user is on the launcher screen. Nothing
        // to do; we'll be re-Initialised when a project is opened.
        LOG_INFO(ZScriptRegistry, "No project loaded; ScriptRegistry idle.");
        return true;
    }

    m_ProjectRoot = project_info->GetProjectRoot();
    m_ScriptsRoot = project_info->GetScriptsRoot();
    m_IntermediateScriptsRoot = project_info->GetIntermediateScriptsRoot();
    m_RegistryFile = project_info->GetScriptRegistryPath();

    // Best-effort load. A missing or corrupt JSON is non-fatal: we'll just
    // start from empty and rebuild deterministically on rescan().
    LoadFromDisk();
    rescan();
    FlushPendingSave();

    LOG_INFO(ZScriptRegistry,
             "Initialised: scripts_root={}, registry={}, entries={}",
             m_ScriptsRoot.generic_string(),
             m_RegistryFile.generic_string(),
             static_cast<int>(m_ByGuid.size()));
    return true;
}

namespace
{
    constexpr auto kRegistrySaveDebounce = std::chrono::milliseconds(500);
}  // namespace

void ScriptRegistry::ScheduleSave()
{
    const auto deadline = std::chrono::steady_clock::now() + kRegistrySaveDebounce;
    std::lock_guard lk(m_Mutex);
    m_SavePending = true;
    m_SaveDeadline = deadline;
}

void ScriptRegistry::FlushPendingSave() const
{
    bool do_save = false;
    {
        std::lock_guard lk(m_Mutex);
        if (!m_SavePending)
        {
            return;
        }
        m_SavePending = false;
        do_save = true;
    }
    if (do_save)
    {
        SaveToDisk();
    }
}

void ScriptRegistry::TickDeferredSave()
{
    bool do_save = false;
    {
        std::lock_guard lk(m_Mutex);
        if (!m_SavePending || std::chrono::steady_clock::now() < m_SaveDeadline)
        {
            return;
        }
        m_SavePending = false;
        do_save = true;
    }
    if (do_save)
    {
        SaveToDisk();
    }
}

void ScriptRegistry::Shutdown()
{
    FlushPendingSave();
    {
        std::lock_guard lk(m_Mutex);
        m_ByPath.clear();
        m_ByGuid.clear();
    }
}

// =============================================================================
// Lookup
// =============================================================================

ScriptAsset* ScriptRegistry::FindByGuid(const eastl::string& guid) const
{
    std::lock_guard lk(m_Mutex);
    auto it = m_ByGuid.find(guid);
    return it == m_ByGuid.end() ? nullptr : it->second.get();
}

ScriptAsset* ScriptRegistry::FindByPath(const eastl::string& source_rel_path) const
{
    // Normalise on the caller side too: callers pass paths fresh from the
    // user (drag&drop, OpenFileDialog, etc.) and shouldn't have to know
    // about our case-folding policy.
    eastl::string key = normaliseInternal(std::filesystem::path(source_rel_path.c_str()));

    std::lock_guard lk(m_Mutex);
    auto it = m_ByPath.find(key);
    return it == m_ByPath.end() ? nullptr : it->second;
}

std::vector<ScriptAsset*> ScriptRegistry::GetAll() const
{
    std::lock_guard lk(m_Mutex);
    std::vector<ScriptAsset*> out;
    out.reserve(m_ByGuid.size());
    for (auto& kv : m_ByGuid)
    {
        out.push_back(kv.second.get());
    }
    return out;
}

std::filesystem::path ScriptRegistry::GetRegistryFilePath() const
{
    return m_RegistryFile;
}

// =============================================================================
// Scan + persistence
// =============================================================================

void ScriptRegistry::rescan()
{
    if (m_ProjectRoot.empty() || m_ScriptsRoot.empty())
    {
        return;
    }

    // Tolerate users who haven't created Scripts/ yet - just yield empty.
    std::error_code ec;
    if (!std::filesystem::exists(m_ScriptsRoot, ec))
    {
        std::lock_guard lk(m_Mutex);
        if (!m_ByGuid.empty())
        {
            m_ByGuid.clear();
            m_ByPath.clear();
            ScheduleSave();
        }
        return;
    }

    auto on_disk = EnumerateScriptFiles(m_ProjectRoot, m_ScriptsRoot);

    bool changed = false;
    {
        std::lock_guard lk(m_Mutex);

        // Pass 1: match on-disk files against existing entries by PATH.
        //   - same path: keep entry; refresh derived fields (mtime, class,
        //     content_hash) if mtime moved.
        //   - new path: stash for pass 2 (we may match it to a vanished
        //     entry by content hash; only forge a new GUID if not).
        //   - old entry whose path is no longer on disk: stash as orphan.
        eastl::unordered_map<eastl::string, bool> on_disk_set;
        on_disk_set.reserve(on_disk.size());
        for (const auto& rel : on_disk)
        {
            on_disk_set[rel] = true;
        }

        std::vector<eastl::string> new_paths;     // on-disk, no existing entry
        std::vector<eastl::string> orphan_guids;  // existing entry, file gone

        for (const auto& rel : on_disk)
        {
            auto pit = m_ByPath.find(rel);
            if (pit != m_ByPath.end())
            {
                ScriptAsset* asset = pit->second;
                std::filesystem::path abs = m_ProjectRoot / std::filesystem::path(toStd(rel));
                int64_t new_mtime = FileMTimeNs(abs);
                if (new_mtime != asset->m_SourceMtimeNs)
                {
                    asset->m_SourceMtimeNs = new_mtime;
                    asset->m_DefaultClassName = ParseDefaultClassName(abs);
                    asset->m_SourceContentHash = contentHashHex(readWholeFile(abs));
                    changed = true;
                }
                else if (asset->m_SourceContentHash.empty())
                {
                    // Backfill content hash for entries loaded from a
                    // pre-content-hash registry version. Without this,
                    // pass 2 wouldn't be able to recover renames spanning
                    // a registry-format upgrade.
                    asset->m_SourceContentHash = contentHashHex(readWholeFile(abs));
                    changed = true;
                }
            }
            else
            {
                new_paths.push_back(rel);
            }
        }

        for (auto& kv : m_ByGuid)
        {
            if (on_disk_set.find(kv.second->m_SourceRelPath) == on_disk_set.end())
            {
                orphan_guids.push_back(kv.first);
            }
        }

        // Pass 2: try to match orphan entries to new paths by content hash.
        //   - If a new path's content hash equals an orphan's stored hash,
        //     this is a rename (UE Redirector equivalent). Update the
        //     entry's path in place; GUID is preserved so all references
        //     keep working.
        //   - Otherwise, the new path becomes a brand-new entry with a
        //     freshly-derived deterministic GUID.
        //
        // Build a hash -> orphan_guid index once. If two orphans share a
        // hash (duplicate identical files were both deleted) we just take
        // the first match - the situation is ambiguous and Either Choice
        // produces a valid registry.
        eastl::unordered_map<eastl::string, eastl::string> orphan_by_hash;
        for (const auto& g : orphan_guids)
        {
            auto it = m_ByGuid.find(g);
            if (it != m_ByGuid.end() && !it->second->m_SourceContentHash.empty())
            {
                orphan_by_hash.insert({it->second->m_SourceContentHash, g});
            }
        }

        for (const auto& rel : new_paths)
        {
            std::filesystem::path abs = m_ProjectRoot / std::filesystem::path(toStd(rel));
            std::string content = readWholeFile(abs);
            eastl::string hash = contentHashHex(content);
            int64_t new_mtime = FileMTimeNs(abs);

            // Try rename detection first.
            if (!hash.empty())
            {
                auto hit = orphan_by_hash.find(hash);
                if (hit != orphan_by_hash.end())
                {
                    auto guid_to_keep = hit->second;
                    auto git = m_ByGuid.find(guid_to_keep);
                    if (git != m_ByGuid.end())
                    {
                        ScriptAsset* asset = git->second.get();

                        // Move the path key in m_ByPath: drop the old
                        // (now-stale) entry, insert under the new rel.
                        m_ByPath.erase(asset->m_SourceRelPath);
                        asset->m_SourceRelPath = rel;
                        asset->m_SourceMtimeNs = new_mtime;
                        asset->m_DefaultClassName = ParseDefaultClassName(abs);
                        asset->m_SourceContentHash = hash;
                        m_ByPath.insert({rel, asset});

                        orphan_by_hash.erase(hit);
                        // Mark the orphan as consumed so the cleanup pass
                        // below doesn't delete it.
                        for (auto oit = orphan_guids.begin(); oit != orphan_guids.end(); ++oit)
                        {
                            if (*oit == guid_to_keep)
                            {
                                orphan_guids.erase(oit);
                                break;
                            }
                        }
                        changed = true;
                        continue;
                    }
                }
            }

            // Genuinely new file: derive a deterministic GUID.
            eastl::string guid = DeterministicGuidFromPath(rel);
            int bump = 0;
            while (m_ByGuid.count(guid) != 0 && bump < 256)
            {
                eastl::string salted = rel + eastl::string(":");
                salted += eastl::string(std::to_string(bump).c_str());
                guid = DeterministicGuidFromPath(salted);
                ++bump;
            }

            auto owned = std::make_unique<ScriptAsset>();
            owned->m_Guid = guid;
            owned->m_SourceRelPath = rel;
            owned->m_SourceMtimeNs = new_mtime;
            owned->m_DefaultClassName = ParseDefaultClassName(abs);
            owned->m_SourceContentHash = hash;
            ScriptAsset* raw = owned.get();
            m_ByGuid.emplace(guid, std::move(owned));
            m_ByPath.emplace(rel, raw);
            changed = true;
        }

        // Pass 3: drop true orphans (nothing matched their content hash, so
        // they really are deletes, not renames).
        for (const auto& g : orphan_guids)
        {
            auto git = m_ByGuid.find(g);
            if (git == m_ByGuid.end())
            {
                continue;
            }
            m_ByPath.erase(git->second->m_SourceRelPath);
            m_ByGuid.erase(git);
            changed = true;
        }

        // Pass 4: reconcile each entry with its compiled `.js` mirror under
        // Intermediate/Scripts/. This is the field the scripting system
        // (TypeScriptComponent::BindAndAwake) actually consults to decide
        // whether the module is ready to load. Without this pass, entries
        // are stuck with `m_CompiledRelPath` empty even after tsc has
        // emitted the .js, and Inspector/binding both report "no compiled
        // output yet".
        //
        // Mapping rule: source `Scripts/sub/Foo.ts(x)` -> compiled
        // `Intermediate/Scripts/sub/Foo.js`. We probe the filesystem rather
        // than recursing the intermediate tree because that's O(N) over
        // entries, not O(M) over .js files; both are small.
        if (!m_IntermediateScriptsRoot.empty())
        {
            for (auto& kv : m_ByGuid)
            {
                ScriptAsset* asset = kv.second.get();
                if (asset == nullptr)
                    continue;

                // Strip "Scripts/" prefix from source rel-path; whatever
                // remains is the path beneath both Scripts/ and
                // Intermediate/Scripts/. enumerateScriptFiles is rooted at
                // m_ScriptsRoot which lives at <project>/Scripts (per
                // ProjectInfo), so all entries start with "scripts/" on
                // case-insensitive platforms.
                const eastl::string& src_rel = asset->m_SourceRelPath;
                std::string src_std = toStd(src_rel);
                static constexpr const char kScriptsPrefix[] = "scripts/";
                static constexpr size_t kPrefLen = sizeof(kScriptsPrefix) - 1;
                std::string sub_path;
                if (src_std.size() > kPrefLen && std::equal(kScriptsPrefix,
                                                            kScriptsPrefix + kPrefLen,
                                                            src_std.begin(),
                                                            [](char a, char b) {
                                                                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                                                            }))
                {
                    sub_path = src_std.substr(kPrefLen);
                }
                else
                {
                    sub_path = src_std;  // defensive; shouldn't trigger in practice
                }

                // Replace .ts/.tsx with .js. .js sources (already
                // hand-authored JavaScript) keep their own path.
                std::filesystem::path js_sub(sub_path);
                std::string ext = js_sub.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".ts" || ext == ".tsx")
                {
                    js_sub.replace_extension(".js");
                }
                else if (ext != ".js")
                {
                    continue;  // unknown source kind; nothing to mirror
                }

                std::filesystem::path js_abs = m_IntermediateScriptsRoot / js_sub;
                std::error_code fs_ec;
                eastl::string desired;
                if (std::filesystem::exists(js_abs, fs_ec) && !fs_ec)
                {
                    std::filesystem::path js_rel = std::filesystem::relative(js_abs, m_ProjectRoot, fs_ec);
                    if (!fs_ec && !js_rel.empty())
                    {
                        desired = normaliseInternal(js_rel);
                    }
                }
                if (desired != asset->m_CompiledRelPath)
                {
                    asset->m_CompiledRelPath = desired;
                    changed = true;
                }
            }
        }
    }

    if (changed)
    {
        ScheduleSave();
    }
}

// =============================================================================
// Live `.js` notifications (driven by TypeScriptCompiler's FileSystemWatcher
// in the Editor; safe no-op in standalone Player builds where it's never
// called).
// =============================================================================

void ScriptRegistry::OnCompiledJsChanged(const std::filesystem::path& abs_js_path)
{
    if (m_IntermediateScriptsRoot.empty() || abs_js_path.empty())
        return;

    std::error_code ec;
    // Reject paths outside <project>/Intermediate/Scripts/. lexically_relative
    // returns ".." prefix in that case, so we just check filesystem::relative.
    std::filesystem::path rel_to_interm = std::filesystem::relative(abs_js_path, m_IntermediateScriptsRoot, ec);
    if (ec || rel_to_interm.empty() || rel_to_interm.string().rfind("..", 0) == 0)
    {
        return;
    }

    // Map back to the source rel-path: "Scripts/<rel_to_interm>" with .js
    // swapped to .ts (preferring .ts over .tsx; if .ts doesn't exist we
    // still try .tsx). enumerateScriptFiles already lower-cases on Windows,
    // so the lookup keys agree.
    std::filesystem::path src_rel_ts = std::filesystem::path("Scripts") / rel_to_interm;
    src_rel_ts.replace_extension(".ts");
    eastl::string key_ts = normaliseInternal(src_rel_ts);

    eastl::string compiled_rel_normalised;
    {
        std::filesystem::path js_rel = std::filesystem::relative(abs_js_path, m_ProjectRoot, ec);
        if (ec || js_rel.empty())
            return;
        compiled_rel_normalised = normaliseInternal(js_rel);
    }

    bool changed = false;
    {
        std::lock_guard lk(m_Mutex);
        ScriptAsset* asset = nullptr;
        auto it = m_ByPath.find(key_ts);
        if (it != m_ByPath.end())
        {
            asset = it->second;
        }
        else
        {
            std::filesystem::path src_rel_tsx = std::filesystem::path("Scripts") / rel_to_interm;
            src_rel_tsx.replace_extension(".tsx");
            it = m_ByPath.find(normaliseInternal(src_rel_tsx));
            if (it != m_ByPath.end())
            {
                asset = it->second;
            }
            else
            {
                // Maybe it's a hand-authored .js (rare but supported by
                // enumerateScriptFiles).
                std::filesystem::path src_rel_js = std::filesystem::path("Scripts") / rel_to_interm;
                src_rel_js.replace_extension(".js");
                it = m_ByPath.find(normaliseInternal(src_rel_js));
                if (it != m_ByPath.end())
                    asset = it->second;
            }
        }

        if (asset != nullptr && asset->m_CompiledRelPath != compiled_rel_normalised)
        {
            asset->m_CompiledRelPath = compiled_rel_normalised;
            changed = true;
        }
    }
    if (changed)
    {
        ScheduleSave();
    }
}

void ScriptRegistry::OnCompiledJsDeleted(const std::filesystem::path& abs_js_path)
{
    if (m_IntermediateScriptsRoot.empty() || abs_js_path.empty())
        return;

    std::error_code ec;
    std::filesystem::path rel_to_interm = std::filesystem::relative(abs_js_path, m_IntermediateScriptsRoot, ec);
    if (ec || rel_to_interm.empty() || rel_to_interm.string().rfind("..", 0) == 0)
    {
        return;
    }

    // Same source-path search as onCompiledJsChanged.
    std::filesystem::path src_rel_ts = std::filesystem::path("Scripts") / rel_to_interm;
    src_rel_ts.replace_extension(".ts");
    eastl::string key_ts = normaliseInternal(src_rel_ts);

    bool changed = false;
    {
        std::lock_guard lk(m_Mutex);
        ScriptAsset* asset = nullptr;
        auto it = m_ByPath.find(key_ts);
        if (it != m_ByPath.end())
            asset = it->second;
        else
        {
            std::filesystem::path src_rel_tsx = std::filesystem::path("Scripts") / rel_to_interm;
            src_rel_tsx.replace_extension(".tsx");
            it = m_ByPath.find(normaliseInternal(src_rel_tsx));
            if (it != m_ByPath.end())
                asset = it->second;
        }

        if (asset != nullptr && !asset->m_CompiledRelPath.empty())
        {
            asset->m_CompiledRelPath.clear();
            changed = true;
        }
    }
    if (changed)
    {
        ScheduleSave();
    }
}

bool ScriptRegistry::LoadFromDisk()
{
    if (m_RegistryFile.empty())
    {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(m_RegistryFile, ec))
    {
        return false;
    }

    FILE* f = fopen(m_RegistryFile.string().c_str(), "rb");
    if (!f)
    {
        LOG_WARNING(ZScriptRegistry, "Cannot open registry file: {}", m_RegistryFile.generic_string());
        return false;
    }

    char readBuffer[65536];
    rapidjson::FileReadStream is(f, readBuffer, sizeof(readBuffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(f);
    if (doc.HasParseError() || !doc.IsObject())
    {
        LOG_WARNING(ZScriptRegistry,
                    "Registry JSON parse failed; will rebuild from scratch: {}",
                    m_RegistryFile.generic_string());
        return false;
    }

    auto entries_it = doc.FindMember("entries");
    if (entries_it == doc.MemberEnd() || !entries_it->value.IsArray())
    {
        return false;
    }

    std::lock_guard lk(m_Mutex);
    m_ByGuid.clear();
    m_ByPath.clear();

    for (auto& e : entries_it->value.GetArray())
    {
        if (!e.IsObject())
        {
            continue;
        }
        auto guid_it = e.FindMember("guid");
        auto path_it = e.FindMember("path");
        if (guid_it == e.MemberEnd() || !guid_it->value.IsString())
            continue;
        if (path_it == e.MemberEnd() || !path_it->value.IsString())
            continue;

        auto owned = std::make_unique<ScriptAsset>();
        owned->m_Guid = toEastl(guid_it->value.GetString());
        owned->m_SourceRelPath = normaliseInternal(std::filesystem::path(path_it->value.GetString()));

        auto class_it = e.FindMember("class");
        if (class_it != e.MemberEnd() && class_it->value.IsString())
        {
            owned->m_DefaultClassName = toEastl(class_it->value.GetString());
        }

        auto mtime_it = e.FindMember("mtime");
        if (mtime_it != e.MemberEnd() && mtime_it->value.IsInt64())
        {
            owned->m_SourceMtimeNs = mtime_it->value.GetInt64();
        }

        auto hash_it = e.FindMember("content_hash");
        if (hash_it != e.MemberEnd() && hash_it->value.IsString())
        {
            owned->m_SourceContentHash = toEastl(hash_it->value.GetString());
        }

        // Compiled .js mirror path (added in P3 wiring; missing for entries
        // written by an older registry version, in which case the next
        // rescan() Pass 4 backfills it).
        auto compiled_it = e.FindMember("compiled");
        if (compiled_it != e.MemberEnd() && compiled_it->value.IsString())
        {
            owned->m_CompiledRelPath = toEastl(compiled_it->value.GetString());
        }

        ScriptAsset* raw = owned.get();
        m_ByPath.emplace(owned->m_SourceRelPath, raw);
        m_ByGuid.emplace(owned->m_Guid, std::move(owned));
    }

    return true;
}

bool ScriptRegistry::SaveToDisk() const
{
    if (m_RegistryFile.empty())
    {
        return false;
    }

    try
    {
        std::filesystem::create_directories(m_RegistryFile.parent_path());

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();

        doc.AddMember("version", 1, alloc);

        rapidjson::Value entries(rapidjson::kArrayType);
        {
            std::lock_guard lk(m_Mutex);

            // Stable order (sorted by path) so VCS diffs are minimal.
            std::vector<ScriptAsset*> sorted;
            sorted.reserve(m_ByGuid.size());
            for (auto& kv : m_ByGuid)
            {
                sorted.push_back(kv.second.get());
            }
            std::sort(sorted.begin(), sorted.end(), [](ScriptAsset* a, ScriptAsset* b) {
                return a->m_SourceRelPath < b->m_SourceRelPath;
            });

            for (ScriptAsset* a : sorted)
            {
                rapidjson::Value entry(rapidjson::kObjectType);
                entry.AddMember("guid",
                                rapidjson::Value(a->m_Guid.c_str(),
                                                 static_cast<rapidjson::SizeType>(a->m_Guid.size()),
                                                 alloc),
                                alloc);
                entry.AddMember("path",
                                rapidjson::Value(a->m_SourceRelPath.c_str(),
                                                 static_cast<rapidjson::SizeType>(a->m_SourceRelPath.size()),
                                                 alloc),
                                alloc);
                entry.AddMember("class",
                                rapidjson::Value(a->m_DefaultClassName.c_str(),
                                                 static_cast<rapidjson::SizeType>(a->m_DefaultClassName.size()),
                                                 alloc),
                                alloc);
                entry.AddMember("mtime", static_cast<int64_t>(a->m_SourceMtimeNs), alloc);
                entry.AddMember("content_hash",
                                rapidjson::Value(a->m_SourceContentHash.c_str(),
                                                 static_cast<rapidjson::SizeType>(a->m_SourceContentHash.size()),
                                                 alloc),
                                alloc);
                entry.AddMember("compiled",
                                rapidjson::Value(a->m_CompiledRelPath.c_str(),
                                                 static_cast<rapidjson::SizeType>(a->m_CompiledRelPath.size()),
                                                 alloc),
                                alloc);
                entries.PushBack(entry, alloc);
            }
        }
        doc.AddMember("entries", entries, alloc);

        // Atomic write: temp + rename.
        std::filesystem::path temp = m_RegistryFile;
        temp += ".tmp";

        {
            FILE* f = fopen(temp.string().c_str(), "wb");
            if (!f)
            {
                LOG_ERROR(ZScriptRegistry,
                          "Cannot open registry tmp file for writing: {}",
                          temp.generic_string());
                return false;
            }
            rapidjson::StringBuffer sb;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
            doc.Accept(writer);
            fwrite(sb.GetString(), 1, sb.GetSize(), f);
            fclose(f);
        }
        std::error_code ec;
        std::filesystem::rename(temp, m_RegistryFile, ec);
        if (ec)
        {
            // rename can fail across volumes; fall back to copy+remove.
            std::filesystem::copy_file(temp, m_RegistryFile, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(temp);
            if (ec)
            {
                LOG_ERROR(ZScriptRegistry,
                          "Cannot finalise registry file: {} ({})",
                          m_RegistryFile.generic_string(),
                          ec.message());
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(ZScriptRegistry, "Save registry failed: {}", Encoding::GetExceptionMessage(e));
        return false;
    }
}

// =============================================================================
// Helpers
// =============================================================================

std::vector<eastl::string>
ScriptRegistry::EnumerateScriptFiles(const std::filesystem::path& project_root,
                                     const std::filesystem::path& scripts_root)
{
    std::vector<eastl::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(scripts_root, ec))
    {
        return out;
    }

    for (auto& entry : std::filesystem::recursive_directory_iterator(
             scripts_root, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        const std::filesystem::path& abs = entry.path();
        if (!hasSupportedScriptExtension(abs))
        {
            continue;
        }
        std::filesystem::path rel = std::filesystem::relative(abs, project_root, ec);
        if (ec || rel.empty())
        {
            ec.clear();
            continue;
        }
        out.push_back(normaliseInternal(rel));
    }
    return out;
}

eastl::string ScriptRegistry::DeterministicGuidFromPath(const eastl::string& rel_path)
{
    // FNV-1a applied twice with different seeds gives us 128 deterministic
    // bits. Different seeds avoid the trivial "both halves identical for any
    // input" pathology of using the same seed twice.
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(rel_path.c_str());
    size_t n = rel_path.size();
    uint64_t hi = fnv1a64(bytes, n, kFnvOffsetBasis);
    uint64_t lo = fnv1a64(bytes, n, hi ^ 0x9e3779b97f4a7c15ULL);  // splitmix-style mix-in
    return toHex64(hi) + toHex64(lo);
}

eastl::string ScriptRegistry::ParseDefaultClassName(const std::filesystem::path& abs_path)
{
    std::string head = readSmallTextFile(abs_path);
    if (head.empty())
    {
        return {};
    }
    // Match "export class X extends Y" or "export default class X extends Y".
    // Y must be Behaviour / Component / Script base; we accept any
    // identifier so that user-defined sub-bases work too. Phase 5 will
    // refine this to actually verify the base class via the TS type checker.
    static const std::regex re(R"(export\s+(?:default\s+)?class\s+([A-Za-z_]\w*)\s+extends\s+[A-Za-z_]\w*)",
                               std::regex::ECMAScript);
    std::smatch m;
    if (std::regex_search(head, m, re) && m.size() >= 2)
    {
        return toEastl(m[1].str());
    }
    return {};
}

int64_t ScriptRegistry::FileMTimeNs(const std::filesystem::path& abs_path)
{
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(abs_path, ec);
    if (ec)
    {
        return 0;
    }
    // file_time_type's epoch is implementation-defined (Windows uses
    // 1601-01-01, libstdc++ uses 1970-01-01) and can produce a NEGATIVE
    // duration on Windows when converted via duration_cast. We don't care:
    // we only ever compare two values produced by this same function, so
    // sign and absolute epoch are irrelevant; we just need a stable,
    // monotonically-changing integer per file. Hence no clock_cast - it
    // would force-bind to system_clock and pull in the platform-specific
    // tzdata that we don't need here.
    auto dur = ft.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count();
}

eastl::string ScriptRegistry::NormaliseRelPath(const std::filesystem::path& rel_path)
{
    return normaliseInternal(rel_path);
}
