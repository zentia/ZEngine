// =====================================================================
// YAML text-serialization round-trip smoke test.
// ---------------------------------------------------------------------
// Standalone executable that proves the new block-YAML text backend
// (`YAMLWrite` / `YAMLRead`, built on the shared rapidjson DOM the JSON
// backend already uses) faithfully round-trips Transfer()-driven data:
//
//     value --YAMLWrite--> YAML text --YAMLRead--> value'   (value == value')
//
// The backend is a peer of JSONWrite / JSONRead: it builds / consumes the
// identical rapidjson node tree and differs only in the on-disk encoding
// (block YAML instead of JSON). These scenarios exercise the encoding's
// genuinely new code -- the emitter (YamlText.cpp EmitYaml) and the
// constrained parser (ParseYaml):
//
//   Y1 scalars:      string / int / negative int / float / bool / int64.
//   Y2 nested map:   a struct field whose value is another struct.
//   Y3 sequences:    vector<int> (scalar sequence) and vector<struct>
//                    (sequence of mappings -- the inline-first-key path).
//   Y4 tricky strings: empty, spaces, embedded ':' / '#', numeric-looking,
//                    reserved words ("true"/"null") -- all must survive as
//                    strings rather than re-parsing as another YAML type.
//
// Why a standalone main() and not CTest/GoogleTest: matches the existing
// precedent (SchemaEvolutionSmokeTest / PptrSmokeTest / *_bindless_smoke_test);
// the engine ships no unit-test harness yet, and this test only needs the
// Core systems (MemoryManager via the rapidjson allocator).
//
// Exit codes: 0 all passed; 1 a round-trip failed; 77 environment bring-up
// failure (skipped, not a regression).
//
// Build: only when -DZENGINE_BUILD_YAML_ROUND_TRIP_SMOKE_TEST=ON. Default
// OFF; no normal build is affected. See the sibling CMakeLists.txt.
// =====================================================================

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Serialize/SerializeTraits.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Core/YamlSerialize/YAMLUtility.h"
#include "Runtime/Core/YamlSerialize/YamlObjectGraph.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/ResType/Common/Level.h"
#include "Runtime/Resource/ResType/Data/TimelineAsset.h"
#include "Runtime/Resource/ResType/Data/TimelineAssetIO.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Runtime/BaseClasses/Cast.h"

// ---------------------------------------------------------------------
// Test-only plain serializable structs (DECLARE_SERIALIZE). They are NOT
// Object subclasses -- the text backend reaches them through the default
// SerializeTraits<T>::Transfer -> data.Transfer(transfer) path, so no
// TypeManager registration is required.
// ---------------------------------------------------------------------

struct YamlInner
{
    DECLARE_SERIALIZE(YamlInner)

    eastl::string name;
    int32_t value {0};
    float ratio {0.0f};

    bool operator==(const YamlInner& o) const
    {
        return name == o.name && value == o.value && ratio == o.ratio;
    }
};

template<class TransferFunction>
void YamlInner::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(value, "value");
    transfer.Transfer(ratio, "ratio");
}

struct YamlScalars
{
    DECLARE_SERIALIZE(YamlScalars)

    eastl::string text;
    int32_t count {0};
    int32_t negative {0};
    float amount {0.0f};
    bool enabled {false};
    int64_t big {0};

    bool operator==(const YamlScalars& o) const
    {
        return text == o.text && count == o.count && negative == o.negative && amount == o.amount &&
               enabled == o.enabled && big == o.big;
    }
};

template<class TransferFunction>
void YamlScalars::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(text, "text");
    transfer.Transfer(count, "count");
    transfer.Transfer(negative, "negative");
    transfer.Transfer(amount, "amount");
    transfer.Transfer(enabled, "enabled");
    transfer.Transfer(big, "big");
}

struct YamlComposite
{
    DECLARE_SERIALIZE(YamlComposite)

    eastl::string id;
    YamlInner inner;
    std::vector<int32_t> numbers;
    std::vector<YamlInner> items;

    bool operator==(const YamlComposite& o) const
    {
        return id == o.id && inner == o.inner && numbers == o.numbers && items == o.items;
    }
};

