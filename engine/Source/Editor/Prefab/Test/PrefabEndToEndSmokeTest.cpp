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
//   P7 the same through the real scene path: PrefabSceneBinding::PlaceInLevel
//      puts an instance into a Level, Level::save writes it next to the level's
//      GameObjects (via the Level serialization hooks), Level::LoadObjectGraph
//      on the file read back re-binds it to the NEW live GameObjects; Revert
//      still reaches the source prefab; a deleted root is not saved; Unload
//      drops the registration
//   P8 a PPtr into a multi-object file nothing has loaded yet (lazy deref of
//      a non-header object) hydrates the graph and returns THAT object -- it
//      used to return the file's header, of the wrong type
//   P9 one file, one identity: the same file named by a relative and by an
//      absolute path yields the same instance ID
//   P10 P7 with a YAML (.prefab) source prefab instead of a binary one: the
//      scene must still reach the source objects after a reload
//   P11 P8 for a YAML (.prefab) file: lazy deref of a non-header object of a
//      YAML graph nothing has loaded yet; LoadPrefabAsset afterwards reuses it
//   P12 a graph read twice puts a second object under the same path-derived ID;
//      destroying the first copy must not unregister the second
//   P14 runtime PrefabUtility::InstantiateFromPath honours its contract: every
//      call returns an independent tree (no object shared with another call or
//      with the cached template graph), for YAML and binary prefabs; the same
//      for a scene GameObject's m_DefinitionUrl expansion
//   P15 the content-browser drop path (PrefabSceneBinding::InstantiateIntoLevel):
//      dropping one prefab twice gives two independent linked instances, all
//      their GameObjects are level objects, both are saved
//   P16 packaging dependencies (BuildDependencyWalk + AssetRegistry): a YAML scene's
//      externals are indexed as dependencies (they were not: only binary
//      SerializedFile headers were read, so a packed .scene took nothing with it);
//      the player walk reads a scene's edges from its cooked copy, so a prefab only
//      a PrefabInstance referred to is not packed while a GameObject's is.
//      AssetRegistry::ScanAssets (was an empty stub) scans synchronously
//   P17 Inspector override model (PrefabOverrideModel): an edit records only the
//      leaves that differ from the source, editing back removes the override;
//      revert / apply one field; apply writes the source file with the same
//      fileIDs and refreshes the level's other instance (keeping its own
//      override); a zero position survives the leaf-by-leaf splice
//   P18 positional-stream symmetry: a MeshRenderer with sub-meshes instantiates
//      from a .prefab (it crashed: TransformTRS / Material probed legacy fields on
//      StreamedBinaryRead and misaligned the clone's stream); plus every
//      producible runtime type: write -> clone -> write gives the same bytes
//   P13 player cook (SceneCook): the editor-only PrefabInstance and the externals
//      only it used leave the cooked scene; the cooked scene loads with the
//      overridden values and binds no instance. Plus StripObjectsFromGraph on a
//      hand-written graph: external renumbering and dangling-reference nulling
//
// Standalone main(); links ZRuntime plus the Editor/Prefab TUs (which are not
// in libZRuntime). Run headless on the web build with:
//   scripts/verify_prefab_e2e.sh
//
// Exit codes: 0 all passed; 1 a check failed; 77 environment bring-up failed.
// =====================================================================

#include "Editor/AssetRegistry/AssetRegistry.h"
#include "Editor/Build/BuildDependencyWalk.h"
#include "Editor/Build/SceneCook.h"
#include "Editor/Prefab/PrefabInstance.h"
#include "Editor/Prefab/PrefabOverrideModel.h"
#include "Editor/Prefab/PrefabSceneBinding.h"
#include "Editor/Prefab/PrefabUtilityEditor.h"
#include "Editor/Undo/UndoSystem.h"
#include "Runtime/BaseClasses/Cast.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/YamlSerialize/YamlObjectGraph.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Serialize/TransferUtility.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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
    const std::filesystem::path kLevelYamlPath = "prefab_e2e_level.scene";
    const std::filesystem::path kLevelDeletedPath = "prefab_e2e_level_deleted.scene";
    const std::filesystem::path kLevelBinaryPath = "prefab_e2e_level.zasset";
    const std::filesystem::path kLazyPrefabPath = "prefab_e2e_lazy.zasset";
    const std::filesystem::path kYamlPrefabPath = "prefab_e2e_source.prefab";
    const std::filesystem::path kYamlLevelPath = "prefab_e2e_level_yamlsrc.scene";
    const std::filesystem::path kYamlLevelBinaryPath = "prefab_e2e_level_yamlsrc.zasset";
    const std::filesystem::path kLazyYamlPrefabPath = "prefab_e2e_lazy.prefab";
    const std::filesystem::path kCookSourcePath = "prefab_e2e_cook_source.scene";
    const std::filesystem::path kCookedPath = "cooked/prefab_e2e_cook_source.scene";

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

namespace
{
    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool InLevel(const Level& level, const GameObject* go)
    {
        return go != nullptr && level.GetGObjectByID(go->GetID()).lock().get() == go;
    }

    /// Level::save resolves its url through AssetManager; an absolute url bypasses
    /// the project-content lookup (there is no project in this test).
    eastl::string AbsoluteUrl(const std::filesystem::path& file)
    {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        return eastl::string((cwd / file).generic_string().c_str());
    }

    /// Reads a level file and builds a fresh Level from it the way Level::load does
    /// (minus the physics scene: the web build has no threads for Jolt's job system).
    bool LoadLevel(Level& level, const std::filesystem::path& file, bool yaml)
    {
        std::vector<std::pair<int64_t, Object*>> entries;
        AssetManager* am = GET_SYSTEM(AssetManager);
        const bool read = yaml ? am->ReadObjectsFromYaml(file, entries) : am->ReadObjectsFromBinary(file, entries);
        if (!read)
            return false;
        level.setLevelResUrl(AbsoluteUrl(file));
        return level.LoadObjectGraph(entries, false);
    }

