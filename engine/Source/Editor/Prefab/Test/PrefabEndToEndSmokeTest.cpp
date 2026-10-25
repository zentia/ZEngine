// =====================================================================
// Prefab end-to-end smoke test (PrefabSystem_Design.md §9.4 items 1 + 2).
// ---------------------------------------------------------------------
// Drives the whole authoring loop through the real code, disk included:
//
//   P1 author a GameObject tree (root + child, both with Transform) and
//      PrefabUtility::SaveAsPrefabAsset it to a .zasset
//   P2 PrefabUtilityEditor::InstantiateAsPrefab(path): read back from disk,
//      structurally cloned, PrefabInstance bookkeeping seeded
//   P3 change a field on the instance, RecordCurrentValueAsOverride, ApplyAll:
//      the change lands on the source PrefabAsset (in memory), the override
//      list is cleared
//   P4 persistence: save the applied source to a second .zasset and read
//      THAT file back as fresh objects -- the applied value survives disk
//   P5 Revert: an un-applied override is pulled back to the source value
//   P6 PrefabInstance itself round-trips through a scene file (binary
//      .zasset and YAML .scene): overrides, corresponding-source map and the
//      instance root survive
//
// Standalone main(); links ZRuntime plus the Editor/Prefab TUs (which are not
// in libZRuntime). Run headless on the web build with:
//   scripts/verify_prefab_e2e.sh
//
// Exit codes: 0 all passed; 1 a check failed; 77 environment bring-up failed.
// =====================================================================

#include "Editor/Prefab/PrefabInstance.h"
#include "Editor/Prefab/PrefabUtilityEditor.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* scenario, const char* detail)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::fprintf(stderr, "  [FAIL] %s: %s\n", scenario, detail);
        }
    }

    bool Near(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

    Transform* TransformOf(GameObject* go) { return go != nullptr ? go->tryGetComponent(Transform) : nullptr; }

    GameObject* MakeGameObject(ObjectManager* om, const char* name)
    {
        GameObject* go = static_cast<GameObject*>(om->Produce(TypeOf<GameObject>(), 0));
        Transform* tc = static_cast<Transform*>(om->Produce(TypeOf<Transform>(), 0));
        if (go == nullptr || tc == nullptr)
            return nullptr;
        om->AllocateAndAssignInstanceID(go);
        om->AllocateAndAssignInstanceID(tc);
        go->SetName(name);
        go->addComponent(tc);
        go->SyncSerializedComponents();
        // What Level::CreateObject does for a live object: the component learns its
        // owner. SaveAsPrefabAsset walks child Transforms back to their GameObject
        // through it and refuses to write a partial prefab without it.
        tc->PostLoadResource(go);
        return go;
    }

    /// Reads a .zasset as FRESH objects (path-derived instance IDs of a path nothing
    /// else has loaded) and returns the PrefabAsset in it.
    PrefabAsset* ReadPrefabFresh(const std::filesystem::path& path)
    {
        std::vector<std::pair<int64_t, Object*>> graph;
        if (!GET_SYSTEM(AssetManager)->ReadObjectsFromBinary(path, graph))
            return nullptr;
        for (const auto& entry : graph)
        {
            if (entry.second != nullptr && entry.second->Is(TypeOf<PrefabAsset>()))
                return static_cast<PrefabAsset*>(entry.second);
        }
        return nullptr;
    }

    GameObject* FirstChild(GameObject* go)
    {
        Transform* tc = TransformOf(go);
        if (tc == nullptr || tc->GetChildCount() == 0)
            return nullptr;
        Transform* child = tc->GetChild(0);
        return child != nullptr ? child->GetParentObject() : nullptr;
    }

    const std::filesystem::path kPrefabPath = "prefab_e2e_source.zasset";
    const std::filesystem::path kAppliedPath = "prefab_e2e_applied.zasset";
    const std::filesystem::path kSceneBinaryPath = "prefab_e2e_scene.zasset";
    const std::filesystem::path kSceneYamlPath = "prefab_e2e_scene.scene";

    // Shared between scenarios (each depends on the previous one).
    PrefabInstance* g_Instance = nullptr;
    PrefabAsset* g_Source = nullptr;
}  // namespace

