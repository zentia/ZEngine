// AssetMigrator -- one-shot converter that retires the legacy JSON asset
// format in favour of the two supported backends: binary .zasset and YAML
// authoring assets (.mat / .prefab / .scene).
//
// Why this exists: JSON cannot carry the (m_FileID, m_PathID) pairs that
// PPtr<T> / ImmediatePtr<T> serialize into, so any asset that participates in
// an object graph silently loses its cross-references when round-tripped
// through JSON. Unity has exactly two on-disk representations for the same
// reason. See doc/asset_management/PPTR_DESIGN.md.
//
// The conversion is lossless by construction: we deserialize with the SAME
// Transfer() the runtime uses and re-serialize through the engine's own
// writer, so the on-disk encoding cannot drift from what the reader expects.
//
// Usage:
//   AssetMigrator <engine/asset root> [--dry-run]
//
// The asset type is derived from the file's double extension (the legacy
// naming convention: foo.mesh.json, bar.material.json, ...), mirroring the
// mapping AssetRegistry and the loadAsset<T> call sites already imply.

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/File/FileSystem.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/ResType/Common/World.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/BoneBlendMask.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"
#include "Runtime/Resource/ResType/Global/GlobalParticle.h"
#include "Runtime/Resource/ResType/Global/GlobalRendering.h"