    /// Checks on a level read back from a file the bound instance was saved into.
    void CheckReboundLevel(Level& level, PrefabInstance* original, const char* tag)
    {
        const std::vector<PrefabInstance*> bound = PrefabSceneBinding::GetInstances(level);
        Check(bound.size() == 1, tag, "exactly one PrefabInstance should be bound to the reloaded level");
        if (bound.size() != 1)
            return;
        PrefabInstance* back = bound[0];
        Check(back != original, tag, "the reloaded level must bind a fresh PrefabInstance");

        GameObject* root = back->GetRootGameObject();
        Check(root != nullptr, tag, "reloaded instance root does not resolve");
        Check(InLevel(level, root), tag, "reloaded instance root must be a LIVE object of the new level (retargeted)");
        if (root == nullptr)
            return;
        Check(root->GetName() == eastl::string("E2ERoot"), tag, "reloaded root name");
        GameObject* child = FirstChild(root);
        Check(child != nullptr && child->GetName() == eastl::string("E2EChild"), tag, "reloaded child missing");
        Check(InLevel(level, child), tag, "reloaded child must be a live object of the new level");
        Check(PrefabSceneBinding::FindInstanceForGameObject(level, child) == back, tag,
              "FindInstanceForGameObject(child) should find the bound instance");

        int gameobject_pairs = 0;
        bool all_live = true;
        for (const PrefabInstance::CorrespondingSourceEntry& e : back->GetCorrespondingSourceMap())
        {
            if (GameObject* go = Cast<GameObject>(static_cast<Object*>(e.instance_object)))
            {
                ++gameobject_pairs;
                all_live = all_live && InLevel(level, go);
            }
        }
        Check(gameobject_pairs == 2, tag, "source map should still pair 2 GameObjects");
        Check(all_live, tag, "every GameObject in the source map must be retargeted to the live level objects");
        Check(!back->GetSourcePrefab().IsNull(), tag, "source PrefabAsset reference lost");

        Transform* tc = TransformOf(root);
        Check(back->GetModifications().size() == 1, tag, "override list did not survive the level round trip");
        if (back->GetModifications().size() == 1 && tc != nullptr)
        {
            const PropertyModification& m = back->GetModifications()[0];
            Check(m.property_path == eastl::string("m_LocalScale.z"), tag, "override path");
            Check(m.target.GetInstanceID() == tc->GetInstanceID(), tag, "override target must be the live root Transform");
        }
        if (tc != nullptr)
        {
            Check(Near(tc->GetLocalScale().z, 4.0f), tag, "overridden value did not survive the level round trip");
            // The link to the source prefab still works after the reload.
            Check(PrefabUtilityEditor::Revert(back) == 1, tag, "Revert after reload should revert 1 override");
            Check(Near(tc->GetLocalScale().z, 1.0f), tag, "Revert after reload did not pull the source value");
        }
    }
}  // namespace

static void scenario_P7_level_save_load()
{
    std::fprintf(stderr, "[P7] PrefabInstance through Level::save / LoadObjectGraph\n");
    PrefabSceneBinding::Install();

    PrefabInstance* instance = PrefabUtilityEditor::InstantiateAsPrefab(kPrefabPath);
    Check(instance != nullptr, "P7", "InstantiateAsPrefab returned null");
    if (instance == nullptr)
        return;
    GameObject* clone_root = instance->GetRootGameObject();

    Level level;
    Check(PrefabSceneBinding::PlaceInLevel(level, instance), "P7", "PlaceInLevel failed");
    Check(level.getAllGObjects().size() == 2, "P7", "PlaceInLevel should add root + child to the level");
    GameObject* root = instance->GetRootGameObject();
    Check(root != nullptr && root != clone_root, "P7", "instance root must be retargeted from the clone to the live object");
    Check(InLevel(level, root), "P7", "instance root is not a live object of the level");
    Check(root != nullptr && root->GetInstanceID() != 0, "P7", "live GameObjects need an instance ID (Level::CreateObject)");
    Check(level.IsDirty(), "P7", "placing a prefab must dirty the level");
    if (!InLevel(level, root))
        return;
    GameObject* child = FirstChild(root);
    Check(InLevel(level, child), "P7", "the child Transform owner must be the live child GameObject");
    Check(PrefabSceneBinding::FindInstanceForGameObject(level, child) == instance, "P7",
          "FindInstanceForGameObject(child) should find the placed instance");

    Transform* tc = TransformOf(root);
    tc->SetLocalScale(Vector3(3.0f, 1.0f, 4.0f));
    Check(PrefabUtilityEditor::RecordCurrentValueAsOverride(instance, PPtr<Object>(tc), "m_LocalScale.z"), "P7",
          "RecordCurrentValueAsOverride(m_LocalScale.z) on the live root failed");

    // ---- YAML (.scene, the editor format) ----
    std::error_code ec;
    std::filesystem::remove(kLevelYamlPath, ec);
    level.setLevelResUrl(AbsoluteUrl(kLevelYamlPath));
    Check(level.save(), "P7/yaml", "Level::save failed");
    Check(!level.IsDirty(), "P7/yaml", "a successful save clears the dirty flag");
    Check(ReadText(kLevelYamlPath).find("PrefabInstance") != std::string::npos, "P7/yaml",
          "the scene file has no PrefabInstance (save hook did not collect it)");
    {
        Level reloaded;
        Check(LoadLevel(reloaded, kLevelYamlPath, true), "P7/yaml", "reading the level back failed");
        Check(reloaded.getAllGObjects().size() == 2, "P7/yaml", "reloaded level should have 2 GameObjects");
        CheckReboundLevel(reloaded, instance, "P7/yaml");

        reloaded.Unload();
        Check(PrefabSceneBinding::GetInstances(reloaded).empty(), "P7/unload", "Unload must drop the level's registrations");
    }

    // ---- binary (.zasset) ----
    std::filesystem::remove(kLevelBinaryPath, ec);
    level.setLevelResUrl(AbsoluteUrl(kLevelBinaryPath));
    Check(level.save(), "P7/binary", "Level::save (.zasset) failed");
    {
        Level reloaded;
        Check(LoadLevel(reloaded, kLevelBinaryPath, false), "P7/binary", "reading the binary level back failed");
        Check(reloaded.getAllGObjects().size() == 2, "P7/binary", "reloaded level should have 2 GameObjects");
        CheckReboundLevel(reloaded, instance, "P7/binary");
    }

    // ---- a deleted instance root is not saved (undo may still hold it) ----
    const GObjectID root_id = root->GetID();
    const std::shared_ptr<GameObject> held = level.GetGObjectByID(root_id).lock();
    level.DeleteGObjectByID(root_id);
    Check(PrefabSceneBinding::GetInstancesToSave(level).empty(), "P7/deleted",
          "an instance whose root left the level must not be saved");
    std::filesystem::remove(kLevelDeletedPath, ec);
    level.setLevelResUrl(AbsoluteUrl(kLevelDeletedPath));
    Check(level.save(), "P7/deleted", "Level::save after delete failed");
    Check(ReadText(kLevelDeletedPath).find("PrefabInstance") == std::string::npos, "P7/deleted",
          "a PrefabInstance with a deleted root was written anyway");
    Check(level.RestoreGObject(held), "P7/deleted", "RestoreGObject (undo) failed");
    Check(PrefabSceneBinding::GetInstancesToSave(level).size() == 1, "P7/deleted",
          "after undo the instance must be saved again");

    PrefabSceneBinding::Uninstall();
}

