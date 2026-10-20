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
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"

#include <cstdio>
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
        MemoryManager::DestroyObject(produced);
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

    std::vector<uint8_t> hero_bytes;
    std::vector<uint8_t> weapon_bytes;
    Check(ReadFileBytes(hero_path, hero_bytes) && !hero_bytes.empty(), "B1: hero bytes on disk");
    Check(ReadFileBytes(weapon_path, weapon_bytes), "B1: weapon bytes on disk");

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

    std::string error;
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
    AssetBundleInfo shared_info;
    shared_info.name = "shared";
    shared_info.asset_paths.push_back("Models/weapon.zasset");
    manifest->bundles.push_back(hero_info);
    manifest->bundles.push_back(shared_info);

    const std::filesystem::path manifest_path = root / "Build" / "Standalone" / "test.manifest.zasset";
    Check(GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(manifest_path, *manifest), "B3: manifest written");
    MemoryManager::DestroyObject(produced);

    // ---- B4: mount, dependencies auto-mount -------------------------------
    auto* bundles = GET_SYSTEM(AssetBundleManager);
    Check(bundles != nullptr, "B4: AssetBundleManager registered");
    Check(bundles->LoadManifest(manifest_path), "B4: manifest loads");
    Check(bundles->MountBundle(hero_bundle_path), "B4: hero bundle mounts");
    Check(bundles->IsMounted("shared"), "B4: dependency bundle auto-mounted");

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