template<class TransferFunction>
void YamlComposite::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(id, "id");
    transfer.Transfer(inner, "inner");
    transfer.Transfer(numbers, "numbers");
    transfer.Transfer(items, "items");
}

// ---------------------------------------------------------------------
// Harness.
// ---------------------------------------------------------------------
namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* scenario, const char* detail)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "  [FAIL] %s: %s\n", scenario, detail);
        }
    }

    template<typename T>
    bool RoundTrip(const T& in, T& out, const char* scenario, bool dump)
    {
        eastl::string yaml;
        YAMLUtility::SerializeToYAML(in, yaml);
        if (dump)
        {
            std::fprintf(stderr, "  --- %s YAML ---\n%s  ---------------\n", scenario, yaml.c_str());
        }
        YAMLUtility::DeserializeFromYAML(yaml.c_str(), out);
        return true;
    }
}  // namespace

static void scenario_Y1_scalars()
{
    std::fprintf(stderr, "[Y1] scalars\n");
    YamlScalars in;
    in.text = "hello world";
    in.count = 42;
    in.negative = -17;
    in.amount = 3.5f;
    in.enabled = true;
    in.big = 9000000000LL;  // > INT32_MAX

    YamlScalars out;
    RoundTrip(in, out, "Y1", /*dump=*/true);
    Check(in == out, "Y1", "scalar struct did not survive round-trip");
}

static void scenario_Y2_nested()
{
    std::fprintf(stderr, "[Y2] nested map\n");
    YamlComposite in;
    in.id = "root";
    in.inner.name = "child";
    in.inner.value = 7;
    in.inner.ratio = 0.25f;

    YamlComposite out;
    RoundTrip(in, out, "Y2", /*dump=*/true);
    Check(in.id == out.id, "Y2", "id mismatch");
    Check(in.inner == out.inner, "Y2", "nested struct mismatch");
}

static void scenario_Y3_sequences()
{
    std::fprintf(stderr, "[Y3] sequences\n");
    YamlComposite in;
    in.id = "seq";
    in.numbers = {1, 2, 3, -4, 5};
    YamlInner a;
    a.name = "alpha";
    a.value = 10;
    a.ratio = 1.5f;
    YamlInner b;
    b.name = "beta";
    b.value = 20;
    b.ratio = 2.5f;
    in.items = {a, b};

    YamlComposite out;
    RoundTrip(in, out, "Y3", /*dump=*/true);
    Check(in.numbers == out.numbers, "Y3", "scalar sequence mismatch");
    Check(in.items == out.items, "Y3", "struct sequence (inline-first-key) mismatch");
}

static void scenario_Y4_tricky_strings()
{
    std::fprintf(stderr, "[Y4] tricky strings\n");
    const char* tricky[] = {
        "",            // empty -> must not read back as null
        "  spaced  ",  // leading/trailing spaces -> must be quoted
        "key: value",  // embedded ": " -> must be quoted
        "trailing#",   // contains '#'
        "12345",       // numeric-looking -> must stay string
        "true",        // reserved bool word -> must stay string
        "null",        // reserved null word -> must stay string
        "-3.14",       // numeric-looking negative
        "Assets/Foo.zasset",  // path -> stays plain, must survive
    };
    for (const char* s : tricky)
    {
        YamlScalars in;
        in.text = s;
        YamlScalars out;
        RoundTrip(in, out, "Y4", /*dump=*/false);
        char detail[256];
        std::snprintf(detail, sizeof(detail), "string '%s' became '%s'", s, out.text.c_str());
        Check(in.text == out.text, "Y4", detail);
    }
}