static void scenario_P1_save()
{
    std::fprintf(stderr, "[P1] author + SaveAsPrefabAsset\n");
    ObjectManager* om = GET_SYSTEM(ObjectManager);
    GameObject* root = MakeGameObject(om, "E2ERoot");
    GameObject* child = MakeGameObject(om, "E2EChild");
    Check(root != nullptr && child != nullptr, "P1", "failed to produce the authored GameObjects");
    if (root == nullptr || child == nullptr)
        return;

    TransformOf(root)->SetLocalPosition(Vector3(1.0f, 2.0f, 3.0f));
    TransformOf(root)->SetLocalScale(Vector3(1.0f, 1.0f, 1.0f));
    TransformOf(child)->SetParent(TransformOf(root), false);
    TransformOf(child)->SetLocalPosition(Vector3(0.0f, 5.0f, 0.0f));

    std::error_code ec;
    std::filesystem::remove(kPrefabPath, ec);
    Check(PrefabUtility::SaveAsPrefabAsset(root, kPrefabPath), "P1", "SaveAsPrefabAsset returned false");
    Check(std::filesystem::exists(kPrefabPath, ec) && std::filesystem::file_size(kPrefabPath, ec) > 0, "P1",
          "no .zasset on disk after SaveAsPrefabAsset");
}

static void scenario_P2_instantiate()
{
    std::fprintf(stderr, "[P2] InstantiateAsPrefab(path) from disk\n");
    g_Instance = PrefabUtilityEditor::InstantiateAsPrefab(kPrefabPath);
    Check(g_Instance != nullptr, "P2", "InstantiateAsPrefab returned null");
    if (g_Instance == nullptr)
        return;

    g_Source = g_Instance->GetSourcePrefab();
    Check(g_Source != nullptr, "P2", "instance has no source PrefabAsset");
    GameObject* inst_root = g_Instance->GetRootGameObject();
    Check(inst_root != nullptr, "P2", "instance has no root GameObject");
    if (g_Source == nullptr || inst_root == nullptr)
        return;

    GameObject* src_root = g_Source->GetRootGameObject();
    Check(src_root != nullptr && src_root != inst_root, "P2", "instance root must be a clone, not the source root");
    Check(inst_root->GetName() == eastl::string("E2ERoot"), "P2", "root name did not survive save + load + clone");
    Transform* inst_tc = TransformOf(inst_root);
    Check(inst_tc != nullptr, "P2", "cloned root has no Transform");
    if (inst_tc != nullptr)
    {
        const Vector3 p = inst_tc->GetLocalPosition();
        Check(Near(p.x, 1.0f) && Near(p.y, 2.0f) && Near(p.z, 3.0f), "P2", "root local position did not survive");
        Check(inst_tc->GetChildCount() == 1, "P2", "cloned root should have exactly one child");
    }
    GameObject* inst_child = FirstChild(inst_root);
    Check(inst_child != nullptr && inst_child->GetName() == eastl::string("E2EChild"), "P2",
          "child GameObject did not survive (name / hierarchy)");
    if (inst_child != nullptr && TransformOf(inst_child) != nullptr)
        Check(Near(TransformOf(inst_child)->GetLocalPosition().y, 5.0f), "P2", "child local position did not survive");

    // 2 GameObjects + 2 Transforms, each paired with its source counterpart.
    Check(g_Instance->GetCorrespondingSourceMap().size() >= 4, "P2", "corresponding-source map should cover 2 GOs + 2 Transforms");
    Check(g_Instance->FindSourceForInstance(PPtr<Object>(inst_root)).GetInstanceID() == src_root->GetInstanceID(), "P2",
          "corresponding-source map does not pair the instance root with the source root");
    Check(g_Instance->GetModifications().empty(), "P2", "a fresh instance must have no overrides");
}

static void scenario_P3_override_apply()
{
    std::fprintf(stderr, "[P3] override + ApplyAll\n");
    if (g_Instance == nullptr || g_Source == nullptr)
    {
        Check(false, "P3", "skipped: P2 failed");
        return;
    }
    Transform* inst_tc = TransformOf(g_Instance->GetRootGameObject());
    Transform* src_tc = TransformOf(g_Source->GetRootGameObject());
    if (inst_tc == nullptr || src_tc == nullptr)
    {
        Check(false, "P3", "missing Transform on instance / source root");
        return;
    }

    inst_tc->SetLocalScale(Vector3(2.5f, 1.0f, 1.0f));
    Check(Near(src_tc->GetLocalScale().x, 1.0f), "P3", "editing the instance must not touch the source before Apply");
    Check(PrefabUtilityEditor::RecordCurrentValueAsOverride(g_Instance, PPtr<Object>(inst_tc), "m_LocalScale.x"), "P3",
          "RecordCurrentValueAsOverride(m_LocalScale.x) failed");
    Check(g_Instance->GetModifications().size() == 1, "P3", "expected exactly one recorded override");

    // Recording the same (target, path) again overwrites instead of appending.
    inst_tc->SetLocalScale(Vector3(3.0f, 1.0f, 1.0f));
    PrefabUtilityEditor::RecordCurrentValueAsOverride(g_Instance, PPtr<Object>(inst_tc), "m_LocalScale.x");
    Check(g_Instance->GetModifications().size() == 1, "P3", "re-recording the same property must overwrite, not append");

    const int applied = PrefabUtilityEditor::ApplyAll(g_Instance);
    Check(applied == 1, "P3", "ApplyAll should report 1 applied modification");
    Check(g_Instance->GetModifications().empty(), "P3", "ApplyAll must clear the override list");
    Check(Near(src_tc->GetLocalScale().x, 3.0f), "P3", "ApplyAll did not write the override into the source");
    Check(Near(src_tc->GetLocalScale().y, 1.0f), "P3", "ApplyAll touched a field that was not overridden");
}

