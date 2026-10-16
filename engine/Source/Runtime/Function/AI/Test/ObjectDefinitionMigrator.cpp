// ObjectDefinitionMigrator -- converts legacy .object.json definitions into
// engine-authored .prefab assets.
//
// The legacy "object definition" is a component list, not a GameObject tree:
// each entry carries a "$typeName" and a "$context" holding that component's
// serialized fields. At runtime the scene stores only a Transform plus a
// m_DefinitionUrl pointing at one of these files, and the real components
// (mesh renderer, rigid body, ...) only exist here. Nothing expands that URL
// today, so a level loads as a set of bare transforms.
//
// This tool turns each definition into a real prefab: it produces each
// component through the runtime type system, fills it from the matching
// "$context" using the component's OWN Transfer() (so the on-disk encoding
// cannot drift from what the reader expects), and writes the result with
// PrefabUtility::SaveAsPrefabAsset -- the same path the editor uses.
//
// Components are dispatched by static type rather than through a runtime
// Type*: Object::Transfer is protected, so it can only be invoked on a
// statically-typed instance. An unmapped $typeName is reported and skipped,
// never guessed at.
//
// Usage:
//   ObjectDefinitionMigrator <engine/asset root> [--dry-run]

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/JsonSerialize/JSONRead.h"
#include "Runtime/Core/JsonSerialize/JSONUtility.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Component/Mesh/SkinMeshRenderer.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Framework/Component/Particle/ParticleComponent.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    using JsonDocument = rapidjson::Document;

    int g_failed = 0;
    int g_converted = 0;
    int g_skipped = 0;

    // Re-serialize one JSON value to text. JSONRead parses from a buffer and
    // has no entry point for "start at this child node", so the "$context"
    // subtree is re-emitted as its own document and handed over as a string.
    bool DumpValueToString(const rapidjson::Value& value, std::string& out_text)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        if (!value.Accept(writer))
        {
            return false;
        }
        out_text.assign(buffer.GetString(), buffer.GetSize());
        return true;
    }

    // Produce one component of a statically-known type and fill it from the
    // serialized "$context" subtree. Returns nullptr (and reports) on any
    // failure, so a broken definition never yields a hollow prefab silently.
    template<typename ComponentType>
    Component* ProduceAndFill(const std::string& context_text, const char* type_name, const std::string& where)
    {
        ComponentType* component =
            static_cast<ComponentType*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<ComponentType>(), 0));
        if (component == nullptr)
        {
            std::fprintf(stderr, "    FAIL  %s: cannot allocate %s\n", where.c_str(), type_name);
            ++g_failed;
            return nullptr;
        }

        JSONRead reader(context_text.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);
        JSONUtility::DeserializedFromJSONRead(reader, *component);
        return component;
    }

    // $typeName -> statically-typed component construction.
    Component* BuildComponent(const std::string& type_name, const std::string& context_text, const std::string& where)
    {
        if (type_name == "Transform" || type_name == "TransformComponent")
        {
            return ProduceAndFill<Transform>(context_text, type_name.c_str(), where);
        }
        if (type_name == "MeshRenderer")
        {
            return ProduceAndFill<MeshRenderer>(context_text, type_name.c_str(), where);
        }
        if (type_name == "SkinMeshRenderer")
        {
            return ProduceAndFill<SkinMeshRenderer>(context_text, type_name.c_str(), where);
        }
        if (type_name == "RigidBodyComponent")
        {
            return ProduceAndFill<RigidBodyComponent>(context_text, type_name.c_str(), where);
        }
        if (type_name == "AnimationComponent")
        {
            return ProduceAndFill<AnimationComponent>(context_text, type_name.c_str(), where);
        }
        if (type_name == "MotorComponent")
        {
            return ProduceAndFill<MotorComponent>(context_text, type_name.c_str(), where);
        }
        if (type_name == "ParticleComponent")
        {
            return ProduceAndFill<Runtime::ParticleComponent>(context_text, type_name.c_str(), where);
        }
        if (type_name == "CameraComponent")
        {
            return ProduceAndFill<CameraComponent>(context_text, type_name.c_str(), where);
        }
        if (type_name == "NavMeshVolumeComponent")
        {
            std::fprintf(stderr, "    skip  %s: %s belongs to the level, not to a definition\n",
                         where.c_str(), type_name.c_str());
            ++g_skipped;
            return nullptr;
        }

        std::fprintf(stderr, "    skip  %s: unmapped component '%s'\n", where.c_str(), type_name.c_str());
        ++g_skipped;
        return nullptr;
    }

    bool ConvertDefinition(const std::filesystem::path& source, const std::filesystem::path& target, bool dry_run)
    {
        const std::string where = source.filename().string();

        FILE* file = nullptr;
        const errno_t open_result = fopen_s(&file, source.string().c_str(), "rb");
        if (open_result != 0 || file == nullptr)
        {
            std::fprintf(stderr, "  FAIL  %s: cannot open\n", where.c_str());
            ++g_failed;
            return false;
        }
        std::string text;
        {
            fseek(file, 0, SEEK_END);
            const long size = ftell(file);
            fseek(file, 0, SEEK_SET);
            if (size > 0)
            {
                text.resize(static_cast<size_t>(size));
                const size_t read = fread(text.data(), 1, static_cast<size_t>(size), file);
                text.resize(read);
            }
        }
        fclose(file);

        JsonDocument document;
        if (document.Parse(text.c_str()).HasParseError())
        {
            std::fprintf(stderr, "  FAIL  %s: json parse error\n", where.c_str());
            ++g_failed;
            return false;
        }
        if (!document.IsObject() || !document.HasMember("components") || !document["components"].IsArray())
        {
            std::fprintf(stderr, "  FAIL  %s: no 'components' array\n", where.c_str());
            ++g_failed;
            return false;
        }

        GameObject* root = static_cast<GameObject*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<GameObject>(), 0));
        if (root == nullptr)
        {
            std::fprintf(stderr, "  FAIL  %s: cannot allocate GameObject\n", where.c_str());
            ++g_failed;
            return false;
        }
        root->SetName(source.stem().string().c_str());

        int component_count = 0;
        int transform_count = 0;
        for (const auto& entry : document["components"].GetArray())
        {
            if (!entry.IsObject() || !entry.HasMember("$typeName") || !entry["$typeName"].IsString())
            {
                std::fprintf(stderr, "    skip  %s: component without '$typeName'\n", where.c_str());
                ++g_skipped;
                continue;
            }

            const std::string type_name = entry["$typeName"].GetString();

            std::string context_text = "{}";
            if (entry.HasMember("$context") && entry["$context"].IsObject())
            {
                if (!DumpValueToString(entry["$context"], context_text))
                {
                    std::fprintf(stderr, "    FAIL  %s: cannot re-emit $context of %s\n",
                                 where.c_str(), type_name.c_str());
                    ++g_failed;
                    continue;
                }
            }

            Component* component = BuildComponent(type_name, context_text, where);
            if (component == nullptr)
            {
                continue;
            }
            if (std::strcmp(component->GetTypeName(), "Transform") == 0)
            {
                ++transform_count;
            }
            root->addComponent(component);
            ++component_count;
        }

        // An empty definition carries no components at all; that is a
        // placeholder, not a broken file, so skip it rather than fail.
        if (component_count == 0)
        {
            std::fprintf(stderr, "  skip  %s: no components\n", where.c_str());
            ++g_skipped;
            return true;
        }
        // A prefab root must carry a Transform (SaveAsPrefabAsset rejects one
        // that does not). Definitions that legitimately have no spatial
        // placement -- a bare particle emitter, for instance -- simply omit it,
        // so synthesise an identity Transform rather than failing the file.
        if (transform_count == 0)
        {
            Transform* root_transform =
                static_cast<Transform*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<Transform>(), 0));
            if (root_transform == nullptr)
            {
                std::fprintf(stderr, "  FAIL  %s: cannot allocate root Transform\n", where.c_str());
                ++g_failed;
                return false;
            }
            root->addComponent(root_transform);
            ++component_count;
            std::fprintf(stderr, "    note %s: no Transform in definition; added an identity one\n", where.c_str());
        }

        root->SyncSerializedComponents();

        if (dry_run)
        {
            std::fprintf(stderr, "  would convert %s -> %s (%d component(s))\n",
                         where.c_str(), target.filename().string().c_str(), component_count);
            ++g_converted;
            return true;
        }

        if (!PrefabUtility::SaveAsPrefabAsset(root, target))
        {
            std::fprintf(stderr, "  FAIL  %s: SaveAsPrefabAsset failed\n", where.c_str());
            ++g_failed;
            return false;
        }

        std::fprintf(stderr, "  ok    %s -> %s (%d component(s))\n",
                     where.c_str(), target.filename().string().c_str(), component_count);
        ++g_converted;
        return true;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: ObjectDefinitionMigrator <asset-root> [--dry-run]\n");
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

    std::vector<std::filesystem::path> definitions;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_root, ec))
    {
        if (ec || !entry.is_regular_file())
        {
            continue;
        }
        if (entry.path().filename().string().find(".object.json") != std::string::npos)
        {
            definitions.push_back(entry.path());
        }
    }

    std::fprintf(stderr, "found %zu object definition(s) under %s\n", definitions.size(), asset_root.string().c_str());
    if (dry_run)
    {
        std::fprintf(stderr, "--dry-run: no files will be written\n");
    }

    RegisterRuntime();
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);

    if (GET_SYSTEM(ObjectManager) == nullptr || GET_SYSTEM(AssetManager) == nullptr)
    {
        std::fprintf(stderr, "ObjectManager / AssetManager unavailable\n");
        return 77;
    }

    for (const std::filesystem::path& source : definitions)
    {
        // "wall.object.json" -> "wall.prefab": strip both trailing suffixes via
        // the stem rather than replace_extension, which only peels one.
        std::filesystem::path target = source.parent_path() / (source.stem().stem().string() + ".prefab");
        ConvertDefinition(source, target, dry_run);
    }

    std::fprintf(stderr, "\nconverted=%d skipped=%d failed=%d\n", g_converted, g_skipped, g_failed);
    return (g_failed == 0) ? 0 : 1;
}