static void scenario_P8_lazy_non_header_pptr()
{
    std::fprintf(stderr, "[P8] lazy PPtr deref of a non-header object\n");
    if (g_Source == nullptr || g_Source->GetRootGameObject() == nullptr)
    {
        Check(false, "P8", "skipped: no source");
        return;
    }
    std::error_code ec;
    std::filesystem::remove(kLazyPrefabPath, ec);
    Check(PrefabUtility::SaveAsPrefabAsset(g_Source->GetRootGameObject(), kLazyPrefabPath), "P8", "save failed");

    AssetManager* am = GET_SYSTEM(AssetManager);
    ObjectManager* om = GET_SYSTEM(ObjectManager);
    const int32_t header_id = am->GetInstanceIDFromPathAndFileID(kLazyPrefabPath, 1);
    // SaveAsPrefabAsset numbers the objects 1..N: 1 = PrefabAsset, then GameObjects / Transforms.
    const int32_t object_id = am->GetInstanceIDFromPathAndFileID(kLazyPrefabPath, 2);
    const bool precondition = om->IDToPointer(header_id) == nullptr && om->IDToPointer(object_id) == nullptr;
    Check(precondition, "P8", "precondition: nothing of the new file may be loaded yet");
    if (!precondition)
        return;

    Object* object = static_cast<Object*>(PPtr<Object>(object_id));
    Check(object != nullptr, "P8", "lazy deref of fileID 2 returned null");
    if (object == nullptr)
        return;
    Check(object->GetInstanceID() == object_id, "P8", "lazy deref returned an object with a different instance ID");
    Check(!object->Is(TypeOf<PrefabAsset>()), "P8", "lazy deref of fileID 2 returned the file header (PrefabAsset)");
    Check(om->IDToPointer(header_id) != nullptr, "P8", "the rest of the graph should have been hydrated with it");
}