static void scenario_P4_persist()
{
    std::fprintf(stderr, "[P4] applied source persists through disk\n");
    if (g_Source == nullptr || g_Source->GetRootGameObject() == nullptr)
    {
        Check(false, "P4", "skipped: no source");
        return;
    }
    std::error_code ec;
    std::filesystem::remove(kAppliedPath, ec);
    Check(PrefabUtility::SaveAsPrefabAsset(g_Source->GetRootGameObject(), kAppliedPath), "P4",
          "saving the applied source failed");

    PrefabAsset* reloaded = ReadPrefabFresh(kAppliedPath);
    Check(reloaded != nullptr, "P4", "could not read the applied .zasset back");
    if (reloaded == nullptr)
        return;
    Check(reloaded != g_Source, "P4", "reload must produce fresh objects");
    GameObject* root = reloaded->GetRootGameObject();
    Check(root != nullptr && root->GetName() == eastl::string("E2ERoot"), "P4", "reloaded root missing / renamed");
    if (root != nullptr && TransformOf(root) != nullptr)
    {
        Check(Near(TransformOf(root)->GetLocalScale().x, 3.0f), "P4", "the applied override did not persist to disk");
        Check(Near(TransformOf(root)->GetLocalPosition().z, 3.0f), "P4", "untouched fields changed through the round trip");
        GameObject* child = FirstChild(root);
        Check(child != nullptr && child->GetName() == eastl::string("E2EChild"), "P4", "child lost through apply + save + reload");
    }
}

static void scenario_P5_revert()
{
    std::fprintf(stderr, "[P5] Revert\n");
    if (g_Instance == nullptr || g_Source == nullptr)
    {
        Check(false, "P5", "skipped: P2 failed");
        return;
    }
    Transform* inst_tc = TransformOf(g_Instance->GetRootGameObject());
    Transform* src_tc = TransformOf(g_Source->GetRootGameObject());
    if (inst_tc == nullptr || src_tc == nullptr)
        return;

    inst_tc->SetLocalScale(Vector3(3.0f, 7.0f, 1.0f));
    Check(PrefabUtilityEditor::RecordCurrentValueAsOverride(g_Instance, PPtr<Object>(inst_tc), "m_LocalScale.y"), "P5",
          "RecordCurrentValueAsOverride(m_LocalScale.y) failed");
    const int reverted = PrefabUtilityEditor::Revert(g_Instance);
    Check(reverted == 1, "P5", "Revert should report 1 reverted modification");
    Check(g_Instance->GetModifications().empty(), "P5", "Revert must clear the override list");
    Check(Near(inst_tc->GetLocalScale().y, 1.0f), "P5", "Revert did not restore the source value on the instance");
    Check(Near(src_tc->GetLocalScale().y, 1.0f), "P5", "Revert must not modify the source");
}