// ---------------------------------------------------------------------
// Y5: multi-object graph round-trip (the scene / prefab path).
// Produces a GameObject + a Transform through ObjectManager so
// both have valid InstanceIDs, links them via the ImmediatePtr container,
// then exercises WriteObjectGraph -> YAML -> ReadObjectGraph and asserts:
//   - both objects are produced back by their class-name tag,
//   - the GameObject's name survived (GameObject::Transfer),
//   - the GameObject's component ImmediatePtr resolved to a *new*
//     Transform in the read graph (local fileID ref + the
//     IPPtrResolver + ImmediatePtr::Transfer round-trip).
// ---------------------------------------------------------------------
static void scenario_Y5_object_graph()
{
    std::fprintf(stderr, "[Y5] multi-object graph (scene path)\n");

    auto om = GET_SYSTEM(ObjectManager);
    if (!om)
    {
        Check(false, "Y5", "ObjectManager not available");
        return;
    }

    GameObject* go = static_cast<GameObject*>(om->Produce(TypeOf<GameObject>(), 0));
    Transform* tc = static_cast<Transform*>(om->Produce(TypeOf<Transform>(), 0));
    if (go == nullptr || tc == nullptr)
    {
        Check(false, "Y5", "failed to Produce GameObject / Transform");
        return;
    }
    om->AllocateAndAssignInstanceID(go);
    om->AllocateAndAssignInstanceID(tc);

    go->SetName("Player");
    go->addComponent(tc);
    go->SyncSerializedComponents();

    std::vector<ZYaml::ObjectGraphEntry> in_entries;
    in_entries.push_back({1, go});
    in_entries.push_back({2, tc});

    eastl::string yaml;
    if (!ZYaml::WriteObjectGraph(in_entries, yaml))
    {
        Check(false, "Y5", "WriteObjectGraph returned false");
        return;
    }
    std::fprintf(stderr, "  --- Y5 YAML ---\n%s  ---------------\n", yaml.c_str());

    std::vector<ZYaml::ObjectGraphEntry> out_entries;
    if (!ZYaml::ReadObjectGraph(yaml.c_str(), out_entries))
    {
        Check(false, "Y5", "ReadObjectGraph returned false");
        return;
    }

    Check(out_entries.size() == 2, "Y5", "expected 2 objects back from graph");

    GameObject* read_go = nullptr;
    for (const ZYaml::ObjectGraphEntry& e : out_entries)
    {
        if (e.object != nullptr && e.object->GetType() == TypeOf<GameObject>())
        {
            read_go = static_cast<GameObject*>(e.object);
            break;
        }
    }
    Check(read_go != nullptr, "Y5", "GameObject not produced back by class tag");
    if (read_go == nullptr)
        return;

    Check(read_go->GetName() == eastl::string("Player"), "Y5", "GameObject name did not survive");

    read_go->RebuildRuntimeComponents();
    std::vector<ImmediatePtr<Component>> comps = read_go->getComponents();
    Check(comps.size() == 1, "Y5", "component ImmediatePtr did not resolve (expected 1 component)");
    if (comps.size() == 1)
    {
        Component* c = comps[0];
        Check(c != nullptr && c->GetType() == TypeOf<Transform>(), "Y5",
              "resolved component is not a Transform");
    }
}

