#include "Editor/Build/AssetBundleBuilder.h"

#include "Editor/AssetPipeline/TextureImporter/TextureImporter.h"
#include "Editor/Build/BuildCache.h"
#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Runtime/AssetBundle/AssetBundleFormat.h"
#include "Runtime/AssetBundle/AssetBundleManifest.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    // The asset types a player can be asked to open. Everything else is either
    // reached from one of these or is dead content that does not need shipping.
    // "PrefabAsset" has no "Res" suffix -- it predates the naming convention.
    const char* const kRootAssetTypes[] = {"LevelRes", "WorldRes", "PrefabAsset"};

    constexpr const char* kSharedBundleName = "shared";
    /// Next to the manifest, not in Bundles/: it describes the whole output
    /// directory, so a written cache always matches the artifacts beside it
    /// (delete Build/<Platform>/ and the next pass is a full one).
    constexpr const char* kCacheFileName = "AssetBundleBuildCache.json";
    /// Reserved key for the manifest's own fingerprint. A bundle name can never
    /// collide with it -- BundleNameFromRoot() strips anything but
    /// [A-Za-z0-9_-], and the leading "__" is not a legal bundle name anyway.
    constexpr const char* kManifestCacheKey = "__manifest__";
    /// Bump when this pass starts feeding something new into a fingerprint
    /// (a new entry field, a different writer, ...). Old caches are discarded.
    constexpr int kBuilderVersion = 1;

    // Layout is the one AGENTS.md 2.24 documents: bundles live in a Bundles/
    // subdirectory so the manifest sits next to a directory, not next to
    // content, and manifest-relative dependency names keep resolving when a
    // player mounts Bundles/ directly.
    constexpr const char* kBundlesSubDir = "Bundles";

    std::string NormaliseKey(const std::string& in_key)
    {
        std::string out;
        out.reserve(in_key.size());
        for (const char c : in_key)
        {
            out.push_back(c == '\\' ? '/' : c);
        }
        return out;
    }

    /// "level/1-1.scene" -> "level_1-1". The directory is kept because two
    /// levels in different folders are allowed to share a file name, and a
    /// bundle name is a file name (it becomes `<name>.zbundle`).
    std::string BundleNameFromRoot(const std::string& rel_key)
    {
        const std::filesystem::path rel(rel_key);
        const std::string stem = rel.stem().string();
        const std::string parent = rel.parent_path().string();

        std::string raw = parent.empty() ? stem : parent + "_" + stem;
        for (char& c : raw)
        {
            const bool keep = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
            if (!keep)
            {
                c = '_';
            }
        }
        return raw;
    }

    /// Every `*.js` under `root`, as (module id, absolute path), sorted by
    /// module id. The module id is the path relative to `root` without the
    /// `.js` suffix -- the same id `ScriptEnv::LoadModule()` takes, which is
    /// what `ScriptAsset::m_CompiledRelPath` stores.
    std::vector<std::pair<std::string, std::filesystem::path>> CollectScriptModules(
        const std::filesystem::path& root)
    {
        std::vector<std::pair<std::string, std::filesystem::path>> found;
        if (root.empty())
        {
            return found;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec) || ec)
        {
            return found;
        }

        for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            if (ec || !it->is_regular_file(ec) || ec)
            {
                continue;
            }
            // .js only: tsc also emits .js.map / .d.ts next to the output and
            // neither is ever loaded at runtime.
            if (it->path().extension() != ".js")
            {
                continue;
            }

            std::string module_id = it->path().lexically_relative(root).generic_string();
            const std::string kSuffix = ".js";
            if (module_id.size() > kSuffix.size() &&
                module_id.compare(module_id.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
            {
                module_id.erase(module_id.size() - kSuffix.size());
            }
            if (module_id.empty())
            {
                continue;
            }
            found.emplace_back(std::move(module_id), it->path());
        }

        std::sort(found.begin(), found.end(),
                  [](const std::pair<std::string, std::filesystem::path>& a,
                     const std::pair<std::string, std::filesystem::path>& b) { return a.first < b.first; });
        return found;
    }

    /// BFS one root over the registry's dependency edges. The root itself is
    /// part of its own result -- a level is an asset that has to be packed too.
    std::vector<std::string> WalkFrom(const AssetRegistry& registry, const std::string& root)
    {
        std::vector<std::string> reachable;
        std::unordered_set<std::string> visited;
        std::deque<std::string> pending;

        visited.insert(root);
        pending.push_back(root);

        while (!pending.empty())
        {
            const std::string current = pending.front();
            pending.pop_front();
            reachable.push_back(current);

            for (const std::string& raw_dependency : registry.GetDependencies(current))
            {
                const std::string dependency = NormaliseKey(raw_dependency);
                if (dependency.empty() || visited.count(dependency) != 0)
                {
                    continue;
                }
                visited.insert(dependency);
                pending.push_back(dependency);
            }
        }

        return reachable;
    }
}  // namespace