/// Writes the PrefabInstance + its cloned subtree as one scene graph and reads it back.
static void RoundTripInstance(const std::filesystem::path& path, bool yaml, const char* tag)
{
    Transform* inst_tc = TransformOf(g_Instance->GetRootGameObject());
    GameObject* inst_root = g_Instance->GetRootGameObject();
    GameObject* inst_child = FirstChild(inst_root);
    if (inst_tc == nullptr || inst_child == nullptr || TransformOf(inst_child) == nullptr)
    {
        Check(false, tag, "instance subtree incomplete");
        return;
    }

    // Scene layout: [PrefabInstance, root GO, root Transform, child GO, child Transform].
    std::vector<Object*> objects {g_Instance, inst_root, inst_tc, inst_child, TransformOf(inst_child)};
    std::vector<int64_t> ids {1, 2, 3, 4, 5};
    AssetManager* am = GET_SYSTEM(AssetManager);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    const bool wrote = yaml ? am->WriteObjectsToYaml(path, objects.data(), ids.data(), objects.size())
                            : am->WriteObjectsToDiskThreadSafe(path, objects.data(), ids.data(), objects.size());
    Check(wrote, tag, "writing the scene graph failed");
    if (!wrote)
        return;

    std::vector<std::pair<int64_t, Object*>> graph;
    const bool read = yaml ? am->ReadObjectsFromYaml(path, graph) : am->ReadObjectsFromBinary(path, graph);
    Check(read, tag, "reading the scene graph back failed");
    if (!read)
        return;

    PrefabInstance* back = nullptr;
    for (const auto& entry : graph)
    {
        if (entry.first == 1 && entry.second != nullptr && entry.second->Is(TypeOf<PrefabInstance>()))
            back = static_cast<PrefabInstance*>(entry.second);
    }
    Check(back != nullptr, tag, "PrefabInstance was not produced back from the scene file");
    if (back == nullptr)
        return;
    Check(back != g_Instance, tag, "read-back instance must be a fresh object");

    Check(back->GetModifications().size() == 1, tag, "override list did not survive");
    if (back->GetModifications().size() == 1)
    {
        const PropertyModification& m = back->GetModifications()[0];
        Check(m.property_path == eastl::string("m_LocalScale.z"), tag, "override property path did not survive");
    }

    GameObject* back_root = back->GetRootGameObject();
    Check(back_root != nullptr, tag, "instance root PPtr did not resolve inside the scene file");
    if (back_root != nullptr)
    {
        Check(back_root != inst_root, tag, "instance root must resolve to the read-back GameObject");
        Check(back_root->GetName() == eastl::string("E2ERoot"), tag, "instance root name did not survive");
    }

    Check(back->GetCorrespondingSourceMap().size() == g_Instance->GetCorrespondingSourceMap().size(), tag,
          "corresponding-source map size changed");
    // The in-file half of each pair must resolve to the read-back objects.
    bool root_pair_found = false;
    for (const PrefabInstance::CorrespondingSourceEntry& e : back->GetCorrespondingSourceMap())
    {
        if (back_root != nullptr && e.instance_object.GetInstanceID() == back_root->GetInstanceID())
            root_pair_found = true;
    }
    Check(root_pair_found, tag, "corresponding-source map no longer references the read-back root");

    // Cross-file half: the source prefab lives in a different file.
    Check(!back->GetSourcePrefab().IsNull(), tag, "source PrefabAsset reference was lost");
}

static void scenario_P6_instance_serialization()
{
    std::fprintf(stderr, "[P6] PrefabInstance round-trips through a scene file\n");
    if (g_Instance == nullptr)
    {
        Check(false, "P6", "skipped: P2 failed");
        return;
    }
    Transform* inst_tc = TransformOf(g_Instance->GetRootGameObject());
    if (inst_tc == nullptr)
        return;
    inst_tc->SetLocalScale(Vector3(3.0f, 1.0f, 4.0f));
    Check(PrefabUtilityEditor::RecordCurrentValueAsOverride(g_Instance, PPtr<Object>(inst_tc), "m_LocalScale.z"), "P6",
          "RecordCurrentValueAsOverride(m_LocalScale.z) failed");

    RoundTripInstance(kSceneBinaryPath, false, "P6/binary");
    RoundTripInstance(kSceneYamlPath, true, "P6/yaml");
}

int main()
{
    std::fprintf(stderr, "ZEngine prefab end-to-end smoke test\n====================================\n");

    RegisterCore();
    // Prefab load / save go through GET_SYSTEM(AssetManager); RegisterCore() alone
    // does not provide one (RegisterPlatform() does in the real app).
    REGISTER_SYSTEM_AS(RuntimeAssetManager, AssetManager);
    if (!SystemRegistry::GetInstance().InitializeAll(false) || GET_SYSTEM(AssetManager) == nullptr ||
        GET_SYSTEM(ObjectManager) == nullptr)
    {
        std::fprintf(stderr, "FATAL: system bring-up failed.\n");
        return 77;
    }

    scenario_P1_save();
    scenario_P2_instantiate();
    scenario_P3_override_apply();
    scenario_P4_persist();
    scenario_P5_revert();
    scenario_P6_instance_serialization();

    std::error_code ec;
    for (const auto& p : {kPrefabPath, kAppliedPath, kSceneBinaryPath, kSceneYamlPath})
        std::filesystem::remove(p, ec);

    SystemRegistry::GetInstance().ShutdownAll();

    std::fprintf(stderr, "\n%d checks, %d failures\n", g_Checks, g_Failures);
    if (g_Failures > 0)
    {
        std::fprintf(stderr, "Prefab end-to-end smoke test: FAILED\n");
        return 1;
    }
    std::fprintf(stderr, "Prefab end-to-end smoke test: ALL PASSED\n");
    return 0;
}