#include <EASTL/string.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    // Legacy double-extension -> runtime class name. Files whose extension is
    // not listed here are reported and skipped rather than guessed at: a wrong
    // type would produce a .zasset that deserializes into garbage.
    struct TypeMapping
    {
        const char* extension;
        const char* class_name;
    };

    // clang-format off
    const TypeMapping kTypeMappings[] = {
        {".material.json",       "Material"},
        {".mesh.json",           "MeshData"},
        {".skeleton.json",       "SkeletonData"},
        {".animation_clip.json", "AnimationAsset"},
        {".skeleton_map.json",   "AnimSkelMap"},
        {".skeleton_mask.json",  "BoneBlendMask"},
        {".world.json",          "WorldRes"},
        {".global.json",         nullptr},  // disambiguated by filename below
    };
    // clang-format on

    // Global config assets share one extension; the file stem names the type.
    const char* GlobalClassNameForStem(const std::string& stem)
    {
        if (stem == "particle")
        {
            return "GlobalParticleRes";
        }
        if (stem == "rendering")
        {
            return "GlobalRenderingRes";
        }
        return nullptr;
    }

    std::string LowerExtensionOf(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        for (char& character : extension)
        {
            if (character >= 'A' && character <= 'Z')
            {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return extension;
    }

    // The legacy convention is a compound suffix: "<name>.<kind>.json".
    std::string CompoundSuffix(const std::filesystem::path& path)
    {
        const std::string filename = path.filename().string();
        const size_t first_dot = filename.find('.');
        if (first_dot == std::string::npos)
        {
            return {};
        }
        return filename.substr(first_dot);
    }

    const char* ClassNameForPath(const std::filesystem::path& path)
    {
        const std::string suffix = CompoundSuffix(path);
        for (const TypeMapping& mapping : kTypeMappings)
        {
            if (suffix != mapping.extension)
            {
                continue;
            }
            if (mapping.class_name != nullptr)
            {
                return mapping.class_name;
            }
            // .global.json: disambiguate by the leading stem component.
            const std::string filename = path.filename().string();
            const std::string stem = filename.substr(0, filename.find('.'));
            return GlobalClassNameForStem(stem);
        }
        return nullptr;
    }
    // Produce + fill + write for one concrete asset type. The template is
    // required rather than a runtime Type* : Object::Transfer is protected,
    // so it can only be invoked on a statically-typed instance.
    template<typename AssetType>
    bool ConvertOne(const std::filesystem::path& source, const std::filesystem::path& target, const char* class_name)
    {
        AssetType* asset = MemoryManager::CreateObject<AssetType>();
        if (asset == nullptr)
        {
            std::fprintf(stderr, "  FAIL  %s (cannot allocate %s)\n", source.filename().string().c_str(), class_name);
            return false;
        }

        if (!GET_SYSTEM(FileSystem)->LoadAssetByJson(asset, source))
        {
            std::fprintf(stderr, "  FAIL  %s (json deserialize failed)\n", source.filename().string().c_str());
            return false;
        }

        if (!GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(target, *asset))
        {
            std::fprintf(stderr, "  FAIL  %s (binary write failed)\n", source.filename().string().c_str());
            return false;
        }

        std::fprintf(stderr, "  ok    %s -> %s\n", source.filename().string().c_str(), target.filename().string().c_str());
        return true;
    }

    // Runtime class name -> the Produce/JSON-fill/binary-write pipeline above.
    // Kept as an explicit table because the mapping is part of the migration
    // contract: an unmapped type is reported, never guessed at.
    bool ConvertByClassName(const char* class_name,
                            const std::filesystem::path& source,
                            const std::filesystem::path& target)
    {
        const std::string name(class_name);
        if (name == "Material")            { return ConvertOne<Material>(source, target, class_name); }
        if (name == "MeshData")            { return ConvertOne<MeshData>(source, target, class_name); }
        if (name == "SkeletonData")        { return ConvertOne<SkeletonData>(source, target, class_name); }
        if (name == "AnimationAsset")      { return ConvertOne<AnimationAsset>(source, target, class_name); }
        if (name == "AnimSkelMap")         { return ConvertOne<AnimSkelMap>(source, target, class_name); }
        if (name == "BoneBlendMask")       { return ConvertOne<BoneBlendMask>(source, target, class_name); }
        if (name == "WorldRes")            { return ConvertOne<WorldRes>(source, target, class_name); }
        if (name == "GlobalParticleRes")   { return ConvertOne<GlobalParticleRes>(source, target, class_name); }
        if (name == "GlobalRenderingRes")  { return ConvertOne<GlobalRenderingRes>(source, target, class_name); }
        return false;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: AssetMigrator <asset-root> [--dry-run]\n");
        return 2;
    }

    const std::filesystem::path asset_root(argv[1]);
    const bool dry_run = (argc >= 3) && (std::string(argv[2]) == "--dry-run");

    std::error_code ec;
    if (!std::filesystem::is_directory(asset_root, ec))
    {
        std::fprintf(stderr, "not a directory: %s\n", asset_root.string().c_str());
        return 2;
    }

    // Collect first so the walk isn't invalidated by the files we write.
    std::vector<std::filesystem::path> json_assets;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_root, ec))
    {
        if (ec || !entry.is_regular_file())
        {
            continue;
        }
        if (LowerExtensionOf(entry.path()) == ".json")
        {
            json_assets.push_back(entry.path());
        }
    }

    std::fprintf(stderr, "found %zu json assets under %s\n", json_assets.size(), asset_root.string().c_str());
    if (dry_run)
    {
        std::fprintf(stderr, "--dry-run: no files will be written\n");
    }

    RegisterRuntime();
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);

    auto asset_manager = GET_SYSTEM(AssetManager);
    auto object_manager = GET_SYSTEM(ObjectManager);
    if (asset_manager == nullptr || object_manager == nullptr)
    {
        std::fprintf(stderr, "AssetManager / ObjectManager unavailable\n");
        return 77;
    }

    int converted = 0;
    int skipped = 0;
    int failed = 0;

    for (const std::filesystem::path& source : json_assets)
    {
        const char* class_name = ClassNameForPath(source);
        if (class_name == nullptr)
        {
            std::fprintf(stderr, "  skip  %s (no type mapping)\n", source.filename().string().c_str());
            ++skipped;
            continue;
        }

        if (dry_run)
        {
            std::fprintf(stderr, "  would convert %s -> %s.zasset\n", source.filename().string().c_str(), class_name);
            ++converted;
            continue;
        }

        std::filesystem::path target = source;
        target.replace_extension(".zasset");

        if (ConvertByClassName(class_name, source, target))
        {
            ++converted;
        }
        else
        {
            ++failed;
        }
    }

    std::fprintf(stderr, "\nconverted=%d skipped=%d failed=%d\n", converted, skipped, failed);
    return (failed == 0) ? 0 : 1;
}
