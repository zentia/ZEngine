// =============================================================================
// AssetBundle smoke test (PR-PAK1).
//
// Headless: no window, no GPU, no project. It proves the thing that makes a
// player build possible -- an asset that EXISTS ONLY INSIDE A BUNDLE still
// loads through the ordinary AssetManager call, because
// SerializedFile::InitializeRead serves it from the bundle instead of the
// filesystem.
//
//   cmake -B build -DZENGINE_BUILD_ASSET_BUNDLE_SMOKE_TEST=ON
//   cmake --build build --target AssetBundleSmokeTest
//   ./build/.../AssetBundleSmokeTest
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/AssetBundle/AssetBundle.h"
#include "Runtime/AssetBundle/AssetBundleFormat.h"
#include "Runtime/AssetBundle/AssetBundleManager.h"
#include "Runtime/AssetBundle/AssetBundleManifest.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    // 32-char lowercase hex, the shape DeterministicGuidFromPath produces. The
    // runtime has no AssetRegistry to mint these, so a build stamps the GUID it
    // read out of each .zasset header into the bundle entry -- which is exactly
    // what makes a GUID-only external reference resolvable in a player.
    constexpr const char* kHeroGuid = "aaaabbbbccccddddeeeeffff00001111";
    constexpr const char* kWeaponGuid = "11112222333344445555666677778888";
    constexpr const char* kAlbedoGuid = "9999aaaabbbbccccddddeeeeffff0000";

    // A compiled module, in the shape tsc emits for `module: commonjs` (which
    // is what ScriptEnv's IIFE wrapper consumes).
    constexpr const char* kMoverSource = "exports.Mover = class Mover { speed() { return 3; } };\n";

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    // ObjectManager::Produce allocates with plain `new` (ProduceHelper), so the
    // matching release is `delete` -- NOT MemoryManager::DestroyObject, which
    // frees through mimalloc. On Apple the global operator new/delete is
    // deliberately NOT routed to mimalloc (see MemoryManager.cpp), so the two
    // disagree there and DestroyObject faults inside mi_usable_size. `delete`
    // is the correct pairing on every platform: off Apple `new` IS mimalloc.
    void DestroyProduced(Object* object) { delete object; }

    bool WriteAsset(const std::filesystem::path& path, const std::vector<int>& convert)
    {
        auto* object_manager = GET_SYSTEM(ObjectManager);
        auto* asset_manager = GET_SYSTEM(AssetManager);
        if (object_manager == nullptr || asset_manager == nullptr)
        {
            return false;
        }

        Object* produced = object_manager->Produce(TypeOf<AnimSkelMap>(), 0);
        if (produced == nullptr)
        {
            return false;
        }
        static_cast<AnimSkelMap*>(produced)->convert = convert;

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const bool ok = asset_manager->WriteObjectToDiskThreadSafe(path, *produced);
        DestroyProduced(produced);
        return ok;
    }

    // A cooked Texture2D, the asset the texture cook emits and the one
    // RenderResourceBase::LoadTexture looks for. Deliberately backend-neutral:
    // Texture2D holds no RHI state, so it can be authored and read headless.
    bool WriteTexture(const std::filesystem::path& path, uint32_t width, uint32_t height)
    {
        auto* object_manager = GET_SYSTEM(ObjectManager);
        auto* asset_manager = GET_SYSTEM(AssetManager);
        if (object_manager == nullptr || asset_manager == nullptr)
        {
            return false;
        }

        Object* produced = object_manager->Produce(TypeOf<Texture2D>(), 0);
        if (produced == nullptr)
        {
            return false;
        }

        auto* texture = static_cast<Texture2D*>(produced);
        texture->m_Width = width;
        texture->m_Height = height;
        texture->m_Format = 0;  // ordinal; only the GPU-side consumer reads it
        texture->m_Pixels.assign(static_cast<size_t>(width) * height * 4, 0xAB);
        texture->m_MipOffsets.push_back(0);

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const bool ok = asset_manager->WriteObjectToDiskThreadSafe(path, *texture);
        DestroyProduced(produced);
        return ok;
    }

    bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        in.seekg(0, std::ios::beg);
        out.resize(static_cast<size_t>(size));
        if (size > 0)
        {
            in.read(reinterpret_cast<char*>(out.data()), size);
        }
        return in.good();
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    RegisterCore();
    RegisterPlatform();
    REGISTER_SYSTEM(AssetBundleManager);
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: system bring-up failed\n");
        return 77;
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("zbundle_smoke_" + std::to_string(static_cast<unsigned>(std::hash<std::string> {}("z"))));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    const std::filesystem::path content_root = root / "Assets";
    const std::filesystem::path bundles_dir = root / "Build" / "Standalone" / "Bundles";

    // ---- B1: author two assets --------------------------------------------
    const std::filesystem::path hero_path = content_root / "Models" / "hero.zasset";
    const std::filesystem::path weapon_path = content_root / "Models" / "weapon.zasset";
    Check(WriteAsset(hero_path, {1, 2, 3}), "B1: hero.zasset written");
    Check(WriteAsset(weapon_path, {7, 8}), "B1: weapon.zasset written");

    const std::filesystem::path albedo_path = content_root / "Textures" / "albedo.zasset";
    Check(WriteTexture(albedo_path, 4, 4), "B1: albedo.zasset written");

    std::vector<uint8_t> hero_bytes;
    std::vector<uint8_t> weapon_bytes;
    std::vector<uint8_t> albedo_bytes;
    Check(ReadFileBytes(hero_path, hero_bytes) && !hero_bytes.empty(), "B1: hero bytes on disk");
    Check(ReadFileBytes(weapon_path, weapon_bytes), "B1: weapon bytes on disk");
    Check(ReadFileBytes(albedo_path, albedo_bytes) && !albedo_bytes.empty(), "B1: albedo bytes on disk");

    // ---- B2: pack them -----------------------------------------------------
    std::vector<AssetBundles::BundleSourceEntry> hero_entries;
    AssetBundles::BundleSourceEntry hero_entry;
    hero_entry.path = "Models/hero.zasset";  // registry-relative key
    hero_entry.guid = kHeroGuid;
    hero_entry.type = "AnimSkelMap";
    hero_entry.bytes = hero_bytes;
    hero_entries.push_back(std::move(hero_entry));

    std::vector<AssetBundles::BundleSourceEntry> shared_entries;
    AssetBundles::BundleSourceEntry weapon_entry;
    weapon_entry.path = "Models/weapon.zasset";
    weapon_entry.guid = kWeaponGuid;
    weapon_entry.type = "AnimSkelMap";
    weapon_entry.bytes = weapon_bytes;
    shared_entries.push_back(std::move(weapon_entry));

    AssetBundles::BundleSourceEntry albedo_entry;
    albedo_entry.path = "Textures/albedo.zasset";
    albedo_entry.guid = kAlbedoGuid;
    albedo_entry.type = "Texture2D";
    albedo_entry.bytes = albedo_bytes;
    shared_entries.push_back(std::move(albedo_entry));

    std::string error;

    // ---- B2b: a compiled script module -------------------------------------
    // Scripts are not registry assets: the loader addresses them by MODULE id
    // ("player/Mover") and the file lives under <project>/Intermediate/Scripts/,
    // outside the content directory the registry indexes. They ride along under
    // a reserved prefix so one entry table can carry both kinds of payload.
    const std::string mover_entry_path = AssetBundles::MakeScriptEntryPath("player/Mover");
    std::vector<AssetBundles::BundleSourceEntry> script_entries;
    AssetBundles::BundleSourceEntry script_entry;
    script_entry.path = mover_entry_path;
    script_entry.type = "ScriptModule";
    script_entry.bytes.assign(kMoverSource, kMoverSource + std::strlen(kMoverSource));
    script_entries.push_back(std::move(script_entry));

    const std::filesystem::path scripts_bundle_path = bundles_dir / "scripts.zbundle";
    Check(AssetBundles::WriteBundleFile(scripts_bundle_path, script_entries, AssetBundles::BundleCompression::Stored, error),
          "B2b: scripts bundle written");
    Check(mover_entry_path == "Intermediate/Scripts/player/Mover.js",
          "B2b: a module id maps to the reserved entry key");
    Check(AssetBundles::MakeScriptEntryPath("").empty(), "B2b: an empty module id maps to nothing");
    {
        std::string parsed;
        Check(AssetBundles::ParseScriptEntryPath(mover_entry_path, parsed) && parsed == "player/Mover",
              "B2b: the entry key parses back to the module id");
        Check(!AssetBundles::ParseScriptEntryPath("Models/hero.zasset", parsed),
              "B2b: an asset key is not a script entry");
    }

    const std::filesystem::path hero_bundle_path = bundles_dir / "models_hero.zbundle";
    const std::filesystem::path shared_bundle_path = bundles_dir / "shared.zbundle";
    Check(AssetBundles::WriteBundleFile(hero_bundle_path, hero_entries, AssetBundles::BundleCompression::Stored, error),
          "B2: hero bundle written");
    Check(AssetBundles::WriteBundleFile(shared_bundle_path, shared_entries, AssetBundles::BundleCompression::Stored, error),
          "B2: shared bundle written");

    AssetBundles::BundleFileHeader header;
    std::vector<AssetBundles::BundleEntry> entries;
    Check(AssetBundles::ReadBundleHeader(hero_bundle_path, header, entries, error), "B2: bundle header parses");
    Check(header.magic == AssetBundles::kBundleMagic && header.version == AssetBundles::kBundleVersion,
          "B2: magic + version");
    Check(entries.size() == 1 && entries[0].path == "Models/hero.zasset", "B2: entry table carries the registry key");

    // ---- B3: manifest (dependencies) --------------------------------------
    auto* object_manager = GET_SYSTEM(ObjectManager);
    Object* produced = object_manager->Produce(TypeOf<AssetBundleManifest>(), 0);
    auto* manifest = static_cast<AssetBundleManifest*>(produced);
    manifest->platform = "Standalone";
    manifest->content_root = content_root.generic_string().c_str();

    AssetBundleInfo hero_info;
    hero_info.name = "models_hero";
    hero_info.asset_paths.push_back("Models/hero.zasset");
    hero_info.dependencies.push_back("shared");
    // Scripts are nobody's dependency in the asset sense: a ScriptAsset names a
    // module id, and the .js is not part of the dependency walk. The builder
    // declares them explicitly so mounting one level is enough to run its
    // behaviours.
    hero_info.dependencies.push_back(AssetBundles::kScriptBundleName);
    AssetBundleInfo shared_info;
    shared_info.name = "shared";
    shared_info.asset_paths.push_back("Models/weapon.zasset");
    shared_info.asset_paths.push_back("Textures/albedo.zasset");
    AssetBundleInfo scripts_info;
    scripts_info.name = AssetBundles::kScriptBundleName;
    scripts_info.asset_paths.push_back(mover_entry_path.c_str());
    manifest->bundles.push_back(hero_info);
    manifest->bundles.push_back(shared_info);
    manifest->bundles.push_back(scripts_info);

    const std::filesystem::path manifest_path = root / "Build" / "Standalone" / "test.manifest.zasset";
    Check(GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(manifest_path, *manifest), "B3: manifest written");
    DestroyProduced(produced);

    // ---- B4: mount, dependencies auto-mount -------------------------------
    auto* bundles = GET_SYSTEM(AssetBundleManager);
    Check(bundles != nullptr, "B4: AssetBundleManager registered");
    Check(bundles->LoadManifest(manifest_path), "B4: manifest loads");
    Check(bundles->MountBundle(hero_bundle_path), "B4: hero bundle mounts");
    Check(bundles->IsMounted("shared"), "B4: dependency bundle auto-mounted");
    Check(bundles->IsMounted(AssetBundles::kScriptBundleName), "B4: the scripts dependency auto-mounts too");

    AssetBundle* hero_bundle = bundles->FindBundle("models_hero");
    Check(hero_bundle != nullptr && hero_bundle->GetEntryCount() == 1, "B4: mounted bundle has its entry");
    Check(bundles->ContainsAsset("Models/weapon.zasset"), "B4: shared asset is visible once mounted");

    // ---- B5: bytes out of the bundle match the cooked file ----------------
    std::string bytes_error;
    const std::vector<uint8_t>* served = hero_bundle->GetAssetBytes("Models/hero.zasset", bytes_error);
    Check(served != nullptr && *served == hero_bytes, "B5: served bytes are byte-identical to the source asset");
    Check(hero_bundle->GetAssetBytes("Models/nope.zasset", bytes_error) == nullptr, "B5: unknown asset is refused");

    // ---- B6: the point of the whole exercise ------------------------------
    // Delete the Assets/ tree: from here on the ONLY copy is inside a bundle.
    std::filesystem::remove_all(content_root, ec);
    Check(!std::filesystem::exists(hero_path, ec), "B6: source Assets/ tree deleted");

    auto* asset_manager = GET_SYSTEM(AssetManager);
    AnimSkelMap* hero = asset_manager->loadAsset<AnimSkelMap>(eastl::string(hero_path.generic_string().c_str()));
    Check(hero != nullptr, "B6: asset loads from the bundle after the file is gone");
    if (hero != nullptr)
    {
        Check(hero->convert.size() == 3 && hero->convert[0] == 1 && hero->convert[2] == 3,
              "B6: bundle-resident asset deserialises correctly");
    }

    AnimSkelMap* weapon = asset_manager->loadAsset<AnimSkelMap>(eastl::string(weapon_path.generic_string().c_str()));
    Check(weapon != nullptr && weapon->convert.size() == 2 && weapon->convert[1] == 8,
          "B6: asset from the dependency bundle loads too");

    // ---- B6b: GUID -> path, the runtime's stand-in for AssetRegistry -------
    // A player build ships no AssetRegistry, so the only GUID index available
    // is the one AssetBundleManager builds from the mounted bundles' entries.
    // Without it, an external PPtr whose FileIdentifier carries a GUID but no
    // usable path resolved to null -- which is what silently dropped every
    // nested prefab in a packaged build.
    {
        std::string resolved_path;
        Check(bundles->TryResolveGuid(kHeroGuid, resolved_path) && resolved_path == "Models/hero.zasset",
              "B6b: a mounted bundle entry GUID resolves to its registry path");

        std::filesystem::path via_asset_manager;
        Check(asset_manager->TryGetPathForGuid(kHeroGuid, via_asset_manager) &&
                  via_asset_manager.generic_string() == "Models/hero.zasset",
              "B6b: AssetManager::TryGetPathForGuid resolves through the bundles");

        std::string should_stay_empty;
        Check(!bundles->TryResolveGuid("00000000000000000000000000000000", should_stay_empty),
              "B6b: an unknown GUID is refused");
        Check(!asset_manager->TryGetPathForGuid("", via_asset_manager), "B6b: an empty GUID is refused");
    }

    // ---- B6c: a reference into an unmounted bundle mounts it on demand -----
    // FileIdentifier carries a GUID and a path but never a bundle name, so the
    // manifest's asset_paths list is the only thing that can answer "which
    // bundle owns this?" -- without it a player has to mount everything up
    // front or lose every reference that crosses a bundle boundary.
    {
        bundles->Unmount("shared");
        Check(!bundles->IsMounted("shared"), "B6c: dependency unmounted to set up the test");
        Check(bundles->EnsureBundleMountedForAsset("Models/weapon.zasset"),
              "B6c: the manifest names the owning bundle and it gets mounted");
        Check(bundles->IsMounted("shared"), "B6c: the owning bundle is mounted again");
        Check(!bundles->EnsureBundleMountedForAsset("Models/not-packed.zasset"),
              "B6c: an asset no bundle claims is refused");
    }

    // ---- B6d: a cooked texture survives a bundle-only build ----------------
    // RenderResourceBase::LoadTexture resolves a cooked Texture2D .zasset and
    // used to give up the moment it was not on disk -- which is every texture
    // in a packaged build, because a player ships neither Assets/ nor
    // Intermediate/Cooked/. The exact call it makes (ReadObject<Texture2D>)
    // must therefore succeed with only a bundle behind it, or every material
    // in a player renders untextured.
    {
        Check(!std::filesystem::exists(albedo_path, ec), "B6d: the cooked texture has no on-disk copy");
        std::filesystem::path texture_path = albedo_path;
        Texture2D* texture = asset_manager->ReadObject<Texture2D>(texture_path);
        Check(texture != nullptr && texture->IsValid(), "B6d: bundle-resident Texture2D loads through ReadObject");
        if (texture != nullptr)
        {
            Check(texture->m_Width == 4 && texture->m_Height == 4 && texture->m_Pixels.size() == 4 * 4 * 4,
                  "B6d: the texture payload round-trips");
        }
    }

    // ---- B9: a player finds the packaged content without being told -------
    // <Build>/<Platform>/ is named after the editor's BuildTarget enum, which
    // a player cannot see, so MountBuild locates the manifest instead of
    // reconstructing the path. This is the step a standalone player has been
    // missing: nothing mounted the bundles, so a packaged build ran against an
    // empty source tree.
    {
        std::filesystem::path found_bundles;
        std::filesystem::path found_manifest;
        Check(bundles->FindPackagedBuild(root / "Build", found_bundles, found_manifest),
              "B9: the packaged build is found under Build/");
        Check(found_bundles == bundles_dir && found_manifest == manifest_path,
              "B9: it reports Bundles/ and the manifest sitting next to it");
        Check(!bundles->FindPackagedBuild(root / "NotABuild", found_bundles, found_manifest),
              "B9: a tree with no packaged content reports none");

        bundles->UnmountAll();
        Check(bundles->MountBuild(root / "Build") == 3, "B9: MountBuild mounts every bundle");
        Check(bundles->IsMounted("models_hero") && bundles->IsMounted("shared") &&
                  bundles->IsMounted(AssetBundles::kScriptBundleName),
              "B9: the root bundle, its dependency and the scripts are mounted");
        Check(bundles->ContainsAsset("Textures/albedo.zasset"), "B9: packaged content is reachable again");
    }

    // ---- B10: compiled script modules come out of the bundle ---------------
    // Without this the only way to ship scripts is to copy
    // <project>/Intermediate/Scripts/ next to the player, so the script bytes
    // and the asset bytes could come from two different builds.
    {
        std::string source;
        Check(bundles->TryReadScriptModule("player/Mover", source) && source == kMoverSource,
              "B10: a bundled module is read back by module id");

        std::string missing;
        Check(!bundles->TryReadScriptModule("player/Missing", missing), "B10: an unknown module is refused");
        Check(!bundles->TryReadScriptModule("", missing) && missing.empty(), "B10: an empty module id is refused");

        // On demand: a scripts bundle is nobody's asset dependency, so it gets
        // mounted when the first module is asked for.
        bundles->Unmount(AssetBundles::kScriptBundleName);
        Check(!bundles->IsMounted(AssetBundles::kScriptBundleName), "B10: scripts unmounted to set up the test");
        std::string remounted;
        Check(bundles->TryReadScriptModule("player/Mover", remounted) && remounted == kMoverSource,
              "B10: asking for a module mounts the scripts bundle on demand");
        Check(bundles->IsMounted(AssetBundles::kScriptBundleName), "B10: the scripts bundle is mounted again");
    }

    // ---- B7: corruption is detected ---------------------------------------
    {
        std::vector<uint8_t> raw;
        ReadFileBytes(shared_bundle_path, raw);

        // Flip a byte inside the BLOB (not the header, whose reserved bytes are
        // deliberately not validated) so the CRC has to catch it.
        AssetBundles::BundleFileHeader shared_header;
        std::vector<AssetBundles::BundleEntry> shared_entries;
        if (AssetBundles::ReadBundleHeader(shared_bundle_path, shared_header, shared_entries, error))
        {
            const size_t target = static_cast<size_t>(shared_header.blob_offset) + 2;
            if (target < raw.size())
            {
                raw[target] ^= 0xFF;
            }

            std::ofstream out(shared_bundle_path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            out.close();

            bundles->Unmount("shared");
            AssetBundle* corrupt = MemoryManager::CreateObject<AssetBundle>();
            const bool parses = corrupt->LoadFromFile(shared_bundle_path, error);
            const std::vector<uint8_t>* blob = parses ? corrupt->GetAssetBytes("Models/weapon.zasset", error) : nullptr;
            Check(blob == nullptr, "B7: a flipped byte in a blob is caught by CRC");
            std::printf("       (crc error: %s)\n", error.c_str());
            MemoryManager::DestroyObject(corrupt);
        }
    }

    // ---- B8: unmount -------------------------------------------------------
    bundles->UnmountAll();
    Check(!bundles->IsMounted("models_hero"), "B8: unmount drops the bundle");
    std::vector<uint8_t> gone;
    Check(!bundles->TryReadAssetBytes(hero_path, gone), "B8: nothing is served after unmount");

    std::filesystem::remove_all(root, ec);

    std::printf("\nAssetBundle smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