static void scenario_P9_one_file_one_identity()
{
    std::fprintf(stderr, "[P9] relative and absolute spellings share one identity\n");
    AssetManager* am = GET_SYSTEM(AssetManager);
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::current_path(ec) / kPrefabPath;
    const std::filesystem::path dotted = std::filesystem::path(".") / kPrefabPath;
    const int32_t rel_id = am->GetInstanceIDFromPathAndFileID(kPrefabPath, 3);
    Check(rel_id != 0, "P9", "no instance ID for the relative spelling");
    Check(am->GetInstanceIDFromPathAndFileID(absolute, 3) == rel_id, "P9", "absolute spelling minted a second identity");
    Check(am->GetInstanceIDFromPathAndFileID(dotted, 3) == rel_id, "P9", "./ spelling minted a second identity");
    // The path-derived GUID (YAML assets, header-less writes) must not depend on the spelling either.
    Check(AssetManager::DeriveDeterministicAssetGuid(kPrefabPath) == AssetManager::DeriveDeterministicAssetGuid(absolute),
          "P9", "relative and absolute spellings derive different GUIDs");
    Check(AssetManager::DeriveDeterministicAssetGuid(dotted) == AssetManager::DeriveDeterministicAssetGuid(absolute),
          "P9", "./ spelling derives a different GUID");
#if defined(_WIN32)
    std::string upper = absolute.generic_string();
    for (char& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    Check(am->GetInstanceIDFromPathAndFileID(upper, 3) == rel_id, "P9", "case-different spelling minted a second identity (NTFS)");
#endif
}

static void scenario_P10_yaml_source_prefab()
{
    std::fprintf(stderr, "[P10] YAML (.prefab) source prefab through Level::save / LoadObjectGraph\n");
    if (g_Source == nullptr || g_Source->GetRootGameObject() == nullptr)
    {
        Check(false, "P10", "skipped: no source");
        return;
    }
    std::error_code ec;
    std::filesystem::remove(kYamlPrefabPath, ec);
    Check(PrefabUtility::SaveAsPrefabAsset(g_Source->GetRootGameObject(), kYamlPrefabPath), "P10", "saving the .prefab failed");
    Check(ReadText(kYamlPrefabPath).find("PrefabAsset") != std::string::npos, "P10", "the .prefab is not a YAML graph");

    PrefabSceneBinding::Install();
    PrefabInstance* instance = PrefabUtilityEditor::InstantiateAsPrefab(kYamlPrefabPath);
    Check(instance != nullptr, "P10", "InstantiateAsPrefab(.prefab) returned null");
    if (instance == nullptr)
        return;

    Level level;
    Check(PrefabSceneBinding::PlaceInLevel(level, instance), "P10", "PlaceInLevel failed");
    Transform* tc = TransformOf(instance->GetRootGameObject());
    if (tc == nullptr)
    {
        Check(false, "P10", "placed root has no Transform");
        return;
    }
    tc->SetLocalScale(Vector3(3.0f, 1.0f, 4.0f));
    Check(PrefabUtilityEditor::RecordCurrentValueAsOverride(instance, PPtr<Object>(tc), "m_LocalScale.z"), "P10",
          "RecordCurrentValueAsOverride failed");

    const std::pair<const std::filesystem::path*, bool> formats[] = {{&kYamlLevelPath, true}, {&kYamlLevelBinaryPath, false}};
    for (const auto& format : formats)
    {
        const char* tag = format.second ? "P10/yaml" : "P10/binary";
        std::filesystem::remove(*format.first, ec);
        level.setLevelResUrl(AbsoluteUrl(*format.first));
        Check(level.save(), tag, "Level::save failed");
        Level reloaded;
        Check(LoadLevel(reloaded, *format.first, format.second), tag, "reading the level back failed");
        CheckReboundLevel(reloaded, instance, tag);
    }
    PrefabSceneBinding::Uninstall();
}

static void scenario_P11_lazy_yaml_non_header_pptr()
{
    std::fprintf(stderr, "[P11] lazy PPtr deref of a non-header object of a YAML graph\n");
    if (g_Source == nullptr || g_Source->GetRootGameObject() == nullptr)
    {
        Check(false, "P11", "skipped: no source");
        return;
    }
    std::error_code ec;
    std::filesystem::remove(kLazyYamlPrefabPath, ec);
    Check(PrefabUtility::SaveAsPrefabAsset(g_Source->GetRootGameObject(), kLazyYamlPrefabPath), "P11", "save failed");

    AssetManager* am = GET_SYSTEM(AssetManager);
    ObjectManager* om = GET_SYSTEM(ObjectManager);
    const int32_t header_id = am->GetInstanceIDFromPathAndFileID(kLazyYamlPrefabPath, 1);
    const int32_t object_id = am->GetInstanceIDFromPathAndFileID(kLazyYamlPrefabPath, 2);
    const bool precondition = om->IDToPointer(header_id) == nullptr && om->IDToPointer(object_id) == nullptr;
    Check(precondition, "P11", "precondition: nothing of the new file may be loaded yet");
    if (!precondition)
        return;

    Object* object = static_cast<Object*>(PPtr<Object>(object_id));
    Check(object != nullptr, "P11", "lazy deref of fileID 2 of a .prefab returned null");
    if (object == nullptr)
        return;
    Check(object->GetInstanceID() == object_id, "P11", "lazy deref returned an object with a different instance ID");
    Check(!object->Is(TypeOf<PrefabAsset>()), "P11", "lazy deref of fileID 2 returned the file header");
    Object* header = om->IDToPointer(header_id);
    Check(header != nullptr && header->Is(TypeOf<PrefabAsset>()), "P11",
          "the rest of the YAML graph should be loaded under its path-derived IDs");

    // Loading the prefab afterwards must reuse that graph, not produce a second copy
    // under the same IDs (which would silently re-point every PPtr into it).
    PrefabAsset* asset = PrefabUtility::LoadPrefabAsset(kLazyYamlPrefabPath);
    Check(asset != nullptr && static_cast<Object*>(asset) == header, "P11",
          "LoadPrefabAsset after the lazy load must return the already-loaded header");
    Check(om->IDToPointer(object_id) == object, "P11", "the lazily loaded object was replaced by a second copy");

    // ... and that graph is usable as a template although LoadPrefabAsset did not
    // read it (the lazy deref skipped the template preparation).
    PrefabInstance* instance = PrefabUtilityEditor::InstantiateAsPrefab(kLazyYamlPrefabPath);
    GameObject* inst_root = instance != nullptr ? static_cast<GameObject*>(instance->GetRootGameObject()) : nullptr;
    Check(inst_root != nullptr && TransformOf(inst_root) != nullptr, "P11",
          "instantiating the lazily loaded prefab gave no root / no Transform");
    Check(FirstChild(inst_root) != nullptr, "P11", "instantiating the lazily loaded prefab lost the child");
}

static void scenario_P12_duplicate_id_destroy()
{
    std::fprintf(stderr, "[P12] destroying a superseded copy keeps the live entry\n");
    AssetManager* am = GET_SYSTEM(AssetManager);
    ObjectManager* om = GET_SYSTEM(ObjectManager);
    const int32_t id = am->GetInstanceIDFromPathAndFileID("prefab_e2e_duplicate_probe.zasset", 1);
    Object* first = om->Produce(TypeOf<GameObject>(), id);
    Object* second = om->Produce(TypeOf<GameObject>(), id);  // what a second read of the file does
    Check(first != nullptr && second != nullptr && om->IDToPointer(id) == second, "P12",
          "the second Produce under the same ID should own the table entry");
    om->Destroy(first);
    Check(om->IDToPointer(id) == second, "P12", "destroying the superseded first copy unregistered the live second one");
    om->Destroy(second);
    Check(om->IDToPointer(id) == nullptr, "P12", "destroying the owner must unregister the ID");
}

static void scenario_P13_player_cook()
{
    std::fprintf(stderr, "[P13] player cook strips editor-only objects\n");
    Check(TypeOf<PrefabInstance>()->IsEditorOnly(), "P13", "PrefabInstance must be flagged editor-only (kTypeIsEditorOnly)");
    Check(!TypeOf<GameObject>()->IsEditorOnly() && !TypeOf<Transform>()->IsEditorOnly(), "P13",
          "runtime types must not be flagged editor-only");
    Check(SceneCook::IsEditorOnlyTypeName("PrefabInstance") && !SceneCook::IsEditorOnlyTypeName("GameObject") &&
              !SceneCook::IsEditorOnlyTypeName("NoSuchType"),
          "P13", "IsEditorOnlyTypeName: PrefabInstance yes, GameObject / unknown no");

    // ---- a scene with a placed, overridden instance of the YAML prefab ----
    PrefabSceneBinding::Install();
    PrefabInstance* instance = PrefabUtilityEditor::InstantiateAsPrefab(kYamlPrefabPath);
    Check(instance != nullptr, "P13", "InstantiateAsPrefab(.prefab) failed");
    if (instance == nullptr)
        return;
    Level level;
    PrefabSceneBinding::PlaceInLevel(level, instance);
    Transform* tc = TransformOf(instance->GetRootGameObject());
    if (tc == nullptr)
    {
        Check(false, "P13", "no root Transform");
        return;
    }
    tc->SetLocalScale(Vector3(3.0f, 1.0f, 6.0f));
    PrefabUtilityEditor::RecordCurrentValueAsOverride(instance, PPtr<Object>(tc), "m_LocalScale.z");
    std::error_code ec;
    std::filesystem::remove(kCookSourcePath, ec);
    level.setLevelResUrl(AbsoluteUrl(kCookSourcePath));
    Check(level.save(), "P13", "Level::save failed");
    const std::string source_text = ReadText(kCookSourcePath);
    Check(source_text.find("PrefabInstance") != std::string::npos && source_text.find(".prefab") != std::string::npos,
          "P13", "precondition: the editor scene carries the instance and the external to the .prefab");

    // ---- cook ----
    std::filesystem::remove_all("cooked", ec);
    const SceneCookResult cook = SceneCook::CookSceneFile(kCookSourcePath, kCookedPath);
    Check(cook.ok && cook.stripped, "P13", "CookSceneFile failed");
    Check(cook.removed_objects == 1, "P13", "exactly the one PrefabInstance should be removed");
    Check(cook.removed_externals >= 1, "P13", "the external to the source .prefab should be pruned");
    Check(cook.dangling_references == 0, "P13", "no player object refers to the PrefabInstance");
    const std::string cooked_text = ReadText(kCookedPath);
    Check(cooked_text.find("PrefabInstance") == std::string::npos, "P13", "the cooked scene still has a PrefabInstance");
    Check(cooked_text.find(".prefab") == std::string::npos, "P13", "the cooked scene still refers to the source .prefab");

    // ---- the cooked scene is a plain level: same objects, overridden values, no instance ----
    {
        Level reloaded;
        Check(LoadLevel(reloaded, kCookedPath, true), "P13", "the cooked scene does not load");
        Check(reloaded.getAllGObjects().size() == 2, "P13", "the cooked scene lost GameObjects");
        Check(PrefabSceneBinding::GetInstances(reloaded).empty(), "P13", "the cooked scene still binds a PrefabInstance");
        bool found_root = false;
        for (const auto& pair : reloaded.getAllGObjects())
        {
            GameObject* go = pair.second.get();
            if (go != nullptr && go->GetName() == eastl::string("E2ERoot"))
            {
                found_root = true;
                Transform* root_tc = TransformOf(go);
                Check(root_tc != nullptr && Near(root_tc->GetLocalScale().z, 6.0f), "P13",
                      "the overridden value did not survive the cook");
                Check(FirstChild(go) != nullptr, "P13", "the cooked root lost its child");
            }
        }
        Check(found_root, "P13", "the cooked scene has no E2ERoot");
    }
    PrefabSceneBinding::Uninstall();

    // ---- StripObjectsFromGraph on a hand-written graph ----
    const char* graph =
        "externals:\n"
        "  - guid: aaa\n    type: PrefabAsset\n    path: only_dropped_uses_me.prefab\n"
        "  - guid: bbb\n    type: MaterialRes\n    path: kept_uses_me.mat\n"
        "objects:\n"
        "  - fileID: 1\n    type: Keep\n    data:\n      Keep:\n"
        "        mat:\n          m_FileID: 2\n          m_PathID: 1\n"
        "        partner:\n          m_FileID: 0\n          m_PathID: 2\n"
        "  - fileID: 2\n    type: Drop\n    data:\n      Drop:\n"
        "        src:\n          m_FileID: 1\n          m_PathID: 1\n";
    eastl::string out;
    ZYaml::StripGraphStats stats;
    const bool stripped = ZYaml::StripObjectsFromGraph(
        graph, [](const char* type_name) { return std::string(type_name) == "Drop"; }, out, &stats);
    Check(stripped, "P13/strip", "StripObjectsFromGraph failed on a valid graph");
    Check(stats.removedObjects == 1 && stats.removedExternals == 1 && stats.danglingReferences == 1, "P13/strip",
          "stats: 1 object, 1 external, 1 dangling reference expected");
    const std::string text(out.c_str());
    Check(text.find("only_dropped_uses_me") == std::string::npos && text.find("kept_uses_me.mat") != std::string::npos,
          "P13/strip", "the external only the dropped object used must go, the other must stay");
    Check(text.find("type: Drop") == std::string::npos && text.find("type: Keep") != std::string::npos, "P13/strip",
          "the Drop object must go, the Keep object must stay");
    // The kept external moved from index 2 to 1; the reference to the dropped local object is null.
    const size_t mat = text.find("mat:");
    const size_t partner = text.find("partner:");
    Check(mat != std::string::npos && text.find("m_FileID: 1", mat) < partner, "P13/strip",
          "the kept object's external reference was not renumbered 2 -> 1");
    Check(partner != std::string::npos && text.find("m_PathID: 0", partner) != std::string::npos, "P13/strip",
          "the reference to the dropped local object was not nulled");
}

namespace
{
    std::vector<Object*> SubtreeObjects(GameObject* root)
    {
        std::vector<Object*> out;
        std::vector<GameObject*> pending {root};
        while (!pending.empty())
        {
            GameObject* go = pending.back();
            pending.pop_back();
            if (go == nullptr)
                continue;
            out.push_back(go);
            for (ImmediatePtr<Component>& c : go->getComponents())
            {
                if (Component* component = c)
                    out.push_back(component);
            }
            if (Transform* tc = TransformOf(go))
            {
                for (uint32_t i = 0; i < tc->GetChildCount(); ++i)
                {
                    Transform* child = tc->GetChild(i);
                    if (child != nullptr)
                        pending.push_back(child->GetParentObject());
                }
            }
        }
        return out;
    }

    bool Disjoint(const std::vector<Object*>& a, const std::vector<Object*>& b)
    {
        for (Object* x : a)
            for (Object* y : b)
                if (x == y)
                    return false;
        return true;
    }
}  // namespace

static void scenario_P14_runtime_instantiate_independent()
{
    std::fprintf(stderr, "[P14] InstantiateFromPath returns independent trees\n");
    const std::pair<const std::filesystem::path*, const char*> prefabs[] = {{&kYamlPrefabPath, "P14/yaml"},
                                                                           {&kPrefabPath, "P14/binary"}};
    for (const auto& prefab : prefabs)
    {
        const char* tag = prefab.second;
        PrefabAsset* template_asset = PrefabUtility::LoadPrefabAsset(*prefab.first);
        GameObject* a = PrefabUtility::InstantiateFromPath(*prefab.first);
        GameObject* b = PrefabUtility::InstantiateFromPath(*prefab.first);
        Check(a != nullptr && b != nullptr, tag, "InstantiateFromPath returned null");
        if (a == nullptr || b == nullptr || template_asset == nullptr)
            continue;
        const std::vector<Object*> objects_a = SubtreeObjects(a);
        const std::vector<Object*> objects_b = SubtreeObjects(b);
        const std::vector<Object*> objects_t = SubtreeObjects(template_asset->GetRootGameObject());
        Check(objects_a.size() == 4 && objects_b.size() == 4, tag, "each instance should be 2 GameObjects + 2 Transforms");
        Check(Disjoint(objects_a, objects_b), tag, "two InstantiateFromPath calls share objects");
        Check(Disjoint(objects_a, objects_t) && Disjoint(objects_b, objects_t), tag,
              "InstantiateFromPath returned (part of) the cached template graph");
        if (TransformOf(a) != nullptr && TransformOf(b) != nullptr)
        {
            TransformOf(a)->SetLocalScale(Vector3(9.0f, 9.0f, 9.0f));
            Check(!Near(TransformOf(b)->GetLocalScale().x, 9.0f), tag, "editing one instance changed the other");
            Check(Near(TransformOf(a)->GetLocalPosition().z, 3.0f), tag, "instance lost the prefab's values");
        }
        GameObject* child_a = FirstChild(a);
        Check(child_a != nullptr && TransformOf(child_a) != nullptr && TransformOf(child_a)->GetParentObject() == child_a,
              tag, "instance child Transform owner is not the instance child");
    }

    // m_DefinitionUrl expansion (GameObject::ExpandDefinition) merges the prefab
    // root's components into the scene object: two scene objects must not end up
    // sharing one component instance.
    GameObject* first = PrefabUtility::InstantiateFromPath(kYamlPrefabPath, false);
    GameObject* second = PrefabUtility::InstantiateFromPath(kYamlPrefabPath, false);
    Check(first != nullptr && second != nullptr && TransformOf(first) != TransformOf(second), "P14/definition",
          "definition expansion (invoke_post_load=false) hands two callers the same components");
}

static void scenario_P15_drop_twice()
{
    std::fprintf(stderr, "[P15] dropping one prefab twice\n");
    PrefabSceneBinding::Install();
    Level level;
    PrefabInstance* first = PrefabSceneBinding::InstantiateIntoLevel(level, kYamlPrefabPath);
    PrefabInstance* second = PrefabSceneBinding::InstantiateIntoLevel(level, kYamlPrefabPath);
    Check(first != nullptr && second != nullptr && first != second, "P15", "two drops should give two instances");
    if (first == nullptr || second == nullptr)
        return;
    Check(level.getAllGObjects().size() == 4, "P15", "two drops of a 2-GameObject prefab should add 4 level objects");
    GameObject* root_a = first->GetRootGameObject();
    GameObject* root_b = second->GetRootGameObject();
    Check(root_a != nullptr && root_b != nullptr && root_a != root_b, "P15", "the instances share a root");
    Check(InLevel(level, root_a) && InLevel(level, root_b) && InLevel(level, FirstChild(root_a)) &&
              InLevel(level, FirstChild(root_b)),
          "P15", "every GameObject of both instances must be a level object");
    if (TransformOf(root_a) != nullptr && TransformOf(root_b) != nullptr)
    {
        Check(TransformOf(root_a) != TransformOf(root_b), "P15", "the instances share a Transform");
        const float before = TransformOf(root_b)->GetLocalScale().x;
        TransformOf(root_a)->SetLocalScale(Vector3(before + 2.0f, 1.0f, 1.0f));
        Check(Near(TransformOf(root_b)->GetLocalScale().x, before), "P15", "editing one dropped instance changed the other");
    }
    Check(PrefabSceneBinding::GetInstancesToSave(level).size() == 2, "P15", "both dropped instances must be saved");
    Check(PrefabSceneBinding::InstantiateIntoLevel(level, "no_such_prefab.prefab") == nullptr && level.getAllGObjects().size() == 4,
          "P15", "dropping a missing prefab must fail without touching the level");
    PrefabSceneBinding::Uninstall();
}

namespace
{
    bool Contains(const std::vector<std::string>& v, const std::string& x)
    {
        return std::find(v.begin(), v.end(), x) != v.end();
    }
}  // namespace

static void scenario_P16_build_dependencies()
{
    std::fprintf(stderr, "[P16] packaging dependencies: YAML scene edges, player walk through the cooked copy\n");
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    const std::filesystem::path content = cwd / "p16_assets";
    const std::filesystem::path cooked_root = cwd / "p16_cooked";
    std::filesystem::remove_all(content, ec);
    std::filesystem::remove_all(cooked_root, ec);
    std::filesystem::create_directories(content, ec);

    // Two prefabs in the scan root: one binary, one YAML.
    ObjectManager* om = GET_SYSTEM(ObjectManager);
    GameObject* hero = MakeGameObject(om, "P16Hero");
    GameObject* villain = MakeGameObject(om, "P16Villain");
    Check(hero != nullptr && PrefabUtility::SaveAsPrefabAsset(hero, content / "hero.zasset"), "P16",
          "saving hero.zasset failed");
    Check(villain != nullptr && PrefabUtility::SaveAsPrefabAsset(villain, content / "villain.prefab"), "P16",
          "saving villain.prefab failed");

    // A real editor scene: one linked instance of each (the only references to them
    // are the PrefabInstance objects -- the GameObjects are concrete copies).
    PrefabSceneBinding::Install();
    {
        Level level;
        Check(PrefabSceneBinding::InstantiateIntoLevel(level, content / "hero.zasset") != nullptr &&
                  PrefabSceneBinding::InstantiateIntoLevel(level, content / "villain.prefab") != nullptr,
              "P16", "placing the two prefabs failed");
        level.setLevelResUrl(eastl::string((content / "level.scene").generic_string().c_str()));
        Check(level.save(), "P16", "Level::save failed");
    }
    PrefabSceneBinding::Uninstall();

    // A hand-written scene where a player object (type Keep) uses hero and an
    // editor-only PrefabInstance uses villain. Paths are registry-key spellings.
    {
        std::ofstream out(content / "keep.scene", std::ios::binary);
        out << "externals:\n"
               "  - guid: \"\"\n    type: PrefabAsset\n    path: hero.zasset\n"
               "  - guid: \"\"\n    type: PrefabAsset\n    path: villain.prefab\n"
               "objects:\n"
               "  - fileID: 1\n    type: Keep\n    data:\n      Keep:\n"
               "        asset:\n          m_FileID: 1\n          m_PathID: 1\n"
               "  - fileID: 2\n    type: PrefabInstance\n    data:\n      PrefabInstance:\n"
               "        m_SourcePrefab:\n          m_FileID: 2\n          m_PathID: 1\n";
    }

    AssetRegistry registry;
    registry.ScanAssets(content);
    Check(registry.GetAssetIndex("hero.zasset").has_value() && registry.GetAssetIndex("villain.prefab").has_value() &&
              registry.GetAssetIndex("level.scene").has_value() && registry.GetAssetIndex("keep.scene").has_value(),
          "P16", "ScanAssets did not index the four assets (synchronous scan)");

    // ---- the registry's (editor) edges ----
    const std::vector<std::string> level_deps = registry.GetDependencies("level.scene");
    Check(Contains(level_deps, "hero.zasset"), "P16", "YAML scene -> binary prefab edge missing from the registry");
    Check(Contains(level_deps, "villain.prefab"), "P16", "YAML scene -> YAML prefab edge missing from the registry");
    const std::vector<std::string> keep_deps = registry.GetDependencies("keep.scene");
    Check(Contains(keep_deps, "hero.zasset") && Contains(keep_deps, "villain.prefab"), "P16",
          "hand-written scene: both externals should be registry edges (path spelling)");
    Check(registry.GetReferencers("hero.zasset").size() == 2, "P16", "hero.zasset should be referenced by both scenes");

    // ---- the player walk ----
    const BuildDependencyWalk::DependencyFn player = [&](const std::string& key) {
        return BuildDependencyWalk::PlayerDependencies(registry, key, content, cooked_root);
    };
    const BuildDependencyWalk::DependencyFn editor = [&](const std::string& key) { return registry.GetDependencies(key); };

    const std::vector<std::string> level_player = BuildDependencyWalk::WalkFrom("level.scene", player);
    Check(level_player.size() == 1 && level_player[0] == "level.scene", "P16",
          "a scene whose prefabs are only referenced by PrefabInstances packs only itself");
    Check(std::filesystem::exists(cooked_root / "level.scene", ec), "P16", "the walk did not cook the scene");
    Check(BuildDependencyWalk::WalkFrom("level.scene", editor).size() == 3, "P16",
          "precondition: the editor edges reach scene + 2 prefabs");

    const std::vector<std::string> keep_player = BuildDependencyWalk::WalkFrom("keep.scene", player);
    Check(Contains(keep_player, "keep.scene") && Contains(keep_player, "hero.zasset"), "P16",
          "a prefab a player object uses must be packed");
    Check(!Contains(keep_player, "villain.prefab"), "P16", "a prefab only a PrefabInstance used must not be packed");

    // Fresh cooked copy: a second walk does not re-cook.
    bool cooked_now = true;
    BuildDependencyWalk::CookSceneIfStale(content / "keep.scene", cooked_root / "keep.scene", &cooked_now);
    Check(!cooked_now, "P16", "an up-to-date cooked scene was cooked again");

    // No cooked root (e.g. no project Intermediate/): fall back to the source edges.
    const std::vector<std::string> no_cook = BuildDependencyWalk::WalkFrom(
        "keep.scene", [&](const std::string& key) { return BuildDependencyWalk::PlayerDependencies(registry, key, content, {}); });
    Check(no_cook.size() == 3, "P16", "without a cooked root the walk uses the source scene's edges");

    std::filesystem::remove_all(content, ec);
    std::filesystem::remove_all(cooked_root, ec);
}

namespace
{
    std::vector<std::string> FileIDsOf(const std::filesystem::path& file)
    {
        std::vector<std::string> ids;
        std::istringstream in(ReadText(file));
        std::string line;
        while (std::getline(in, line))
        {
            const size_t at = line.find("- fileID:");
            if (at != std::string::npos)
                ids.push_back(line.substr(at));
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    Transform* SourceTransformOf(PrefabInstance* instance, Transform* instance_tc)
    {
        return Cast<Transform>(static_cast<Object*>(instance->FindSourceForInstance(PPtr<Object>(instance_tc))));
    }
}  // namespace

static void scenario_P17_inspector_override_model()
{
    std::fprintf(stderr, "[P17] Inspector override model: sync / revert / apply + save + propagate\n");
    namespace M = PrefabOverrideModel;
    std::error_code ec;
    const std::filesystem::path prefab = "p17_source.prefab";
    const std::filesystem::path copy = "p17_copy.prefab";
    std::filesystem::remove(prefab, ec);
    std::filesystem::remove(copy, ec);

    ObjectManager* om = GET_SYSTEM(ObjectManager);
    GameObject* root = MakeGameObject(om, "P17Root");
    GameObject* child = MakeGameObject(om, "P17Child");
    if (root == nullptr || child == nullptr)
    {
        Check(false, "P17", "authoring failed");
        return;
    }
    TransformOf(root)->SetLocalPosition(Vector3(1.0f, 2.0f, 3.0f));
    TransformOf(child)->SetParent(TransformOf(root), false);
    Check(PrefabUtility::SaveAsPrefabAsset(root, prefab), "P17", "saving the source prefab failed");
    const std::vector<std::string> ids_before = FileIDsOf(prefab);

    PrefabSceneBinding::Install();
    Level level;
    PrefabInstance* a = PrefabSceneBinding::InstantiateIntoLevel(level, prefab);
    PrefabInstance* b = PrefabSceneBinding::InstantiateIntoLevel(level, prefab);
    if (a == nullptr || b == nullptr)
    {
        Check(false, "P17", "placing two instances failed");
        PrefabSceneBinding::Uninstall();
        return;
    }
    GameObject* a_root = a->GetRootGameObject();
    GameObject* b_root = b->GetRootGameObject();
    Transform* ta = TransformOf(a_root);
    Transform* tb = TransformOf(b_root);
    Check(M::FindInstance(&level, a_root) == a && M::FindInstance(&level, FirstChild(b_root)) == b, "P17",
          "FindInstance should map a root / a child to its instance");
    Check(M::PathCovers("m_LocalScale", "m_LocalScale.x") && M::PathCovers("m_A", "m_A[2].b") &&
              !M::PathCovers("m_LocalScale", "m_LocalScaleX") && !M::PathCovers("m_LocalScale.x", "m_LocalScale"),
          "P17", "PathCovers");

    // ---- implicit override: only the leaf that differs ----
    ta->SetLocalScale(Vector3(2.0f, 1.0f, 1.0f));
    Check(M::SyncFieldOverride(a, ta, "m_LocalScale") == 1, "P17/sync", "editing scale.x should give 1 override");
    Check(a->GetModifications().size() == 1 && a->GetModifications()[0].property_path == eastl::string("m_LocalScale.x"),
          "P17/sync", "the override should be the leaf m_LocalScale.x");
    Check(M::IsOverridden(a, ta, "m_LocalScale") && M::IsOverridden(a, ta, "m_LocalScale.x") &&
              !M::IsOverridden(a, ta, "m_LocalScale.y") && !M::IsOverridden(b, tb, "m_LocalScale"),
          "P17/sync", "IsOverridden: compound + leaf yes, other leaf / other instance no");
    Check(M::SyncFieldOverride(a, ta, "m_LocalScale") == 1 && a->GetModifications().size() == 1, "P17/sync",
          "syncing again must not duplicate the override");
    const std::vector<M::OverrideRow> rows = M::ListOverrides(a);
    Check(rows.size() == 1 && rows[0].label.find("P17Root") != eastl::string::npos, "P17/sync", "ListOverrides label");
    ta->SetLocalScale(Vector3(1.0f, 1.0f, 1.0f));
    Check(M::SyncFieldOverride(a, ta, "m_LocalScale") == 0 && a->GetModifications().empty(), "P17/sync",
          "editing back to the source value removes the override");
    Check(M::SyncFieldOverride(a, ta, "m_NoSuchField") == -1, "P17/sync", "an unknown path reports -1");

    // ---- the override list is undoable with the edit ----
    {
        UndoSystem undo;
        undo.BeginTransaction("Set Scale");
        undo.AddRecord(M::MakeOverrideListUndoRecord(a));
        ta->SetLocalScale(Vector3(3.0f, 1.0f, 1.0f));
        M::SyncFieldOverride(a, ta, "m_LocalScale");
        undo.EndTransaction();
        Check(a->GetModifications().size() == 1 && undo.GetUndoCount() == 1, "P17/undo", "precondition: 1 override, 1 step");
        Check(undo.Undo() && a->GetModifications().empty(), "P17/undo", "undo must restore the empty override list");
        Check(undo.Redo() && a->GetModifications().size() == 1, "P17/undo", "redo must bring the override back");
        undo.BeginTransaction("No-op");
        undo.AddRecord(M::MakeOverrideListUndoRecord(a));
        undo.EndTransaction();
        Check(undo.GetUndoCount() == 1, "P17/undo", "an unchanged override list must not make an undo step");
        Check(M::OverrideListUndoKey(a) != 0 && M::OverrideListUndoKey(a) != M::OverrideListUndoKey(b), "P17/undo",
              "per-instance coalesce key");
        ta->SetLocalScale(Vector3(1.0f, 1.0f, 1.0f));
        M::SyncFieldOverride(a, ta, "m_LocalScale");
    }

    // ---- revert one field, keep the other ----
    ta->SetLocalScale(Vector3(1.0f, 4.0f, 1.0f));
    M::SyncFieldOverride(a, ta, "m_LocalScale");
    ta->SetLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
    const int pos_overrides = M::SyncFieldOverride(a, ta, "m_LocalPositionD");
    Check(pos_overrides == 3, "P17/position", "a zero position from (1,2,3) is 3 leaf overrides");
    Check(M::RevertField(a, ta, "m_LocalPositionD") == 3, "P17/revert", "reverting the position reverts its 3 leaves");
    Check(Near(ta->GetLocalPosition().x, 1.0f) && Near(ta->GetLocalPosition().z, 3.0f), "P17/revert",
          "revert did not restore the source position");
    Check(Near(ta->GetLocalScale().y, 4.0f) && a->GetModifications().size() == 1, "P17/revert",
          "reverting the position must not touch the scale override");

    // ---- b has its own override; apply a's position to the source ----
    tb->SetLocalScale(Vector3(1.0f, 1.0f, 7.0f));
    M::SyncFieldOverride(b, tb, "m_LocalScale");
    ta->SetLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
    M::SyncFieldOverride(a, ta, "m_LocalPositionD");
    const M::ApplyReport applied = M::ApplyField(&level, a, ta, "m_LocalPositionD");
    Check(applied.applied == 3 && applied.saved && applied.merged == 1, "P17/apply",
          "apply position: 3 applied, source saved, the other instance merged");
    Transform* src_tc = SourceTransformOf(a, ta);
    // Zero is the interesting value: leaves are spliced one at a time, and Transform's
    // positional read used to fall back to the (stale) legacy float copy whenever the
    // double position came out all-zero.
    Check(src_tc != nullptr && Near(src_tc->GetLocalPosition().x, 0.0f) && Near(src_tc->GetLocalPosition().y, 0.0f) &&
              Near(src_tc->GetLocalPosition().z, 0.0f),
          "P17/apply", "the source position must be the applied zero (not the legacy copy's old value)");
    Check(a->GetModifications().size() == 1 && Near(ta->GetLocalScale().y, 4.0f), "P17/apply",
          "applying one field keeps the instance's other override");
    Check(Near(tb->GetLocalPosition().x, 0.0f) && Near(tb->GetLocalPosition().z, 0.0f), "P17/apply",
          "the other instance did not pick up the applied value");
    Check(Near(tb->GetLocalScale().z, 7.0f) && b->GetModifications().size() == 1, "P17/apply",
          "the other instance lost its own override in the merge");

    // ---- on disk: same fileIDs, applied value ----
    Check(FileIDsOf(prefab) == ids_before, "P17/save", "saving the source renumbered its fileIDs");
    std::filesystem::copy_file(prefab, copy, std::filesystem::copy_options::overwrite_existing, ec);
    {
        std::vector<GameObject*> gos;
        PrefabAsset* fresh = PrefabUtility::LoadPrefabAsset(copy, &gos);
        Transform* fresh_tc = (fresh != nullptr) ? TransformOf(fresh->GetRootGameObject()) : nullptr;
        Check(fresh_tc != nullptr && Near(fresh_tc->GetLocalPosition().x, 0.0f) && Near(fresh_tc->GetLocalPosition().z, 0.0f),
              "P17/save", "the file on disk does not have the applied position");
        Check(gos.size() == 2, "P17/save", "the saved prefab lost a GameObject");
    }

    // ---- apply everything that is left ----
    const M::ApplyReport all = M::ApplyAll(&level, a);
    Check(all.applied == 1 && all.saved && a->GetModifications().empty(), "P17/applyall", "ApplyAll: the scale override");
    Check(src_tc != nullptr && Near(src_tc->GetLocalScale().y, 4.0f), "P17/applyall", "the source scale was not applied");
    Check(M::ApplyAll(&level, a).applied == 0, "P17/applyall", "nothing left to apply");

    PrefabSceneBinding::Uninstall();
    std::filesystem::remove(prefab, ec);
    std::filesystem::remove(copy, ec);
}

static void scenario_P18_positional_stream_symmetry()
{
    std::fprintf(stderr, "[P18] positional stream symmetry (prefab clone of a MeshRenderer, every type)\n");
    std::error_code ec;
    const std::filesystem::path prefab = "p18_mesh.prefab";
    std::filesystem::remove(prefab, ec);

    // ---- the reported crash: a prefab with a MeshRenderer ----
    ObjectManager* om = GET_SYSTEM(ObjectManager);
    GameObject* go = MakeGameObject(om, "P18Sky");
    auto* renderer = static_cast<MeshRenderer*>(om->Produce(TypeOf<MeshRenderer>(), 0));
    if (go == nullptr || renderer == nullptr)
    {
        Check(false, "P18", "authoring failed");
        return;
    }
    om->AllocateAndAssignInstanceID(renderer);
    SubMeshRes sub;
    sub.m_MeshAsset = "SkyDome.obj";
    sub.m_Transform = TransformTRS(Vector3(1.0f, 2.0f, 3.0f), Quaternion::IDENTITY, Vector3(4.0f, 5.0f, 6.0f));
    renderer->SetSubMeshes({sub, sub});
    renderer->SetSharedMaterialAssets({"Sky.mat", "Other.mat"});
    go->addComponent(renderer);
    go->SyncSerializedComponents();
    renderer->PostLoadResource(go);
    Check(PrefabUtility::SaveAsPrefabAsset(go, prefab), "P18", "saving the MeshRenderer prefab failed");

    PrefabInstance* instance = PrefabUtilityEditor::InstantiateAsPrefab(prefab);
    Check(instance != nullptr, "P18", "InstantiateAsPrefab of a MeshRenderer prefab failed");
    if (instance != nullptr)
    {
        GameObject* root = instance->GetRootGameObject();
        MeshRenderer* clone = root != nullptr ? root->tryGetComponent(MeshRenderer) : nullptr;
        Check(clone != nullptr, "P18", "the clone has no MeshRenderer");
        if (clone != nullptr)
        {
            const auto& subs = clone->getSubMeshes();
            const auto& mats = clone->getSharedMaterialAssets();
            Check(subs.size() == 2 && mats.size() == 2, "P18", "clone: 2 sub-meshes, 2 materials");
            if (subs.size() == 2)
            {
                Check(subs[1].m_MeshAsset == eastl::string("SkyDome.obj"), "P18", "clone: sub-mesh asset");
                Check(Near(static_cast<float>(subs[1].m_Transform.m_Position.y), 2.0f) &&
                          Near(subs[1].m_Transform.m_Scale.z, 6.0f),
                      "P18", "clone: sub-mesh transform");
            }
            Check(mats.size() == 2 && mats[1] == eastl::string("Other.mat"), "P18", "clone: material list");
        }
    }
    std::filesystem::remove(prefab, ec);

    // ---- every producible runtime type: a positional read takes exactly the bytes
    //      the writer emitted (write -> read into a fresh object -> write again) ----
    RuntimeTypeArray& types = TypeManager::GetInstance().GetRuntimeTypes();
    int checked = 0;
    for (uint32_t i = 0; i < types.count; ++i)
    {
        const Type* type = types.types[i];
        if (type == nullptr || type->isAbstract || type->GetFactory() == nullptr)
            continue;
        Object* a = om->Produce(type, 0);
        Object* b = om->Produce(type, 0);
        if (a == nullptr || b == nullptr)
            continue;
        std::vector<uint8_t> first;
        TransferUtility::WriteObjectToVector(*a, first);
        std::vector<uint8_t> copy = first;
        TransferUtility::ReadObjectFromVector(*b, copy);
        std::vector<uint8_t> second;
        TransferUtility::WriteObjectToVector(*b, second);
        if (first != second)
        {
            std::fprintf(stderr, "    asymmetric: %s (%zu -> %zu bytes)\n", type->GetName(), first.size(), second.size());
            Check(false, "P18/types", "write -> positional read -> write changed the bytes");
        }
        ++checked;
        om->Destroy(a);
        om->Destroy(b);
    }
    std::fprintf(stderr, "    %d types round-tripped\n", checked);
    Check(checked > 20, "P18/types", "expected to round-trip the registered runtime types");
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
    scenario_P7_level_save_load();
    scenario_P8_lazy_non_header_pptr();
    scenario_P9_one_file_one_identity();
    scenario_P10_yaml_source_prefab();
    scenario_P11_lazy_yaml_non_header_pptr();
    scenario_P12_duplicate_id_destroy();
    scenario_P13_player_cook();
    scenario_P14_runtime_instantiate_independent();
    scenario_P15_drop_twice();
    scenario_P16_build_dependencies();
    scenario_P17_inspector_override_model();
    scenario_P18_positional_stream_symmetry();

    std::error_code ec;
    for (const auto& p : {kPrefabPath, kAppliedPath, kSceneBinaryPath, kSceneYamlPath, kLevelYamlPath, kLevelDeletedPath,
                          kLevelBinaryPath, kLazyPrefabPath, kYamlPrefabPath, kYamlLevelPath,
                          kYamlLevelBinaryPath, kLazyYamlPrefabPath, kCookSourcePath})
        std::filesystem::remove(p, ec);

    std::filesystem::remove_all("cooked", ec);
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