// ---------------------------------------------------------------------
// Y6: prefab graph round-trip (the .prefab path). Mirrors
// PrefabUtility::SaveAsPrefabAsset's flattened object list: a PrefabAsset
// header (fileID 1) whose m_RootGameObject ImmediatePtr points at the root
// GameObject, plus the GameObject + its Transform. Verifies:
//   - PrefabAsset produced back, m_RootGameObject ImmediatePtr resolved,
//   - the resolved root is the same-fileID GameObject (not the source one).
// ---------------------------------------------------------------------
static void scenario_Y6_prefab_graph()
{
    std::fprintf(stderr, "[Y6] prefab graph (.prefab path)\n");

    auto om = GET_SYSTEM(ObjectManager);
    if (!om)
    {
        Check(false, "Y6", "ObjectManager not available");
        return;
    }

    PrefabAsset* prefab = static_cast<PrefabAsset*>(om->Produce(TypeOf<PrefabAsset>(), 0));
    GameObject* root = static_cast<GameObject*>(om->Produce(TypeOf<GameObject>(), 0));
    Transform* tc = static_cast<Transform*>(om->Produce(TypeOf<Transform>(), 0));
    if (prefab == nullptr || root == nullptr || tc == nullptr)
    {
        Check(false, "Y6", "failed to Produce PrefabAsset / GameObject / Transform");
        return;
    }
    om->AllocateAndAssignInstanceID(prefab);
    om->AllocateAndAssignInstanceID(root);
    om->AllocateAndAssignInstanceID(tc);

    root->SetName("PrefabRoot");
    root->addComponent(tc);
    root->SyncSerializedComponents();
    prefab->SetRootGameObject(root);

    // Same ordered list PrefabUtility builds: [PrefabAsset, root, components...].
    std::vector<ZYaml::ObjectGraphEntry> in_entries;
    in_entries.push_back({1, prefab});
    in_entries.push_back({2, root});
    in_entries.push_back({3, tc});

    eastl::string yaml;
    if (!ZYaml::WriteObjectGraph(in_entries, yaml))
    {
        Check(false, "Y6", "WriteObjectGraph returned false");
        return;
    }

    std::vector<ZYaml::ObjectGraphEntry> out_entries;
    if (!ZYaml::ReadObjectGraph(yaml.c_str(), out_entries))
    {
        Check(false, "Y6", "ReadObjectGraph returned false");
        return;
    }

    PrefabAsset* read_prefab = nullptr;
    for (const ZYaml::ObjectGraphEntry& e : out_entries)
    {
        if (e.object != nullptr && e.object->GetType() == TypeOf<PrefabAsset>())
        {
            read_prefab = static_cast<PrefabAsset*>(e.object);
            break;
        }
    }
    Check(read_prefab != nullptr, "Y6", "PrefabAsset not produced back");
    if (read_prefab == nullptr)
        return;

    GameObject* read_root = read_prefab->GetRootGameObject();
    Check(read_root != nullptr, "Y6", "PrefabAsset.m_RootGameObject ImmediatePtr did not resolve");
    Check(read_root != root, "Y6", "resolved root should be the freshly-read object, not the source");
    if (read_root != nullptr)
    {
        Check(read_root->GetName() == eastl::string("PrefabRoot"), "Y6", "root name did not survive");
    }
}

// ---------------------------------------------------------------------
// Y7: multi-LEVEL Transform hierarchy round-trip (root -> child ->
// grandchild). This is the serialization half of the PrefabPostLoadDriver
// multi-level fix: each level is its own GameObject + Transform,
// linked through PPtr<Transform> m_Children / m_Parent. After the
// graph round-trip we assert the full 3-deep chain resolves back, proving
// the child-Transform PPtr sequence (not just a single level) survives and
// that the driver's owner map can therefore reach grandchildren.
// ---------------------------------------------------------------------
static void scenario_Y7_multilevel_hierarchy()
{
    std::fprintf(stderr, "[Y7] multi-level Transform hierarchy\n");

    auto om = GET_SYSTEM(ObjectManager);
    if (!om)
    {
        Check(false, "Y7", "ObjectManager not available");
        return;
    }

    GameObject* gos[3] = {nullptr, nullptr, nullptr};
    Transform* tcs[3] = {nullptr, nullptr, nullptr};
    const char* names[3] = {"Root", "Child", "Grandchild"};
    for (int i = 0; i < 3; ++i)
    {
        gos[i] = static_cast<GameObject*>(om->Produce(TypeOf<GameObject>(), 0));
        tcs[i] = static_cast<Transform*>(om->Produce(TypeOf<Transform>(), 0));
        if (gos[i] == nullptr || tcs[i] == nullptr)
        {
            Check(false, "Y7", "failed to Produce GameObject / Transform");
            return;
        }
        om->AllocateAndAssignInstanceID(gos[i]);
        om->AllocateAndAssignInstanceID(tcs[i]);
        gos[i]->SetName(names[i]);
        gos[i]->addComponent(tcs[i]);
        gos[i]->SyncSerializedComponents();
    }
    // root -> child -> grandchild (worldPositionStays=false keeps it owner-free).
    tcs[1]->SetParent(tcs[0], false);
    tcs[2]->SetParent(tcs[1], false);

    std::vector<ZYaml::ObjectGraphEntry> in_entries;
    int file_id = 1;
    for (int i = 0; i < 3; ++i)
    {
        in_entries.push_back({file_id++, gos[i]});
        in_entries.push_back({file_id++, tcs[i]});
    }

    eastl::string yaml;
    if (!ZYaml::WriteObjectGraph(in_entries, yaml))
    {
        Check(false, "Y7", "WriteObjectGraph returned false");
        return;
    }

    std::vector<ZYaml::ObjectGraphEntry> out_entries;
    if (!ZYaml::ReadObjectGraph(yaml.c_str(), out_entries))
    {
        Check(false, "Y7", "ReadObjectGraph returned false");
        return;
    }

    // Find the read root GameObject by name, rebuild components, then walk the
    // Transform chain down two levels and check every link resolved.
    GameObject* read_root = nullptr;
    for (const ZYaml::ObjectGraphEntry& e : out_entries)
    {
        if (e.object != nullptr && e.object->GetType() == TypeOf<GameObject>())
        {
            GameObject* g = static_cast<GameObject*>(e.object);
            g->RebuildRuntimeComponents();
            if (g->GetName() == eastl::string("Root"))
            {
                read_root = g;
            }
        }
    }
    Check(read_root != nullptr, "Y7", "read root GameObject not found");
    if (read_root == nullptr)
        return;

    Transform* root_tc = read_root->tryGetComponent(Transform);
    Check(root_tc != nullptr, "Y7", "root Transform did not resolve");
    if (root_tc == nullptr)
        return;

    Check(root_tc->GetChildCount() == 1, "Y7", "root should have exactly 1 child Transform");
    Transform* child_tc = root_tc->GetChild(0);
    Check(child_tc != nullptr, "Y7", "child Transform PPtr did not resolve");
    if (child_tc == nullptr)
        return;

    Check(child_tc->GetChildCount() == 1, "Y7", "child should have exactly 1 grandchild Transform");
    Transform* grandchild_tc = child_tc->GetChild(0);
    Check(grandchild_tc != nullptr, "Y7", "grandchild Transform PPtr did not resolve (depth-2 link lost)");
    if (grandchild_tc != nullptr)
    {
        Check(grandchild_tc->GetParent() == child_tc, "Y7", "grandchild m_Parent back-pointer mismatch");
    }
}

