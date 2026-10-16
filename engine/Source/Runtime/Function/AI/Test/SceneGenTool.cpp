// Generates a .scene (YAML) level that Level::load() can actually consume,
// so the navigation mesh can be validated against the real engine load path.
//
// Why this exists: the legacy .level.json path deserializes through
// LevelRes::Transfer, where `m_Objects` is currently commented out, so JSON
// levels silently produce ZERO runtime objects. The .scene branch
// (ReadObjectsFromYaml) works, and is what the editor itself writes.
//
// This tool reads the authored 1-1.level.json for object placement and emits
// an equivalent .scene using the engine's own WriteObjectsToYaml, so the
// on-disk format cannot drift from what the reader expects.
//
// Bring-up mirrors YamlRoundTripSmokeTest's Y8 scenario: RegisterRuntime +
// the lightweight RuntimeAssetManager, no WorldManager / RHI / physics.

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Function/AI/Navigation/NavigationVolumeComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/ResType/Common/Level.h"

#include <EASTL/string.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    struct PlacedObject
    {
        std::string name;
        std::string definition;
        Vector3 position;
        Quaternion rotation;
        Vector3 scale {1.f, 1.f, 1.f};
    };

    // ---- authored level parsing -------------------------------------------
    //
    // The legacy .level.json is pretty-printed, so a hand-rolled character scan
    // easily latches onto a key name instead of a number. Use rapidjson (already
    // a runtime dependency, and what NavMeshTest uses) so the parse is exact.
    std::vector<PlacedObject> ParseLevelJson(const std::string& text)
    {
        std::vector<PlacedObject> out;

        rapidjson::Document document;
        document.Parse(text.c_str());
        if (document.HasParseError() || !document.IsObject())
        {
            std::fprintf(stderr, "level json is not a valid object\n");
            return out;
        }

        const auto objects_it = document.FindMember("objects");
        if (objects_it == document.MemberEnd() || !objects_it->value.IsArray())
        {
            std::fprintf(stderr, "level json has no objects array\n");
            return out;
        }

        for (const auto& object_value : objects_it->value.GetArray())
        {
            if (!object_value.IsObject())
            {
                continue;
            }

            PlacedObject placed;
            placed.position = Vector3::ZERO;

            const auto name_it = object_value.FindMember("name");
            if (name_it != object_value.MemberEnd() && name_it->value.IsString())
            {
                placed.name = name_it->value.GetString();
            }

            // The definition url is what binds an authored object to its
            // prefab (wall / floor / player / ...). Dropping it would emit a
            // scene full of unnamed, definition-less shells, so it must
            // round-trip even though the legacy JSON branch never read it.
            const auto definition_it = object_value.FindMember("definition");
            if (definition_it != object_value.MemberEnd() && definition_it->value.IsString())
            {
                placed.definition = definition_it->value.GetString();
            }

            const auto components_it = object_value.FindMember("instanced_components");
            if (components_it != object_value.MemberEnd() && components_it->value.IsArray())
            {
                for (const auto& component_value : components_it->value.GetArray())
                {
                    if (!component_value.IsObject())
                    {
                        continue;
                    }
                    const auto context_it = component_value.FindMember("$context");
                    if (context_it == component_value.MemberEnd() || !context_it->value.IsObject())
                    {
                        continue;
                    }
                    const auto transform_it = context_it->value.FindMember("transform");
                    if (transform_it == context_it->value.MemberEnd() || !transform_it->value.IsObject())
                    {
                        continue;
                    }
                    const auto& transform = transform_it->value;

                    const auto position_it = transform.FindMember("position");
                    if (position_it != transform.MemberEnd() && position_it->value.IsObject())
                    {
                        placed.position.x = position_it->value.HasMember("x") && position_it->value["x"].IsNumber()
                                                 ? position_it->value["x"].GetFloat() : 0.f;
                        placed.position.y = position_it->value.HasMember("y") && position_it->value["y"].IsNumber()
                                                 ? position_it->value["y"].GetFloat() : 0.f;
                        placed.position.z = position_it->value.HasMember("z") && position_it->value["z"].IsNumber()
                                                 ? position_it->value["z"].GetFloat() : 0.f;
                    }

                    // Rotation and scale round-trip too: the shipped level
                    // orients walls by yaw, and losing it would collapse the
                    // whole level onto the axis-aligned default.
                    const auto rotation_it = transform.FindMember("rotation");
                    if (rotation_it != transform.MemberEnd() && rotation_it->value.IsObject())
                    {
                        const auto& rotation = rotation_it->value;
                        placed.rotation.x = rotation.HasMember("x") && rotation["x"].IsNumber()
                                                ? rotation["x"].GetFloat() : 0.f;
                        placed.rotation.y = rotation.HasMember("y") && rotation["y"].IsNumber()
                                                ? rotation["y"].GetFloat() : 0.f;
                        placed.rotation.z = rotation.HasMember("z") && rotation["z"].IsNumber()
                                                ? rotation["z"].GetFloat() : 0.f;
                        placed.rotation.w = rotation.HasMember("w") && rotation["w"].IsNumber()
                                                ? rotation["w"].GetFloat() : 1.f;
                    }

                    const auto scale_it = transform.FindMember("scale");
                    if (scale_it != transform.MemberEnd() && scale_it->value.IsObject())
                    {
                        const auto& scale = scale_it->value;
                        placed.scale.x = scale.HasMember("x") && scale["x"].IsNumber() ? scale["x"].GetFloat() : 1.f;
                        placed.scale.y = scale.HasMember("y") && scale["y"].IsNumber() ? scale["y"].GetFloat() : 1.f;
                        placed.scale.z = scale.HasMember("z") && scale["z"].IsNumber() ? scale["z"].GetFloat() : 1.f;
                    }
                    break;
                }
            }

            out.push_back(placed);
        }

        return out;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: SceneGenTool <input.level.json> <output.scene>\n");
        return 2;
    }

    const std::filesystem::path input_path(argv[1]);
    const std::filesystem::path output_path(argv[2]);

    FILE* file = std::fopen(input_path.string().c_str(), "rb");
    if (file == nullptr)
    {
        std::fprintf(stderr, "cannot open %s\n", input_path.string().c_str());
        return 77;
    }
    std::string text;
    char buffer[4096];
    size_t read_count = 0;
    while ((read_count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        text.append(buffer, read_count);
    }
    std::fclose(file);

    // Pull every authored object placement out of the legacy level file.
    std::vector<PlacedObject> placed = ParseLevelJson(text);

    std::printf("parsed %zu objects\n", placed.size());

    // ---- bring up the minimal runtime ----------------------------------
    RegisterRuntime();
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);

    auto object_manager = GET_SYSTEM(ObjectManager);
    if (object_manager == nullptr)
    {
        std::fprintf(stderr, "ObjectManager unavailable\n");
        return 77;
    }

    // ---- build the object graph ----------------------------------------
    LevelRes* header = static_cast<LevelRes*>(object_manager->Produce(TypeOf<LevelRes>(), 0));
    object_manager->AllocateAndAssignInstanceID(header);
    header->m_Gravity = Vector3(0.f, 0.f, -9.8f);
    header->m_CharacterName = "Player";

    std::vector<Object*> graph_objects;
    std::vector<int64_t> graph_ids;
    graph_objects.push_back(header);
    graph_ids.push_back(1);
    int64_t next_file_id = 2;

    for (const PlacedObject& item : placed)
    {
        NavMeshVolumeComponent* volume_component = nullptr;
        GameObject* game_object = static_cast<GameObject*>(object_manager->Produce(TypeOf<GameObject>(), 0));
        Transform* transform = static_cast<Transform*>(object_manager->Produce(TypeOf<Transform>(), 0));
        object_manager->AllocateAndAssignInstanceID(game_object);
        object_manager->AllocateAndAssignInstanceID(transform);
        game_object->SetName(item.name.c_str());
        game_object->setDefinitionUrl(item.definition.c_str());
        transform->SetLocalPosition(Vector3d(item.position));
        transform->SetLocalRotation(item.rotation);
        transform->SetLocalScale(item.scale);
        game_object->addComponent(transform);

        // The authored level carries a bare "NavVolume" object whose
        // instanced_components only lists a Transform, so the volume component
        // itself has to be attached here. Doing it in the same loop keeps a
        // single NavVolume in the output (the JSON entry supplies its name and
        // placement; we only add the missing component).
        if (item.name == "NavVolume")
        {
            NavMeshVolumeComponent* volume =
                static_cast<NavMeshVolumeComponent*>(object_manager->Produce(TypeOf<NavMeshVolumeComponent>(), 0));
            object_manager->AllocateAndAssignInstanceID(volume);
            volume->SetBuildOnLoad(true);
            game_object->addComponent(volume);
            volume_component = volume;
        }

        game_object->SyncSerializedComponents();

        graph_objects.push_back(game_object);
        graph_ids.push_back(next_file_id++);
        graph_objects.push_back(transform);
        graph_ids.push_back(next_file_id++);
        if (volume_component != nullptr)
        {
            graph_objects.push_back(volume_component);
            graph_ids.push_back(next_file_id++);
        }
    }

    // The navigation volume. The legacy level already carries one (mounted
    // when the volume was first authored), so only append if it is missing --
    // otherwise the scene would end up with two volumes and build twice.
    const bool has_volume =
        std::any_of(placed.begin(), placed.end(),
                    [](const PlacedObject& item) { return item.name == "NavVolume"; });

    if (!has_volume)
    {
        GameObject* volume_object = static_cast<GameObject*>(object_manager->Produce(TypeOf<GameObject>(), 0));
        Transform* volume_transform = static_cast<Transform*>(object_manager->Produce(TypeOf<Transform>(), 0));
        NavMeshVolumeComponent* volume =
            static_cast<NavMeshVolumeComponent*>(object_manager->Produce(TypeOf<NavMeshVolumeComponent>(), 0));
        object_manager->AllocateAndAssignInstanceID(volume_object);
        object_manager->AllocateAndAssignInstanceID(volume_transform);
        object_manager->AllocateAndAssignInstanceID(volume);
        volume_object->SetName("NavVolume");
        volume->SetBuildOnLoad(true);
        volume_object->addComponent(volume_transform);
        volume_object->addComponent(volume);
        volume_object->SyncSerializedComponents();

        graph_objects.push_back(volume_object);
        graph_ids.push_back(next_file_id++);
        graph_objects.push_back(volume_transform);
        graph_ids.push_back(next_file_id++);
        graph_objects.push_back(volume);
        graph_ids.push_back(next_file_id++);
    }

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    auto asset_manager = GET_SYSTEM(AssetManager);
    const bool written =
        asset_manager != nullptr &&
        asset_manager->WriteObjectsToYaml(output_path, graph_objects.data(), graph_ids.data(),
                                          graph_objects.size());
    if (!written)
    {
        std::fprintf(stderr, "WriteObjectsToYaml failed\n");
        return 1;
    }

    std::printf("wrote %s (%zu graph objects)\n", output_path.string().c_str(), graph_objects.size());
    return 0;
}