std::filesystem::path AssetBundleBuilder::DefaultOutputDir(TextureImporterSettings::BuildTarget target)
{
    const ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->project_path.empty())
    {
        return {};
    }

    return project_info->project_path / "Build" / TextureImporterSettings::BuildTargetDisplayName(target);
}

AssetBundleBuildResult AssetBundleBuilder::Build(const AssetBundleBuildOptions& options)
{
    AssetBundleBuildResult result;

    const ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
    if (project_info == nullptr || project_info->project_path.empty())
    {
        result.error = "no project is open";
        return result;
    }

    const std::filesystem::path content_root = project_info->GetProjectContent();
    std::error_code ec;
    if (content_root.empty() || !std::filesystem::exists(content_root, ec))
    {
        result.error = "project content directory does not exist: " + content_root.generic_string();
        return result;
    }

    // The whole dependency walk is AssetRegistry's job, and AssetRegistry only
    // exists behind the editor asset manager. A standalone player has no
    // registry, so a build can only be driven from the editor.
    auto* editor_asset_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
    if (editor_asset_mgr == nullptr)
    {
        result.error = "packaging needs the editor asset manager (AssetRegistry); a player build cannot package itself";
        return result;
    }

    AssetRegistry& registry = editor_asset_mgr->getAssetRegistry();
    if (registry.isScanning())
    {
        // Packing against a half-built index would silently omit assets, which
        // is the one failure mode that is invisible until runtime.
        LOG_INFO(ZPak, "AssetBundleBuilder: waiting for the asset registry scan to finish");
        registry.WaitForScanComplete();
    }

    const std::filesystem::path output_dir =
        options.output_dir.empty() ? DefaultOutputDir(options.target) : options.output_dir;
    if (output_dir.empty())
    {
        result.error = "cannot determine an output directory";
        return result;
    }

    const std::string platform_tag = TextureImporterSettings::BuildTargetDisplayName(options.target);

    // ---- 1. roots ----------------------------------------------------------
    std::vector<std::string> roots;
    for (const std::string& raw_root : options.roots)
    {
        const std::string root = NormaliseKey(raw_root);
        if (root.empty())
        {
            continue;
        }
        if (!registry.GetAssetIndex(root).has_value())
        {
            LOG_WARNING(ZPak, "AssetBundleBuilder: root '{}' is not in the asset registry; skipped", root);
            continue;
        }
        roots.push_back(root);
    }

    if (roots.empty())
    {
        std::unordered_set<std::string> discovered;
        for (const char* asset_type : kRootAssetTypes)
        {
            for (const std::string& raw : registry.GetAssetsByType(asset_type))
            {
                discovered.insert(NormaliseKey(raw));
            }
        }
        roots.assign(discovered.begin(), discovered.end());
        std::sort(roots.begin(), roots.end());
    }

    if (roots.empty())
    {
        result.error = "no root assets to package (no LevelRes / WorldRes / PrefabAsset in the registry)";
        return result;
    }

    LOG_INFO(ZPak, "AssetBundleBuilder: {} root(s), target {}", roots.size(), platform_tag);

    // ---- 2. cook -----------------------------------------------------------
    // Incremental too, and this is where the time actually goes: re-encoding a
    // BC7/ASTC texture dwarfs everything the packing pass does. `force_rebuild`
    // turns it back into the "cook everything, always" behaviour the cook menu
    // items still have.
    if (options.cook_first)
    {
        const int cooked = TextureImporter::CookProjectTextures(options.target, !options.force_rebuild);
        result.cooked_texture_count = cooked > 0 ? static_cast<std::size_t>(cooked) : 0;
        LOG_INFO(ZPak, "AssetBundleBuilder: cooked {} texture(s) for {}", cooked, platform_tag);
    }

    // ---- 3. dependency walk + partition ------------------------------------
    std::vector<std::vector<std::string>> per_root_reachable;
    per_root_reachable.reserve(roots.size());
    std::unordered_map<std::string, int> reach_count;

    for (const std::string& root : roots)
    {
        std::vector<std::string> reachable = WalkFrom(registry, root);
        for (const std::string& asset : reachable)
        {
            ++reach_count[asset];
        }
        per_root_reachable.push_back(std::move(reachable));
    }

    // An asset two roots reach must not be duplicated into both: the copy the
    // runtime happens to mount first would win, and the two would drift apart
    // on any later build. It goes into `shared` instead, which every root
    // bundle declares as a dependency.
    std::unordered_set<std::string> shared_assets;
    for (const auto& [asset, count] : reach_count)
    {
        if (count >= 2)
        {
            shared_assets.insert(asset);
        }
    }

    // ---- 4. collect inputs -------------------------------------------------
    // Nothing is read yet. A no-op build must be able to decide "unchanged"
    // from stat() + a cached hash alone, which is the whole point: reading
    // every asset is what made a rebuild cost the same as a first build.
    struct PlannedEntry
    {
        std::string path;              ///< key inside the bundle
        std::filesystem::path file;    ///< what to read: cooked bytes when there are any
        std::filesystem::path source;  ///< editor-platform fallback (see below)
        std::string guid;
        std::string type;
    };

    const std::filesystem::path cooked_root = project_info->GetIntermediateCookedRoot();
    const std::filesystem::path cooked_platform_root = cooked_root.empty() ? std::filesystem::path()
                                                                           : cooked_root / platform_tag;

    std::unordered_map<std::string, std::vector<PlannedEntry>> bundles;

    auto addAsset = [&](const std::string& bundle_name, const std::string& asset) {
        const std::filesystem::path source_path = content_root / asset;
        std::filesystem::path read_path = source_path;

        if (!cooked_platform_root.empty())
        {
            const std::filesystem::path cooked_path = cooked_platform_root / asset;
            std::error_code exists_ec;
            if (std::filesystem::exists(cooked_path, exists_ec))
            {
                read_path = cooked_path;
            }
        }

        std::error_code exists_ec;
        if (!std::filesystem::exists(read_path, exists_ec) || exists_ec)
        {
            LOG_WARNING(ZPak, "AssetBundleBuilder: cannot read asset {}; skipped", source_path.generic_string());
            return;
        }

        PlannedEntry entry;
        entry.path = asset;
        entry.file = std::move(read_path);
        // Kept even when `file` is the cooked copy: a cooked file that exists
        // but cannot be read (truncated by a killed cook) used to fall back to
        // the editor bytes, and shipping the wrong platform's texture is still
        // better than shipping no texture at all.
        entry.source = source_path;

        if (const std::optional<AssetIndexEntry> index = registry.GetAssetIndex(asset))
        {
            entry.guid = index->guid;
            entry.type = index->asset_type;
        }

        bundles[bundle_name].push_back(std::move(entry));
    };

    for (std::size_t root_index = 0; root_index < roots.size(); ++root_index)
    {
        const std::string bundle_name = BundleNameFromRoot(roots[root_index]);
        for (const std::string& asset : per_root_reachable[root_index])
        {
            if (shared_assets.count(asset) != 0)
            {
                continue;
            }
            addAsset(bundle_name, asset);
        }
    }

    for (const std::string& asset : shared_assets)
    {
        addAsset(kSharedBundleName, asset);
    }

    // ---- 4b. compiled scripts ---------------------------------------------
    // The .js a Behaviour runs is not reachable through the asset dependency
    // graph: a ScriptAsset names a MODULE ("foo/Bar"), and the file lives in
    // <project>/Intermediate/Scripts/, outside the content directory the
    // registry indexes. Without this pass the only way to ship scripts is to
    // copy that directory next to the player, so script and asset bytes could
    // come from two different builds.
    const std::vector<std::pair<std::string, std::filesystem::path>> script_modules =
        CollectScriptModules(project_info->GetIntermediateScriptsRoot());

    for (const auto& [module_id, absolute_path] : script_modules)
    {
        std::error_code exists_ec;
        if (!std::filesystem::exists(absolute_path, exists_ec) || exists_ec)
        {
            LOG_WARNING(ZPak, "AssetBundleBuilder: cannot read script module '{}'; skipped", module_id);
            continue;
        }

        PlannedEntry entry;
        entry.path = AssetBundles::MakeScriptEntryPath(module_id);
        // No GUID and no runtime type: a compiled module is not a registry
        // asset and not an Object. The prefix is what identifies it.
        entry.type = "ScriptModule";
        entry.file = absolute_path;

        bundles[AssetBundles::kScriptBundleName].push_back(std::move(entry));
    }

    result.script_count = bundles[AssetBundles::kScriptBundleName].size();
    if (result.script_count > 0)
    {
        LOG_INFO(ZPak, "AssetBundleBuilder: {} compiled script module(s)", result.script_count);
    }
    else
    {
        // Never leave an empty "scripts" bundle behind: it would be declared as
        // a dependency of every other bundle, and MountBundle treats a missing
        // dependency as a hard failure.
        bundles.erase(AssetBundles::kScriptBundleName);
    }

    // ---- 5. write (incrementally) ------------------------------------------
    // A bundle is rewritten only when its FINGERPRINT changed: the content hash
    // of every entry plus everything that decides how entries become bytes
    // (platform, compression switch, the bundle's dependency list). See
    // BuildCache.h for why a timestamp is only a fast path, never the verdict.
    const std::filesystem::path bundles_dir = output_dir / kBundlesSubDir;
    std::filesystem::create_directories(bundles_dir, ec);
    if (ec)
    {
        result.error = "cannot create output directory " + bundles_dir.generic_string() + ": " + ec.message();
        return result;
    }

    // The cache sits next to the manifest, not inside Bundles/: it describes
    // the whole output directory, so it can never outlive the artifacts it
    // vouchsafes. Wiping Build/<Platform>/ -- the first thing anyone tries
    // when a build looks wrong -- yields a clean full build.
    const std::filesystem::path cache_path = output_dir / kCacheFileName;
    BuildCache cache;
    std::string cache_error;
    if (!cache.Load(cache_path, cache_error) && !cache_error.empty())
    {
        LOG_WARNING(ZPak, "AssetBundleBuilder: build cache unusable ({}); rebuilding everything", cache_error);
    }

    // Anything that changes how inputs turn into bytes. One mismatch drops the
    // whole cache -- there is no partial invalidation to get wrong.
    BuildHasher env_hasher;
    env_hasher.Add(std::to_string(kBuilderVersion));
    env_hasher.Add(platform_tag);
    env_hasher.Add(options.compress ? "1" : "0");
    env_hasher.Add(content_root.generic_string());
    const std::string environment = env_hasher.Hex();
    if (options.force_rebuild || !cache.loaded() || cache.environment != environment)
    {
        cache.Reset();
        cache.environment = environment;
    }

    std::vector<AssetBundleInfo> infos;
    infos.reserve(bundles.size());
    std::unordered_map<std::string, std::string> fingerprints;
    const bool has_scripts = bundles.find(AssetBundles::kScriptBundleName) != bundles.end();

    // Sorted: the fingerprints below do not depend on iteration order, but the
    // log does, and a build whose "changed" set differs run to run is
    // impossible to reason about.
    std::vector<std::string> bundle_names;
    bundle_names.reserve(bundles.size());
    for (const auto& [name, entries] : bundles)
    {
        bundle_names.push_back(name);
    }
    std::sort(bundle_names.begin(), bundle_names.end());

    for (const std::string& name : bundle_names)
    {
        // Deterministic order: the same content must produce the same bundle
        // bytes, or a CI size regression check would flap on map iteration.
        std::vector<PlannedEntry> sorted = bundles[name];
        std::sort(sorted.begin(), sorted.end(),
                  [](const PlannedEntry& a, const PlannedEntry& b) { return a.path < b.path; });

        // Dependencies go INTO the fingerprint: a bundle that gains one (shared
        // content appeared, script modules arrived) has to be rewritten even
        // when none of its own bytes changed, because the dependency list is
        // part of what the manifest records for it.
        std::vector<std::string> dependencies;
        if (name != kSharedBundleName && !shared_assets.empty())
        {
            dependencies.emplace_back(kSharedBundleName);
        }
        // Scripts are nobody's dependency in the asset sense, so they are
        // declared explicitly: mounting one level has to be enough to run the
        // behaviours hanging off it. (EnsureBundleMountedForAsset would also
        // find them, but only after the first module is already being loaded.)
        if (name != AssetBundles::kScriptBundleName && has_scripts)
        {
            dependencies.emplace_back(AssetBundles::kScriptBundleName);
        }

        BuildHasher fp;
        fp.Add(std::to_string(kBuilderVersion));
        fp.Add(platform_tag);
        fp.Add(options.compress ? "1" : "0");
        for (const std::string& dependency : dependencies)
        {
            fp.Add(dependency);
        }

        bool inputs_readable = true;
        for (const PlannedEntry& entry : sorted)
        {
            fp.Add(entry.path);
            fp.Add(entry.guid);
            fp.Add(entry.type);

            bool hash_ok = false;
            const std::string hash = cache.ContentHash(name + "|" + entry.path, entry.file, hash_ok);
            if (!hash_ok)
            {
                // An input we cannot hash is an input we cannot reproduce; the
                // bundle must be rebuilt, and the pack pass below is what
                // reports the unreadable file.
                inputs_readable = false;
                continue;
            }
            fp.Add(hash);
        }
        const std::string fingerprint = fp.Hex();
        fingerprints[name] = fingerprint;

        const std::filesystem::path bundle_path = bundles_dir / (name + ".zbundle");
        const bool skip = inputs_readable && cache.IsUpToDate(name, fingerprint, bundle_path);

        if (skip)
        {
            ++result.skipped_bundle_count;
        }
        else
        {
            std::vector<AssetBundles::BundleSourceEntry> packed;
            packed.reserve(sorted.size());
            for (const PlannedEntry& planned : sorted)
            {
                std::vector<uint8_t> bytes;
                bool read = BuildReadWholeFile(planned.file, bytes);
                if (!read && !planned.source.empty() && planned.source != planned.file)
                {
                    read = BuildReadWholeFile(planned.source, bytes);
                }
                if (!read)
                {
                    LOG_WARNING(ZPak, "AssetBundleBuilder: cannot read asset {}; skipped",
                                planned.file.generic_string());
                    continue;
                }

                AssetBundles::BundleSourceEntry entry;
                entry.path = planned.path;
                entry.guid = planned.guid;
                entry.type = planned.type;
                entry.bytes = std::move(bytes);
                packed.push_back(std::move(entry));
            }

            // Per entry, not per bundle: WriteBundleFile keeps an entry Stored when
            // deflating it does not come out smaller, so block-compressed textures
            // cost nothing here.
            const AssetBundles::BundleCompression method =
                options.compress ? AssetBundles::BundleCompression::Zlib : AssetBundles::BundleCompression::Stored;
            std::string write_error;
            if (!AssetBundles::WriteBundleFile(bundle_path, packed, method, write_error))
            {
                result.error = "cannot write " + bundle_path.generic_string() + ": " + write_error;
                return result;
            }
            ++result.rebuilt_bundle_count;
        }

        int64_t size_bytes = 0;
        if (const auto recorded = cache.outputs.find(name); skip && recorded != cache.outputs.end())
        {
            size_bytes = recorded->second.size_bytes;
        }
        else
        {
            std::error_code size_ec;
            const std::uintmax_t size = std::filesystem::file_size(bundle_path, size_ec);
            size_bytes = size_ec ? 0 : static_cast<int64_t>(size);
            cache.Record(name, fingerprint, size_bytes);
        }

        result.total_bytes += static_cast<std::size_t>(size_bytes);
        // Script modules are counted separately: they are not part of the
        // dependency walk, so folding them into asset_count would break any
        // "did the walk find everything" comparison against the registry.
        if (name != AssetBundles::kScriptBundleName)
        {
            result.asset_count += sorted.size();
        }

        AssetBundleInfo info;
        info.name = name.c_str();
        info.size_bytes = size_bytes;
        for (const PlannedEntry& entry : sorted)
        {
            info.asset_paths.emplace_back(entry.path.c_str());
        }
        for (const std::string& dependency : dependencies)
        {
            info.dependencies.emplace_back(dependency.c_str());
        }

        infos.push_back(std::move(info));
    }

    // ---- 5b. drop artifacts nothing produces any more ----------------------
    // A level deleted since the last build would otherwise keep shipping: the
    // manifest no longer lists it, but the .zbundle is still on disk, and a
    // "just copy Build/ over again" deploy picks up whatever is there.
    std::vector<std::string> current_keys = bundle_names;
    current_keys.push_back(kManifestCacheKey);

    std::size_t stale_removed = 0;
    for (const std::string& key : cache.StaleKeys(current_keys))
    {
        std::error_code remove_ec;
        if (std::filesystem::remove(bundles_dir / (key + ".zbundle"), remove_ec))
        {
            ++stale_removed;
        }
        cache.Erase(key);
    }
    if (stale_removed > 0)
    {
        LOG_INFO(ZPak, "AssetBundleBuilder: removed {} stale bundle(s)", stale_removed);
        // The manifest listed them, so it has to be written again.
        cache.Erase(kManifestCacheKey);
    }

    // ---- 6. manifest -------------------------------------------------------
    std::string manifest_file_name = project_info->name.c_str();
    if (manifest_file_name.empty())
    {
        manifest_file_name = "AssetBundleManifest";
    }
    manifest_file_name += ".manifest.zasset";
    const std::filesystem::path manifest_path = output_dir / manifest_file_name;

    // The manifest is content-addressed too: it changes exactly when the set of
    // bundles or any bundle's fingerprint does, so a build that rewrote nothing
    // leaves it alone.
    BuildHasher manifest_hasher;
    manifest_hasher.Add(std::to_string(kBuilderVersion));
    manifest_hasher.Add(platform_tag);
    manifest_hasher.Add(content_root.generic_string());
    manifest_hasher.Add(manifest_file_name);
    for (const AssetBundleInfo& info : infos)
    {
        manifest_hasher.Add(info.name.c_str());
        if (const auto it = fingerprints.find(info.name.c_str()); it != fingerprints.end())
        {
            manifest_hasher.Add(it->second);
        }
    }
    const std::string manifest_fingerprint = manifest_hasher.Hex();

    if (!cache.IsUpToDate(kManifestCacheKey, manifest_fingerprint, manifest_path))
    {
        auto* object_manager = GET_SYSTEM(ObjectManager);
        if (object_manager == nullptr)
        {
            result.error = "ObjectManager is not available; cannot write the manifest";
            return result;
        }

        Object* produced = object_manager->Produce(TypeOf<AssetBundleManifest>(), 0);
        if (produced == nullptr)
        {
            result.error = "cannot produce an AssetBundleManifest";
            return result;
        }

        auto* manifest = static_cast<AssetBundleManifest*>(produced);
        manifest->platform = platform_tag.c_str();
        manifest->content_root = content_root.generic_string().c_str();
        manifest->bundles = infos;

        const bool manifest_ok = GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(manifest_path, *manifest);
        object_manager->Destroy(produced);

        if (!manifest_ok)
        {
            result.error = "cannot write the manifest to " + manifest_path.generic_string();
            return result;
        }

        std::error_code manifest_size_ec;
        const std::uintmax_t manifest_size = std::filesystem::file_size(manifest_path, manifest_size_ec);
        cache.Record(kManifestCacheKey, manifest_fingerprint,
                     manifest_size_ec ? 0 : static_cast<int64_t>(manifest_size));
    }

    // ---- 7. persist what this pass decided ---------------------------------
    std::string save_error;
    if (!cache.Save(cache_path, save_error))
    {
        // Not fatal: this build produced correct artifacts either way. All that
        // is lost is the next build's ability to skip work.
        LOG_WARNING(ZPak,
                    "AssetBundleBuilder: cannot save the build cache ({}); the next build will be a full one",
                    save_error);
    }

    result.succeeded = true;
    result.bundle_count = infos.size();
    result.output_dir = output_dir;

    LOG_INFO(ZPak,
             "AssetBundleBuilder: {} bundle(s) ({} rebuilt, {} skipped), {} asset(s), {} script(s), {} bytes -> {}",
             result.bundle_count,
             result.rebuilt_bundle_count,
             result.skipped_bundle_count,
             result.asset_count,
             result.script_count,
             result.total_bytes,
             output_dir.generic_string());
    return result;
}