// ---------------------------------------------------------------------
// Y8: DISK round-trip through the real AssetManager scene API. Mirrors
// Level::save / Level::load exactly: a LevelRes header (fileID 1, carrying
// gravity + character name) followed by a multi-level GameObject hierarchy
// (root->child->grandchild) and each GameObject's Transform, written
// to a temp .scene file via AssetManager::WriteObjectsToYaml and read back via
// ReadObjectsFromYaml. Proves the full editor save/load path -- file I/O +
// the writer/reader external-ref hooks (no externals here, so they no-op) +
// graph resolution -- works headlessly, without the editor GUI. Uses the
// concrete RuntimeAssetManager (lightweight: only path<->streamID maps).
// ---------------------------------------------------------------------
static void scenario_Y8_assetmanager_disk_roundtrip()
{
    std::fprintf(stderr, "[Y8] AssetManager disk round-trip (.scene save/load)\n");

    auto om = GET_SYSTEM(ObjectManager);
    if (!om)
    {
        Check(false, "Y8", "ObjectManager not available");
        return;
    }

    LevelRes* header = static_cast<LevelRes*>(om->Produce(TypeOf<LevelRes>(), 0));
    if (header == nullptr)
    {
        Check(false, "Y8", "failed to Produce LevelRes header");
        return;
    }
    om->AllocateAndAssignInstanceID(header);
    header->m_Gravity = Vector3(0.f, 0.f, -19.6f);
    header->m_CharacterName = "Hero";

    GameObject* gos[3] = {nullptr, nullptr, nullptr};
    Transform* tcs[3] = {nullptr, nullptr, nullptr};
    const char* names[3] = {"SceneRoot", "SceneChild", "SceneGrandchild"};
    for (int i = 0; i < 3; ++i)
    {
        gos[i] = static_cast<GameObject*>(om->Produce(TypeOf<GameObject>(), 0));
        tcs[i] = static_cast<Transform*>(om->Produce(TypeOf<Transform>(), 0));
        if (gos[i] == nullptr || tcs[i] == nullptr)
        {
            Check(false, "Y8", "failed to Produce GameObject / Transform");
            return;
        }
        om->AllocateAndAssignInstanceID(gos[i]);
        om->AllocateAndAssignInstanceID(tcs[i]);
        gos[i]->SetName(names[i]);
        gos[i]->addComponent(tcs[i]);
        gos[i]->SyncSerializedComponents();
    }
    tcs[1]->SetParent(tcs[0], false);
    tcs[2]->SetParent(tcs[1], false);

    // Exact graph layout Level::save builds: [LevelRes@1, go,tc, go,tc, go,tc].
    std::vector<Object*> graph_objects;
    std::vector<int64_t> graph_ids;
    graph_objects.push_back(header);
    graph_ids.push_back(1);
    int64_t next_file_id = 2;
    for (int i = 0; i < 3; ++i)
    {
        graph_objects.push_back(gos[i]);
        graph_ids.push_back(next_file_id++);
        graph_objects.push_back(tcs[i]);
        graph_ids.push_back(next_file_id++);
    }

    std::filesystem::path scene_path =
        std::filesystem::temp_directory_path() / "zengine_yaml_y8_roundtrip.scene";
    std::error_code ec;
    std::filesystem::remove(scene_path, ec);

    RuntimeAssetManager ram;
    bool wrote = ram.WriteObjectsToYaml(scene_path, graph_objects.data(), graph_ids.data(),
                                        graph_objects.size());
    Check(wrote, "Y8", "WriteObjectsToYaml returned false");
    Check(std::filesystem::exists(scene_path, ec), "Y8", ".scene file was not created on disk");
    if (!wrote)
        return;

    std::vector<std::pair<int64_t, Object*>> out;
    bool read = ram.ReadObjectsFromYaml(scene_path, out);
    Check(read, "Y8", "ReadObjectsFromYaml returned false");
    if (!read)
    {
        std::filesystem::remove(scene_path, ec);
        return;
    }

    // Header back with its scene settings.
    LevelRes* read_header = nullptr;
    GameObject* read_root = nullptr;
    int read_go_count = 0;
    for (const auto& entry : out)
    {
        Object* o = entry.second;
        if (o == nullptr)
            continue;
        if (o->GetType() == TypeOf<LevelRes>())
        {
            read_header = static_cast<LevelRes*>(o);
        }
        else if (o->GetType() == TypeOf<GameObject>())
        {
            ++read_go_count;
            GameObject* g = static_cast<GameObject*>(o);
            g->RebuildRuntimeComponents();
            if (g->GetName() == eastl::string("SceneRoot"))
            {
                read_root = g;
            }
        }
    }

    Check(read_header != nullptr, "Y8", "LevelRes header not produced back from disk");
    if (read_header != nullptr)
    {
        Check(read_header->m_CharacterName == eastl::string("Hero"), "Y8",
              "LevelRes.m_CharacterName did not survive disk round-trip");
        Check(std::fabs(read_header->m_Gravity.z - (-19.6f)) < 1e-4f, "Y8",
              "LevelRes.m_Gravity did not survive disk round-trip");
    }
    Check(read_go_count == 3, "Y8", "expected 3 GameObjects back from disk");

    // Multi-level hierarchy survived the disk round-trip end to end.
    Check(read_root != nullptr, "Y8", "read SceneRoot GameObject not found");
    if (read_root != nullptr)
    {
        Transform* root_tc = read_root->tryGetComponent(Transform);
        Check(root_tc != nullptr, "Y8", "root Transform did not resolve from disk");
        if (root_tc != nullptr && root_tc->GetChildCount() == 1)
        {
            Transform* child_tc = root_tc->GetChild(0);
            Check(child_tc != nullptr && child_tc->GetChildCount() == 1, "Y8",
                  "child->grandchild link lost across disk round-trip");
        }
        else
        {
            Check(false, "Y8", "root should have exactly 1 child after disk round-trip");
        }
    }

    std::filesystem::remove(scene_path, ec);
}

