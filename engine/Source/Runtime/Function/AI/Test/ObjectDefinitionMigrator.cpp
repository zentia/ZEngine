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
#include "Runtime/BaseClasses/PPtr.h"
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
#include "Runtime/Resource/ResType/Components/Camera.h"
#include "Runtime/Resource/ResType/Components/Motor.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

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

    // Canonical LocalIdentifierInFile for a single-object .zasset. Matches
    // what AssetManager::ReadObject / WriteObjectToDiskThreadSafe use, so an
    // InstanceID minted here resolves against the file both ways.
    constexpr int64_t kSingletonLocalIdentifier = 1;

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

    // Pull the "$context" subtree out of an inline polymorphic value and
    // re-emit it as standalone text, ready for JSONRead. Returns an empty
    // string (and reports) when the value has no "$context" -- an inline
    // object with no payload carries no fields to restore.
    std::string DumpContextText(const rapidjson::Value& inline_value, const std::string& where)
    {
        const auto context_it = inline_value.FindMember("$context");
        if (context_it == inline_value.MemberEnd() || !context_it->value.IsObject())
        {
            std::fprintf(stderr, "    note %s: inline object has no $context; nothing to restore\n", where.c_str());
            return {};
        }

        std::string out_text;
        DumpValueToString(context_it->value, out_text);
        return out_text;
    }

    // Walk a chain of object members ("a" -> "b" -> "c") and return the leaf,
    // or nullptr if any link is missing or not an object. Returns a pointer
    // into the source document; not owned by the caller.
    const rapidjson::Value* FindMemberPath(const rapidjson::Value& root,
                                           const char* first_key,
                                           const char* second_key,
                                           const char* third_key)
    {
        const rapidjson::Value* current = &root;
        for (const char* key : {first_key, second_key, third_key})
        {
            if (key == nullptr || current == nullptr || !current->IsObject())
            {
                return nullptr;
            }
            const auto it = current->FindMember(key);
            if (it == current->MemberEnd())
            {
                return nullptr;
            }
            current = &it->value;
        }
        return current;
    }

    // Produce one object of a statically-known type and fill it from a
    // serialized "$context" subtree. Returns nullptr (and reports) on any
    // failure, so a broken definition never yields a hollow object silently.
    template<typename ObjectType>
    Object* ProduceObjectAndFill(const std::string& context_text,
                                 const char* type_name,
                                 const std::string& where,
                                 int32_t instance_id = 0)
    {
        // Passing the id at Produce time registers the object under it
        // immediately; assigning afterwards would leave the table keyed on the
        // id Produce picked, so a later PPtr lookup would miss.
        ObjectType* object =
            static_cast<ObjectType*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<ObjectType>(), instance_id));
        if (object == nullptr)
        {
            std::fprintf(stderr, "    FAIL  %s: cannot allocate %s\n", where.c_str(), type_name);
            ++g_failed;
            return nullptr;
        }

        JSONRead reader(context_text.c_str(), TransferInstructionFlags::kNoTransferInstructionFlags);
        JSONUtility::DeserializedFromJSONRead(reader, *object);
        return object;
    }

    // Legacy JSON stores polymorphic sub-objects INLINE
    // ("$typeName" + "$context"), but the runtime declares those fields as
    // PPtr<T> -- a (fileID, pathID) reference. PPtr::Transfer never
    // instantiates anything on read, so every inline sub-object used to
    // deserialize as a null reference: walls lost their collision shapes,
    // the player lost its controller config and camera parameter.
    //
    // ReifyInlineObject resolves the mismatch the way the rest of the engine
    // expects: the inline value becomes a real .zasset on disk, and the PPtr
    // points at it. The instance ID is minted from the asset path, which is
    // exactly what AssetManager::ReadObject and the PPtr writer hook key on,
    // so the reference survives both the write and a later load.
    //
    // Returns 0 when the inline value is absent or not a recognised subtype.
    // A caller that gets 0 should leave the PPtr null rather than guess.
    template<typename ObjectType>
    int32_t ReifyInlineObject(const rapidjson::Value* inline_value,
                              const std::filesystem::path& asset_path,
                              const char* type_name,
                              const std::string& where)
    {
        if (inline_value == nullptr || !inline_value->IsObject())
        {
            return 0;
        }

        // Path-derived ID: must be minted BEFORE the object is produced so
        // the in-memory identity and the file's on-disk identity agree, which
        // is what AssetManager::ReadObject and the PPtr writer hook key on.
        const int32_t instance_id =
            GET_SYSTEM(AssetManager)->GetInstanceIDFromPathAndFileID(asset_path, kSingletonLocalIdentifier);

        Object* object = ProduceObjectAndFill<ObjectType>(
            DumpContextText(*inline_value, where), type_name, where, instance_id);
        if (object == nullptr)
        {
            return 0;
        }

        if (!GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(asset_path, *object))
        {
            std::fprintf(stderr, "    FAIL  %s: cannot write %s\n", where.c_str(), asset_path.generic_string().c_str());
            ++g_failed;
            return 0;
        }

        std::fprintf(stderr, "    wrote %s -> %s\n", type_name, asset_path.generic_string().c_str());
        return instance_id;
    }

    // $typeName -> statically-typed inline reification. Only the subtypes that
    // actually appear in the legacy corpus are listed; anything else is
    // reported rather than guessed at.
    int32_t ReifyInlineObjectByType(const rapidjson::Value* inline_value,
                                    const std::filesystem::path& asset_path,
                                    const std::string& type_name,
                                    const std::string& where)
    {
        if (type_name == "Box")
        {
            return ReifyInlineObject<Box>(inline_value, asset_path, "Box", where);
        }
        if (type_name == "Sphere")
        {
            return ReifyInlineObject<Sphere>(inline_value, asset_path, "Sphere", where);
        }
        if (type_name == "Capsule")
        {
            return ReifyInlineObject<Capsule>(inline_value, asset_path, "Capsule", where);
        }
        if (type_name == "PhysicsControllerConfig")
        {
            return ReifyInlineObject<PhysicsControllerConfig>(
                inline_value, asset_path, "PhysicsControllerConfig", where);
        }
        if (type_name == "ThirdPersonCameraParameter")
        {
            return ReifyInlineObject<ThirdPersonCameraParameter>(
                inline_value, asset_path, "ThirdPersonCameraParameter", where);
        }
        if (type_name == "FirstPersonCameraParameter")
        {
            return ReifyInlineObject<FirstPersonCameraParameter>(
                inline_value, asset_path, "FirstPersonCameraParameter", where);
        }
        if (type_name == "FreeCameraParameter")
        {
            return ReifyInlineObject<FreeCameraParameter>(
                inline_value, asset_path, "FreeCameraParameter", where);
        }

        std::fprintf(stderr, "    skip  %s: inline subtype '%s' is not mapped; left null\n",
                     where.c_str(), type_name.c_str());
        return 0;
    }

    // Restore the polymorphic sub-objects that legacy JSON stores inline but
    // the runtime holds by PPtr. Each one is written out as its own .zasset
    // next to the definition, and the PPtr is pointed at it.
    //
    // Paths are derived from the definition's own location so a re-run is
    // idempotent (same content -> same path -> same instance ID).
    void RestoreInlineReferences(Component* component,
                                 const std::string& type_name,
                                 const rapidjson::Value& component_entry,
                                 const std::filesystem::path& source,
                                 const std::string& where)
    {
        // Geometry assets live in a sibling "geometry" folder so they read as
        // part of the object rather than as stray files next to the prefab.
        const std::filesystem::path geometry_dir = source.parent_path() / "geometry";
        const std::string stem = source.stem().string();

        if (type_name == "RigidBodyComponent")
        {
            RigidBodyComponent* rigidbody = static_cast<RigidBodyComponent*>(component);
            RigidBodyComponentRes& res = rigidbody->GetRigidbodyResMutable();

            const auto context_it = component_entry.FindMember("$context");
            if (context_it == component_entry.MemberEnd())
            {
                return;
            }
            const auto res_it = context_it->value.FindMember("rigidbody_res");
            if (res_it == context_it->value.MemberEnd())
            {
                return;
            }
            const auto shapes_it = res_it->value.FindMember("shapes");
            if (shapes_it == res_it->value.MemberEnd() || !shapes_it->value.IsArray())
            {
                return;
            }

            // Index-aligned: the shapes array in the JSON is the same array
            // the Res holds, so shape i in one is shape i in the other.
            size_t shape_index = 0;
            for (const auto& shape_value : shapes_it->value.GetArray())
            {
                if (shape_index >= res.m_Shapes.size())
                {
                    break;
                }
                const auto geometry_it = shape_value.FindMember("geometry");
                if (geometry_it != shape_value.MemberEnd() && geometry_it->value.IsObject())
                {
                    const auto type_it = geometry_it->value.FindMember("$typeName");
                    if (type_it != geometry_it->value.MemberEnd() && type_it->value.IsString())
                    {
                        const std::string geometry_type = type_it->value.GetString();
                        const std::filesystem::path asset_path =
                            geometry_dir / (stem + "_shape" + std::to_string(shape_index) + ".zasset");
                        const int32_t id = ReifyInlineObjectByType(
                            &geometry_it->value, asset_path, geometry_type, where);
                        if (id != 0)
                        {
                            // The Res holds ImmediatePtr<RigidBodyShape>, which
                            // also reads as null from inline JSON. Create the
                            // shape here and restore its local transform, then
                            // hang the geometry off it.
                            RigidBodyShape* shape = res.m_Shapes[shape_index];
                            if (shape == nullptr)
                            {
                                shape = static_cast<RigidBodyShape*>(
                                    GET_SYSTEM(ObjectManager)->Produce(TypeOf<RigidBodyShape>(), 0));
                                if (shape == nullptr)
                                {
                                    std::fprintf(stderr,
                                                 "    FAIL  %s: cannot allocate RigidBodyShape\n",
                                                 where.c_str());
                                    ++g_failed;
                                    ++shape_index;
                                    continue;
                                }
                                res.m_Shapes[shape_index] = shape;
                            }

                            // "rotate" is the legacy spelling of "rotation"; an
                            // empty object leaves the identity rotation, which
                            // is what the authored data meant. The subtree is
                            // re-emitted under the key RigidBodyShape::Transfer
                            // actually reads.
                            const auto local_it = shape_value.FindMember("local_transform");
                            if (local_it != shape_value.MemberEnd() && local_it->value.IsObject())
                            {
                                std::string local_text;
                                if (DumpValueToString(local_it->value, local_text))
                                {
                                    const std::string wrapped = "{\"local_transform\":" + local_text + "}";
                                    JSONRead reader(wrapped.c_str(),
                                                    TransferInstructionFlags::kNoTransferInstructionFlags);
                                    JSONUtility::DeserializedFromJSONRead(reader, *shape);
                                }
                            }

                            shape->m_Geometry = PPtr<Geometry>(id);

                            // m_Type is redundant with the geometry's own type
                            // (toShape dispatches on m_Geometry), but leaving
                            // it at `invalid` misleads anyone reading the file.
                            if (geometry_type == "Box")
                            {
                                shape->m_Type = RigidBodyShapeType::box;
                            }
                            else if (geometry_type == "Sphere")
                            {
                                shape->m_Type = RigidBodyShapeType::sphere;
                            }
                            else if (geometry_type == "Capsule")
                            {
                                shape->m_Type = RigidBodyShapeType::capsule;
                            }
                        }
                    }
                }
                ++shape_index;
            }
            return;
        }

        if (type_name == "MotorComponent")
        {
            const auto config_value = FindMemberPath(component_entry, "$context", "motor_res", "controller_config");
            if (config_value == nullptr)
            {
                return;
            }
            const auto type_it = config_value->FindMember("$typeName");
            if (type_it == config_value->MemberEnd() || !type_it->value.IsString())
            {
                return;
            }
            const std::filesystem::path asset_path = source.parent_path() / (stem + "_controller_config.zasset");
            const int32_t id = ReifyInlineObjectByType(config_value, asset_path, type_it->value.GetString(), where);
            if (id != 0)
            {
                static_cast<MotorComponent*>(component)->GetMotorResMutable().m_ControllerConfig =
                    PPtr<ControllerConfig>(id);
            }
            return;
        }

        if (type_name == "CameraComponent")
        {
            const auto param_value = FindMemberPath(component_entry, "$context", "camera_res", "parameter");
            if (param_value == nullptr)
            {
                return;
            }
            const auto type_it = param_value->FindMember("$typeName");
            if (type_it == param_value->MemberEnd() || !type_it->value.IsString())
            {
                return;
            }
            const std::filesystem::path asset_path = source.parent_path() / (stem + "_camera_parameter.zasset");
            const int32_t id = ReifyInlineObjectByType(param_value, asset_path, type_it->value.GetString(), where);
            if (id != 0)
            {
                static_cast<CameraComponent*>(component)->GetCameraResMutable().m_Parameter =
                    PPtr<CameraParameter>(id);
            }
        }
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

            // The component's own Transfer cannot restore these: they are
            // inline in the legacy JSON but declared PPtr at runtime. Patch
            // them from the source DOM after construction.
            RestoreInlineReferences(component, type_name, entry, source, where);

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
    // Walk the same steps AssetManager::ReadObject(int32_t) walks and report
    // which one fails. ReadObject returns nullptr at six different points and
    // they all look identical from outside, so without this there is no way to
    // tell "remapper lost the id" from "the type is unregistered".
    void DiagnoseInstanceID(int32_t instance_id, const std::string& where)
    {
        std::fprintf(stderr, "         id=%d\n", instance_id);
        if (instance_id == 0)
        {
            std::fprintf(stderr, "         -> id is zero: nothing was ever assigned\n");
            return;
        }

        std::filesystem::path asset_path;
        int64_t local_id = 0;
        if (!GET_SYSTEM(AssetManager)->TryGetIdentityForInstance(instance_id, asset_path, local_id))
        {
            std::fprintf(stderr, "         -> FAIL: remapper has no (file, lfid) for this id\n");
            return;
        }
        std::fprintf(stderr, "         -> path=%s lfid=%lld\n",
                     asset_path.generic_string().c_str(), static_cast<long long>(local_id));

        std::error_code ec;
        if (!std::filesystem::exists(asset_path, ec))
        {
            std::fprintf(stderr, "         -> FAIL: file does not exist on disk\n");
            return;
        }
        std::fprintf(stderr, "         -> file exists (%lld bytes)\n",
                     static_cast<long long>(std::filesystem::file_size(asset_path, ec)));

        const std::string type_name = GET_SYSTEM(AssetManager)->GetAssetTypeName(asset_path);
        std::fprintf(stderr, "         -> asset type name='%s'\n", type_name.c_str());
        if (type_name.empty())
        {
            std::fprintf(stderr, "         -> FAIL: no type name stamped in the file\n");
            return;
        }

        const Type* resolved_type = TypeManager::GetInstance().ClassNameToType(type_name.c_str());
        if (resolved_type == nullptr)
        {
            std::fprintf(stderr, "         -> FAIL: ClassNameToType('%s') returned null\n", type_name.c_str());
            return;
        }
        std::fprintf(stderr, "         -> type resolved: %s (factory=%s)\n",
                     resolved_type->GetName(),
                     resolved_type->GetFactory() != nullptr ? "yes" : "NO");
        if (resolved_type->GetFactory() == nullptr)
        {
            std::fprintf(stderr, "         -> FAIL: type has no factory, Produce will return null\n");
            return;
        }

        // Everything upstream resolved, so try the load that ReadObject
        // itself would do. If this succeeds but PPtr still reads null, the
        // break is in the ID->object lookup, not in the file.
        std::filesystem::path load_path(asset_path);
        Object* loaded = GET_SYSTEM(AssetManager)->ReadObject(load_path, resolved_type);
        std::fprintf(stderr, "         -> ReadObject(path, type) = %s\n", loaded != nullptr ? "ok" : "NULL");

        // The two ways PPtr resolves: the in-memory table first, then the
        // on-demand load. Comparing them says whether the id is registered
        // at all or just not loadable.
        std::fprintf(stderr, "         -> IDToPointer(%d) = %s\n", instance_id,
                     GET_SYSTEM(ObjectManager)->IDToPointer(instance_id) != nullptr ? "hit" : "miss");
        Object* by_id = GET_SYSTEM(AssetManager)->ReadObject(instance_id);
        std::fprintf(stderr, "         -> ReadObject(instanceID) = %s\n", by_id != nullptr ? "ok" : "NULL");
        std::fprintf(stderr, "         -> GetSerializedFile(path) = %s\n",
                     GET_SYSTEM(AssetManager)->GetSerializedFile(load_path) != nullptr ? "ok" : "NULL");
    }

    // Load a written prefab back and report whether the references the
    // migrator emitted actually resolve. This is the only check that proves
    // the conversion is usable: a file that writes clean but reads back with
    // null geometry is still a wall with no collision.
    bool VerifyPrefab(const std::filesystem::path& prefab_path)
    {
        const std::string where = prefab_path.filename().string();

        // Read the object graph directly rather than going through
        // PrefabUtility::Instantiate: Instantiate runs PostLoadResource on
        // every component, and RigidBodyComponent::PostLoadResource builds a
        // physics body -- which blocks in this tool, where the full engine
        // system set was never brought up. This tool checks that references
        // resolve, not that a live body can be simulated.
        std::vector<std::pair<int64_t, Object*>> objects;
        if (!GET_SYSTEM(AssetManager)->ReadObjectsFromYaml(prefab_path, objects))
        {
            std::fprintf(stderr, "  FAIL  %s: cannot load back\n", where.c_str());
            ++g_failed;
            return false;
        }

        int resolved = 0;
        int dangling = 0;

        for (const auto& id_object_pair : objects)
        {
            Object* object = id_object_pair.second;
            if (object == nullptr)
            {
                continue;
            }
            Component* component = dynamic_cast<Component*>(object);
            if (component == nullptr)
            {
                continue;
            }

            if (RigidBodyComponent* rigidbody = dynamic_cast<RigidBodyComponent*>(component))
            {
                for (const ImmediatePtr<RigidBodyShape>& shape_ptr : rigidbody->GetRigidbodyRes().m_Shapes)
                {
                    RigidBodyShape* shape = shape_ptr;
                    if (shape == nullptr)
                    {
                        ++dangling;
                        std::fprintf(stderr, "    NULL %s: rigid body shape did not resolve\n", where.c_str());
                        continue;
                    }
                    Geometry* geometry = shape->m_Geometry;
                    if (geometry == nullptr)
                    {
                        ++dangling;
                        std::fprintf(stderr, "    NULL %s: shape geometry did not resolve\n", where.c_str());
                        DiagnoseInstanceID(shape->m_Geometry.GetInstanceID(), where);
                        continue;
                    }
                    ++resolved;
                    std::fprintf(stderr, "    ok   %s: shape %s\n", where.c_str(), geometry->GetTypeName());
                }
            }
            else if (MotorComponent* motor = dynamic_cast<MotorComponent*>(component))
            {
                ControllerConfig* config = motor->GetMotorResMutable().m_ControllerConfig;
                if (config == nullptr)
                {
                    ++dangling;
                    std::fprintf(stderr, "    NULL %s: controller config did not resolve\n", where.c_str());
                    DiagnoseInstanceID(motor->GetMotorResMutable().m_ControllerConfig.GetInstanceID(), where);
                }
                else
                {
                    ++resolved;
                    std::fprintf(stderr, "    ok   %s: controller config %s\n", where.c_str(), config->GetTypeName());
                }
            }
            else if (CameraComponent* camera = dynamic_cast<CameraComponent*>(component))
            {
                CameraParameter* parameter = camera->GetCameraResMutable().m_Parameter;
                if (parameter == nullptr)
                {
                    ++dangling;
                    std::fprintf(stderr, "    NULL %s: camera parameter did not resolve\n", where.c_str());
                    DiagnoseInstanceID(camera->GetCameraResMutable().m_Parameter.GetInstanceID(), where);
                }
                else
                {
                    ++resolved;
                    std::fprintf(stderr, "    ok   %s: camera parameter %s\n", where.c_str(), parameter->GetTypeName());
                }
            }
        }

        if (dangling > 0)
        {
            std::fprintf(stderr, "  FAIL  %s: %d reference(s) unresolved\n", where.c_str(), dangling);
            ++g_failed;
            return false;
        }

        std::fprintf(stderr, "  ok    %s (%d reference(s) resolved)\n", where.c_str(), resolved);
        return true;
    }
    // Print what the asset system thinks a single file is.
    // sniffed from the file. This separates two failure modes that look
    // identical from the outside: "the file is corrupt / wrong" versus "the
    // file is fine but the type cannot be recovered from it".
    bool LoadBoxExplicit(const std::filesystem::path& asset_path)
    {
        std::filesystem::path path(asset_path);
        Box* box = GET_SYSTEM(AssetManager)->ReadObject<Box>(path);
        if (box == nullptr)
        {
            std::fprintf(stderr, "  -> ReadObject<Box> returned null\n");
            return false;
        }
        std::fprintf(stderr, "  -> loaded Box: half_extents=(%.4f, %.4f, %.4f)\n",
                     box->m_HalfExtents.x, box->m_HalfExtents.y, box->m_HalfExtents.z);
        return true;
    }

    // Print what the asset system thinks a single file is. Used to compare a
    // migrator-emitted .zasset against a known-good one: if the good one also
    // reports empty, the problem is in the reader, not in what we wrote.
    bool ProbeAsset(const std::filesystem::path& asset_path)
    {
        std::error_code ec;
        std::fprintf(stderr, "probe %s\n", asset_path.generic_string().c_str());
        if (!std::filesystem::exists(asset_path, ec))
        {
            std::fprintf(stderr, "  -> does not exist\n");
            return false;
        }
        std::fprintf(stderr, "  -> %lld bytes\n", static_cast<long long>(std::filesystem::file_size(asset_path, ec)));

        const std::string type_name = GET_SYSTEM(AssetManager)->GetAssetTypeName(asset_path);
        std::fprintf(stderr, "  -> GetAssetTypeName='%s'\n", type_name.c_str());
        if (type_name.empty())
        {
            std::fprintf(stderr, "  -> FAIL: empty (same symptom as the migrator output)\n");
            return false;
        }

        const Type* resolved_type = TypeManager::GetInstance().ClassNameToType(type_name.c_str());
        std::fprintf(stderr, "  -> ClassNameToType=%s\n", resolved_type != nullptr ? resolved_type->GetName() : "null");
        return resolved_type != nullptr;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: ObjectDefinitionMigrator <asset-root> [--dry-run]\n");
        return 2;
    }

    // Probe has to short-circuit before the asset-root directory check: it
    // takes a single file, not a root. It registers the runtime itself because
    // it needs AssetManager and the runtime type table to resolve anything.
    if (argc >= 3)
    {
        const std::string mode(argv[2]);

        if (mode == "--loadbox")
        {
            if (argc < 4)
            {
                std::fprintf(stderr, "usage: ObjectDefinitionMigrator x --loadbox <asset path>\n");
                return 2;
            }
            RegisterRuntime();
            REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);
            return LoadBoxExplicit(argv[3]) ? 0 : 1;
        }

        if (mode == "--probe")
        {
            if (argc < 4)
            {
                std::fprintf(stderr, "usage: ObjectDefinitionMigrator x --probe <asset path>\n");
                return 2;
            }
            RegisterRuntime();
            REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);
            return ProbeAsset(argv[3]) ? 0 : 1;
        }
    }

    const std::filesystem::path asset_root(argv[1]);
    const bool dry_run = (argc >= 3) && (std::string(argv[2]) == "--dry-run");
    const bool verify = (argc >= 3) && (std::string(argv[2]) == "--verify");

    std::error_code ec;
    if (!std::filesystem::is_directory(asset_root, ec))
    {
        std::fprintf(stderr, "not a directory: %s\n", asset_root.string().c_str());
        return 2;
    }

    std::vector<std::filesystem::path> definitions;
    const char* wanted_suffix = verify ? ".prefab" : ".object.json";
    const char* found_label = verify ? "prefab" : "object definition";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_root, ec))
    {
        if (ec || !entry.is_regular_file())
        {
            continue;
        }
        if (entry.path().filename().string().find(wanted_suffix) != std::string::npos)
        {
            definitions.push_back(entry.path());
        }
    }

    std::fprintf(stderr, "found %zu %s(s) under %s\n", definitions.size(), found_label, asset_root.string().c_str());
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
        if (verify)
        {
            VerifyPrefab(source);
            continue;
        }

        // "wall.object.json" -> "wall.prefab": strip both trailing suffixes via
        // the stem rather than replace_extension, which only peels one.
        std::filesystem::path target = source.parent_path() / (source.stem().stem().string() + ".prefab");
        ConvertDefinition(source, target, dry_run);
    }

    std::fprintf(stderr, "\nconverted=%d skipped=%d failed=%d\n", g_converted, g_skipped, g_failed);
    return (g_failed == 0) ? 0 : 1;
}