// ---------------------------------------------------------------------
// Y9: TimelineAsset graph (asset + tracks + clips) through TimelineAssetIO,
// once as a YAML .timeline and once as a binary .zasset. Every track / clip
// is a sub-object behind a PPtr, so this proves the graph is written and
// read as a whole and that the concrete subclasses come back by type name.
// ---------------------------------------------------------------------
namespace
{
    TimelineAsset* BuildTimeline()
    {
        auto om = GET_SYSTEM(ObjectManager);
        TimelineAsset* asset = om->NewObject<TimelineAsset>();
        asset->m_Duration = 12.5f;
        asset->m_FrameRate = 60.0f;

        AnimationTimelineTrack* anim_track = om->NewObject<AnimationTimelineTrack>();
        anim_track->m_Name = "Hero Anim";
        anim_track->m_TargetObjectId = 0x1122334455667788ull;
        AnimationTimelineClip* run = om->NewObject<AnimationTimelineClip>();
        run->m_StartTime = 0.5f;
        run->m_Duration = 2.0f;
        run->m_AnimationPath = "Assets/Anim/run.zasset";
        run->m_Speed = 1.5f;
        run->m_Loop = true;
        anim_track->m_Clips.push_back(run);
        asset->m_Tracks.push_back(anim_track);

        EventTimelineTrack* event_track = om->NewObject<EventTimelineTrack>();
        event_track->m_Name = "Events";
        event_track->m_Muted = true;
        EventTimelineClip* hit = om->NewObject<EventTimelineClip>();
        hit->m_StartTime = 3.0f;
        hit->m_EventName = "OnHit";
        hit->m_EventParams = "{\"damage\": 10}";
        ActivationTimelineClip* show = om->NewObject<ActivationTimelineClip>();
        show->m_StartTime = 4.0f;
        show->m_Enabled = false;
        show->m_Active = false;
        event_track->m_Clips.push_back(hit);
        event_track->m_Clips.push_back(show);
        asset->m_Tracks.push_back(event_track);

        AudioTimelineTrack* audio_track = om->NewObject<AudioTimelineTrack>();
        audio_track->m_Locked = true;
        AudioTimelineClip* music = om->NewObject<AudioTimelineClip>();
        music->m_AudioPath = "Assets/Audio/theme.wav";
        music->m_Volume = 0.25f;
        audio_track->m_Clips.push_back(music);
        asset->m_Tracks.push_back(audio_track);
        return asset;
    }

    void CheckTimeline(const TimelineAsset* loaded, const TimelineAsset* source, const char* tag)
    {
        Check(loaded != nullptr, tag, "TimelineAssetIO::Load returned null");
        if (loaded == nullptr)
            return;
        Check(loaded != source, tag, "Load returned the in-memory source instead of a freshly read graph");
        Check(loaded->m_Duration == 12.5f && loaded->m_FrameRate == 60.0f, tag, "asset fields lost");
        Check(loaded->m_Tracks.size() == 3, tag, "track count changed");
        if (loaded->m_Tracks.size() != 3)
            return;

        TimelineTrack* t0 = loaded->m_Tracks[0];
        TimelineTrack* t1 = loaded->m_Tracks[1];
        TimelineTrack* t2 = loaded->m_Tracks[2];
        Check(t0 && t1 && t2, tag, "a track PPtr did not resolve");
        if (!(t0 && t1 && t2))
            return;
        Check(t0->GetType() == TypeOf<AnimationTimelineTrack>(), tag, "track 0 lost its concrete type");
        Check(t1->GetType() == TypeOf<EventTimelineTrack>(), tag, "track 1 lost its concrete type");
        Check(t2->GetType() == TypeOf<AudioTimelineTrack>(), tag, "track 2 lost its concrete type");
        Check(t0->Is(TypeOf<TimelineTrack>()), tag, "Is<TimelineTrack>() is false for a track");
        Check(t0->m_Name == "Hero Anim" && t0->m_TargetObjectId == 0x1122334455667788ull, tag, "track 0 fields lost");
        Check(t1->m_Muted && !t1->m_Locked && t2->m_Locked, tag, "track flags lost");

        Check(t0->m_Clips.size() == 1 && t1->m_Clips.size() == 2 && t2->m_Clips.size() == 1, tag, "clip counts changed");
        if (t0->m_Clips.size() != 1 || t1->m_Clips.size() != 2 || t2->m_Clips.size() != 1)
            return;
        auto* run = Cast<AnimationTimelineClip>(static_cast<TimelineClip*>(t0->m_Clips[0]));
        auto* hit = Cast<EventTimelineClip>(static_cast<TimelineClip*>(t1->m_Clips[0]));
        auto* show = Cast<ActivationTimelineClip>(static_cast<TimelineClip*>(t1->m_Clips[1]));
        auto* music = Cast<AudioTimelineClip>(static_cast<TimelineClip*>(t2->m_Clips[0]));
        Check(run && hit && show && music, tag, "a clip PPtr did not resolve to its concrete type");
        if (!(run && hit && show && music))
            return;
        Check(run->m_StartTime == 0.5f && run->m_Duration == 2.0f && run->m_Speed == 1.5f && run->m_Loop &&
                  run->m_AnimationPath == "Assets/Anim/run.zasset",
              tag, "animation clip fields lost");
        Check(hit->m_StartTime == 3.0f && hit->m_EventName == "OnHit" && hit->m_EventParams == "{\"damage\": 10}", tag, "event clip fields lost");
        Check(show->m_StartTime == 4.0f && !show->m_Enabled && !show->m_Active, tag, "activation clip fields lost");
        Check(music->m_AudioPath == "Assets/Audio/theme.wav" && music->m_Volume == 0.25f, tag, "audio clip fields lost");
        Check(loaded->calculateActualDuration() == 4.0f, tag, "calculateActualDuration over the loaded graph is wrong");
    }
}  // namespace

static void scenario_Y9_timeline_graph()
{
    std::fprintf(stderr, "[Y9] TimelineAsset graph (.timeline YAML + .zasset binary)\n");
    TimelineAsset* source = BuildTimeline();

    std::vector<Object*> objects;
    std::vector<int64_t> ids;
    TimelineAssetIO::CollectObjectGraph(*source, objects, ids);
    Check(objects.size() == 8 && ids.front() == 1 && objects.front() == source, "Y9", "graph must be asset@1 + 3 tracks + 4 clips");

    RuntimeAssetManager ram;
    std::error_code ec;
    const std::filesystem::path yaml_path = std::filesystem::temp_directory_path() / "zengine_y9_roundtrip.timeline";
    const std::filesystem::path binary_path = std::filesystem::temp_directory_path() / "zengine_y9_roundtrip.zasset";
    std::filesystem::remove(yaml_path, ec);
    std::filesystem::remove(binary_path, ec);

    Check(TimelineAssetIO::Save(ram, *source, yaml_path), "Y9", "YAML save failed");
    Check(ram.GetAssetTypeName(yaml_path) == "TimelineAsset", "Y9", ".timeline is not reported as a TimelineAsset");
    CheckTimeline(TimelineAssetIO::Load(ram, yaml_path), source, "Y9/yaml");

    Check(TimelineAssetIO::Save(ram, *source, binary_path), "Y9", "binary save failed");
    TimelineAsset* binary_loaded = TimelineAssetIO::Load(ram, binary_path);
    CheckTimeline(binary_loaded, source, "Y9/binary");
    Check(TimelineAssetIO::Load(ram, binary_path) == binary_loaded, "Y9", "second binary Load did not reuse the hydrated graph");

    std::filesystem::remove(yaml_path, ec);
    std::filesystem::remove(binary_path, ec);
}

int main()
{
    std::fprintf(stderr,
                 "ZEngine YAML round-trip smoke test\n"
                 "==================================\n");
    std::fflush(stderr);

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr, "FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    scenario_Y1_scalars();
    scenario_Y2_nested();
    scenario_Y3_sequences();
    scenario_Y4_tricky_strings();
    scenario_Y5_object_graph();
    scenario_Y6_prefab_graph();
    scenario_Y7_multilevel_hierarchy();
    scenario_Y8_assetmanager_disk_roundtrip();
    scenario_Y9_timeline_graph();

    SystemRegistry::GetInstance().ShutdownAll();

    if (g_failures > 0)
    {
        std::fprintf(stderr, "\nYAML round-trip smoke test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nYAML round-trip smoke test: ALL PASSED\n");
    return 0;
}
