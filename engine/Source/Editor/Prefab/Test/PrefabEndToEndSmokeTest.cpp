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
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/SerializationCaching/MemoryCacheReader.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/RuntimeAssetManager.h"
#include "Runtime/Resource/Prefab/PrefabAsset.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"
#include "Runtime/Resource/Prefab/PropertyModification.h"
#include "Runtime/Resource/Prefab/PropertyPathResolver.h"
#include "Runtime/Resource/Prefab/PropertyValueLocator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

// ---- P18 helpers: field-VALUE symmetry ------------------------------------------------
namespace
{
    struct ScalarLeaf
    {
        std::string path;   ///< dotted field path from the object root (no array descent)
        std::string type;   ///< TypeTree type name ("float", "bool", "int", ...)
        size_t offset = 0;
        size_t size = 0;
    };

    /// Every fixed-size scalar field reachable from `node` WITHOUT entering an array.
    /// (A default-constructed object has empty arrays, so there is nothing to patch in
    /// there; and the locator cannot index variable-size elements anyway.)
    void CollectScalarLeaves(const TypeTreeIterator& node, const std::string& prefix, std::vector<ScalarLeaf>& out)
    {
        for (TypeTreeIterator c = node.Children(); !c.IsNull(); c = c.Next())
        {
            if (c->IsArray())
                continue;
            const std::string path = prefix.empty() ? std::string(c.Name().c_str()) : prefix + "." + c.Name().c_str();
            if (!c.Children().IsNull())
            {
                CollectScalarLeaves(c, path, out);
                continue;
            }
            const int32_t sz = c->m_ByteSize;
            if (sz == 1 || sz == 2 || sz == 4 || sz == 8)
            {
                ScalarLeaf leaf;
                leaf.path = path;
                leaf.type = c.Type().c_str();
                leaf.size = static_cast<size_t>(sz);
                out.push_back(std::move(leaf));
            }
        }
    }

    /// A distinct, harmless, non-zero value for leaf number `index`: floats / doubles
    /// stay finite (no NaN -- a NaN would compare unequal to itself in some readers),
    /// bools stay 0/1, integers stay small so an enum-ish field is not driven wildly
    /// out of range.
    void FillLeafValue(const ScalarLeaf& leaf, size_t index, uint8_t* dst)
    {
        const int64_t v = static_cast<int64_t>(index) + 2;  // never 0 or 1: 1 is also what a bool becomes
        if (leaf.type == "float" && leaf.size == 4)
        {
            const float f = static_cast<float>(v) * 1.5f;
            std::memcpy(dst, &f, 4);
        }
        else if (leaf.type == "double" && leaf.size == 8)
        {
            const double d = static_cast<double>(v) * 1.5;
            std::memcpy(dst, &d, 8);
        }
        else if (leaf.type == "bool" && leaf.size == 1)
        {
            dst[0] = 1;
        }
        else
        {
            const uint64_t u = static_cast<uint64_t>(v);
            std::memcpy(dst, &u, leaf.size);  // little-endian host: low bytes first
        }
    }

    /// A position is serialised twice: a double `<name>D.<axis>` and a legacy float copy
    /// `<name>.<axis>` that the WRITER regenerates from the double (Transform.cpp and
    /// TransformTRS.h: `legacy = m_Position.ToVector3()`). For a double leaf of that
    /// shape this returns the path of its float partner, else "". The shape is checked
    /// structurally (a `D` suffix on the parent segment, double/float widths) instead
    /// of by type name, so a new TRS-bearing struct is covered without a list to update.
    std::string DerivedFloatPartnerOfDouble(const ScalarLeaf& leaf)
    {
        if (leaf.type != "double" || leaf.size != 8)
            return "";
        const size_t axis_dot = leaf.path.rfind('.');
        if (axis_dot == std::string::npos)
            return "";
        const std::string parent = leaf.path.substr(0, axis_dot);   // e.g. "global_transform.m_positionD"
        if (parent.empty() || parent.back() != 'D')
            return "";
        return parent.substr(0, parent.size() - 1) + leaf.path.substr(axis_dot);  // "....m_position.x"
    }

    /// The float side of that pair: its double partner exists in `all`, so the planted
    /// float is overwritten on write.
    bool IsDerivedFloatCopy(const ScalarLeaf& leaf, const std::vector<ScalarLeaf>& all)
    {
        if (leaf.type != "float" || leaf.size != 4)
            return false;
        const size_t axis_dot = leaf.path.rfind('.');
        if (axis_dot == std::string::npos)
            return false;
        const std::string double_path = leaf.path.substr(0, axis_dot) + "D" + leaf.path.substr(axis_dot);
        for (const ScalarLeaf& other : all)
            if (other.path == double_path && other.type == "double")
                return true;
        return false;
    }

    /// True for a leaf we must leave alone: object references. PPtr / ImmediatePtr
    /// serialise as a struct {m_FileID, m_PathID} whose members are plain ints, so the
    /// TYPE name of the leaf does not say "pointer" -- the PARENT path does. (The first
    /// version tested only the leaf's type name and reported six `m_FileID` mismatches
    /// that were the reader resolving a planted id, not a misread field.)
    bool IsReferenceLeaf(const ScalarLeaf& leaf)
    {
        if (leaf.type.find("Ptr") != std::string::npos)
            return true;
        const size_t dot = leaf.path.rfind('.');
        const std::string last = dot == std::string::npos ? leaf.path : leaf.path.substr(dot + 1);
        return last == "m_FileID" || last == "m_PathID";
    }
}  // namespace

// ---- P18 helpers: WHOLE-STREAM synthesis (dynamic content + wide / edge values) ---------
//
// The per-leaf pass above plants ONE small integer into ONE fixed-size scalar of a
// default object. Two things it cannot reach:
//
//  * Dynamic content. A default object has empty arrays and empty strings, so a reader
//    that reverses a vector, truncates a string, drops the last element or mis-sizes an
//    element has nothing to get wrong.
//  * Value range. (idx+2)*1.5f and small ints exercise "wrong field" but not "wrong
//    width": a reader that keeps only the low 16 bits of an int32, clears a float's low
//    mantissa byte, or canonicalises -0.0 to +0.0 reads 3.0f / 5 back perfectly.
//
// So build a complete, populated stream straight from the type's TypeTree (the layout
// rules are WalkPastNode's: fixed-size nodes are flat, arrays are [int32 count][elements],
// kAlignBytesFlag pads to 4 on the ABSOLUTE stream position), read it positionally into a
// fresh object, write it back, and demand byte-for-byte equality.
namespace
{
    enum class SynthMode
    {
        Wide,  ///< every byte of every scalar non-zero and distinct (sign bits and high bytes set)
        Edge,  ///< -0.0 / denormal / max-finite floats and doubles, INT_MIN / INT_MAX / -1 / 1 integers
    };

    struct SynthSpan
    {
        std::string path;
        std::string type;
        size_t offset = 0;
        size_t size = 0;
    };

    struct StreamSynth
    {
        SynthMode mode = SynthMode::Wide;
        std::vector<uint8_t> bytes;
        std::vector<SynthSpan> spans;  ///< every scalar leaf in stream order
        size_t counter = 0;
        int arrays = 0;
        int strings = 0;
        int maps = 0;
        int elements = 0;  ///< array elements actually emitted
        int pair_index = -1;  ///< set just before descending into element i of a {first, second} array
        int key_index = -1;   ///< >= 0 while synthesizing the `first` (key) of element i of such an array
        bool unsupported = false;  ///< a node shape the synthesizer does not understand
        std::string reason;        ///< why, for the log
    };

    void SynthUnsupported(StreamSynth& s, const std::string& why)
    {
        if (!s.unsupported)
            s.reason = why;
        s.unsupported = true;
    }

    // Non-zero byte `i` of scalar number `k`. The period in k is 251 (prime) -- an earlier
    // `0x11 * (((k*3+i) % 15) + 1)` repeated every 5 scalars, so the two keys of a
    // two-element map came out byte-identical and the map legitimately collapsed them to
    // one entry (a generator defect that looked like an engine bug).
    uint8_t SynthByte(size_t k, size_t i) { return static_cast<uint8_t>((k * 7919u + i * 104729u + 13u) % 251u + 1u); }

    uint32_t SynthFloatBits(SynthMode mode, size_t k)
    {
        if (mode == SynthMode::Edge)
        {
            static const uint32_t kEdge[4] = {0x80000000u /* -0.0 */, 0x0000007Fu /* denormal */, 0x7E967699u /* 3e38 */,
                                              0xFF7FFFFFu /* -FLT_MAX */};
            return kEdge[k % 4];
        }
        const uint32_t sign = static_cast<uint32_t>(k & 1);
        const uint32_t exponent = 0x70u + static_cast<uint32_t>(k % 0x20);  // finite, well inside the range
        const uint32_t mantissa = ((0x2A5A5Bu ^ static_cast<uint32_t>(k * 0x010203u)) & 0x7FFFFFu) | 0x010101u;  // every byte non-zero
        return (sign << 31) | (exponent << 23) | mantissa;
    }

    uint64_t SynthDoubleBits(SynthMode mode, size_t k)
    {
        if (mode == SynthMode::Edge)
        {
            static const uint64_t kEdge[4] = {0x8000000000000000ull /* -0.0 */, 0x0000000000000001ull /* denormal */,
                                              0x7FEFFFFFFFFFFFFFull /* DBL_MAX */, 0xFFEFFFFFFFFFFFFFull /* -DBL_MAX */};
            return kEdge[k % 4];
        }
        uint64_t u = 0;
        for (size_t i = 0; i < 8; ++i)
            u |= static_cast<uint64_t>(SynthByte(k, i)) << (8 * i);
        return (u & 0x000FFFFFFFFFFFFFull) | (static_cast<uint64_t>(0x3E0 + k % 0x20) << 52) | (static_cast<uint64_t>(k & 1) << 63);
    }

    void SynthPush(StreamSynth& s, const void* data, size_t size)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        s.bytes.insert(s.bytes.end(), p, p + size);
    }

    void SynthLeaf(StreamSynth& s, const std::string& path, const std::string& type, size_t size)
    {
        const size_t k = s.counter++;
        uint8_t buf[16] = {};
        ScalarLeaf probe;
        probe.path = path;
        probe.type = type;
        probe.size = size;
        if (size > sizeof(buf))
        {
            SynthUnsupported(s, "scalar leaf '" + path + "' is " + std::to_string(size) + " bytes wide");
            return;
        }
        if (IsReferenceLeaf(probe))
        {
            // zeros: a planted id is something the reader may try to resolve
        }
        else if (s.key_index >= 0 && type == "char" && size == 1)
        {
            // a map key that is a string: element i is "a"*len, "b"*len, ... -- lexicographically increasing
            buf[0] = static_cast<uint8_t>('a' + s.key_index % 26);
        }
        else if (s.key_index >= 0 && type != "float" && type != "double" && type != "bool")
        {
            // a map key that is an integer: positive in BOTH the signed and unsigned reading and
            // strictly increasing with the element index. A std::map re-sorts on write, so a key
            // list that is not already in order would "change" for a perfectly good reader.
            const uint64_t v = 0x11223344ull + static_cast<uint64_t>(s.key_index) * 0x01010101ull;
            std::memcpy(buf, &v, size < 8 ? size : 8);
        }
        else if (type == "bool" && size == 1)
        {
            buf[0] = 1;
        }
        else if (type == "char" && size == 1)
        {
            buf[0] = static_cast<uint8_t>('a' + k % 26);
        }
        else if (type == "float" && size == 4)
        {
            const uint32_t u = SynthFloatBits(s.mode, k);
            std::memcpy(buf, &u, 4);
        }
        else if (type == "double" && size == 8)
        {
            const uint64_t u = SynthDoubleBits(s.mode, k);
            std::memcpy(buf, &u, 8);
        }
        else if (s.mode == SynthMode::Wide)
        {
            for (size_t i = 0; i < size; ++i)
                buf[i] = SynthByte(k, i);
        }
        else
        {
            switch (k % 4)
            {
                case 0: buf[size - 1] = 0x80; break;                                   // INT_MIN
                case 1: std::memset(buf, 0xFF, size); buf[size - 1] = 0x7F; break;      // INT_MAX
                case 2: std::memset(buf, 0xFF, size); break;                            // -1 / UINT_MAX
                default: buf[0] = 1; break;                                             // 1
            }
        }
        SynthSpan span;
        span.path = path;
        span.type = type;
        span.offset = s.bytes.size();
        span.size = size;
        s.spans.push_back(span);
        SynthPush(s, buf, size);
    }

    void SynthNode(const TypeTreeIterator& node, const std::string& path, StreamSynth& s, int depth)
    {
        const TypeTreeNode* n = node.operator->();
        const int pair_index = s.pair_index;  // belongs to THIS element only
        s.pair_index = -1;
        // Each array costs two levels (the array node and its element), so a vector of
        // structs holding a vector of structs holding a string is already 6 deep.
        if (n == nullptr || depth > 16 || s.bytes.size() > (1u << 20))
        {
            SynthUnsupported(s, n == nullptr ? "null TypeTree node at '" + path + "'"
                                             : "nesting deeper than 16 (or stream over 1 MiB) at '" + path + "'");
            return;
        }
        const bool any_child_align = (n->m_MetaFlag & kAnyChildUsesAlignBytesFlag) != 0;
        // Visit the children of a struct-like node. The child named `first` of a {first, second}
        // array element is a map key, which must come out in ascending order (see SynthLeaf).
        auto visit_children = [&](const TypeTreeIterator& parent) {
            for (TypeTreeIterator c = parent.Children(); !c.IsNull(); c = c.Next())
            {
                const bool is_key = pair_index >= 0 && c.Name() == "first";
                if (is_key)
                    s.key_index = pair_index;
                SynthNode(c, path.empty() ? std::string(c.Name().c_str()) : path + "." + c.Name().c_str(), s, depth + 1);
                if (is_key)
                    s.key_index = -1;
            }
        };

        if (n->m_ByteSize != -1 && !any_child_align)
        {
            if (node.Children().IsNull())
            {
                SynthLeaf(s, path, node.Type().c_str(), static_cast<size_t>(n->m_ByteSize));
            }
            else
            {
                const size_t before = s.bytes.size();
                visit_children(node);
                if (s.bytes.size() - before != static_cast<size_t>(n->m_ByteSize))
                    SynthUnsupported(s, "children of '" + path + "' add up to " + std::to_string(s.bytes.size() - before) +
                                            " bytes but the node says " + std::to_string(n->m_ByteSize));
            }
        }
        else if (n->IsArray())
        {
            TypeTreeIterator size_child = node.Children();
            TypeTreeIterator data_child = size_child.IsNull() ? size_child : size_child.Next();
            if (data_child.IsNull() || (n->m_TypeFlags & TypeTreeNode::IsArrayOfRefs) != 0)
            {
                // Arrays of managed references need a type table to be meaningful; leave them empty.
                const int32_t zero = 0;
                SynthPush(s, &zero, sizeof(zero));
            }
            else
            {
                const bool is_string = data_child.Children().IsNull() && data_child.Type() == "char";
                TypeTreeIterator first_child = data_child.Children();
                const bool is_pair = !first_child.IsNull() && first_child.Name() == "first" && !first_child.Next().IsNull() &&
                                     first_child.Next().Name() == "second";
                // A {first, second} array is a map. Maps get ONE element: the TypeTree does not say
                // ordered or unordered, and a std::unordered_map reads two elements back in an order
                // of its own (MemoryProfiler's CSharp.data did exactly that: element 1 came out
                // first, every byte intact). So element ORDER of a multi-element map is not
                // checkable here -- multi-element handling is covered by the plain vectors.
                const int32_t count = is_pair ? 1 : (is_string ? static_cast<int32_t>(3 + s.counter % 3) : 2);
                SynthPush(s, &count, sizeof(count));
                // Count only arrays that really got elements. Counting visited arrays let a
                // generator that emitted every array EMPTY keep the premise check green
                // (found by mutating the generator: `count = 0` survived).
                if (count > 0)
                {
                    ++s.arrays;
                    s.elements += count;
                    if (is_string)
                        ++s.strings;
                    if (is_pair)
                        ++s.maps;
                }
                for (int32_t i = 0; i < count; ++i)
                {
                    s.pair_index = is_pair ? i : -1;
                    SynthNode(data_child, path + "[" + std::to_string(i) + "]", s, depth + 1);
                }
            }
        }
        else
        {
            if (node.Children().IsNull())
                SynthUnsupported(s, "'" + path + "' (" + std::string(node.Type().c_str()) +
                                        ") is variable-size, not an array and has no children");
            visit_children(node);
        }

        if ((n->m_MetaFlag & kAlignBytesFlag) != 0)
            while (s.bytes.size() % 4 != 0)
                s.bytes.push_back(0);
    }

    /// The writer regenerates a position's float copy from its double (see
    /// DerivedFloatPartnerOfDouble). A stream that disagrees with itself there cannot
    /// round-trip, so make the synthesized one consistent.
    void SynthMakeDerivedConsistent(StreamSynth& s)
    {
        for (const SynthSpan& d : s.spans)
        {
            ScalarLeaf probe;
            probe.path = d.path;
            probe.type = d.type;
            probe.size = d.size;
            const std::string partner = DerivedFloatPartnerOfDouble(probe);
            if (partner.empty())
                continue;
            for (const SynthSpan& f : s.spans)
            {
                if (f.path == partner && f.size == 4 && f.type == "float")
                {
                    double v = 0;
                    std::memcpy(&v, s.bytes.data() + d.offset, sizeof(v));
                    const float fv = static_cast<float>(v);
                    std::memcpy(s.bytes.data() + f.offset, &fv, sizeof(fv));
                    break;
                }
            }
        }
    }

    std::string HexRun(const std::vector<uint8_t>& b, size_t from, size_t count)
    {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (size_t i = from; i < from + count && i < b.size(); ++i)
        {
            out += kHex[b[i] >> 4];
            out += kHex[b[i] & 15];
            out += ' ';
        }
        return out;
    }
}  // namespace

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
    int value_types_checked = 0;
    size_t value_leaves_checked = 0;
    int derived_leaves = 0;
    std::vector<std::string> derived_names;
    int synth_runs = 0;
    int synth_arrays = 0;
    int synth_strings = 0;
    int synth_maps = 0;
    int synth_elements = 0;
    int synth_unsupported = 0;
    size_t synth_scalars = 0;
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

        // Positional read, done by hand so the CONSUMED byte count is observable
        // (ReadObjectFromVector throws it away). A reader that skips, repeats or adds
        // one field consumes a different number of bytes than the writer emitted --
        // whatever the values are. Comparing only write -> read -> write misses that
        // for a default-constructed object: its payload is almost all zeros, so a
        // stream shifted by one byte still reads (and re-writes) the same zeros.
        // (Found by mutation: a reader consuming one extra byte survived the old check.)
        std::vector<uint8_t> copy = first;
        // Positional read of `bytes` into `target`; returns the bytes it consumed.
        auto ReadPositional = [](Object& target, std::vector<uint8_t>& bytes) -> size_t {
            if (bytes.empty())
                return 0;
            MemoryCacheReader memory_cache(bytes);
            StreamedBinaryRead read_stream;
            CachedReader& read_cache = read_stream.Init(kNoTransferInstructionFlags);
            read_cache.InitRead(memory_cache, /*position=*/0, /*readSize=*/bytes.size());
            target.VirtualRedirectTransfer(read_stream);
            return read_cache.End();
        };
        const size_t consumed = ReadPositional(*b, copy);
        std::vector<uint8_t> second;
        TransferUtility::WriteObjectToVector(*b, second);
        if (consumed != first.size() && !first.empty())
        {
            std::fprintf(stderr, "    misaligned: %s (wrote %zu bytes, positional read consumed %zu)\n", type->GetName(),
                         first.size(), consumed);
            Check(false, "P18/types", "the positional reader must consume exactly the bytes the writer emitted");
        }
        if (first != second)
        {
            std::fprintf(stderr, "    asymmetric: %s (%zu -> %zu bytes)\n", type->GetName(), first.size(), second.size());
            Check(false, "P18/types", "write -> positional read -> write changed the bytes");
        }

        // ---- field VALUES: does each field come back from the field it was written to? ----
        // The two checks above cannot see a reader that consumes the right NUMBER of bytes
        // but puts them in the wrong field, drops one, or widens/narrows it: a default
        // object is nearly all zeros, so every such mistake re-writes the same zeros.
        // So overwrite every fixed-size scalar with a distinct non-zero value, read that
        // stream back, write it again, and demand the bytes are exactly what we planted.
        // (Skips object references -- a planted InstanceID is something the reader may
        // try to resolve -- and array contents, which are empty on a default object.)
        {
            prefab::SerialisedObject tree;
            std::vector<ScalarLeaf> leaves;
            if (prefab::SerialiseObject(a, tree) && !tree.type_tree.Root().IsNull())
                CollectScalarLeaves(tree.type_tree.Root(), "", leaves);

            std::vector<uint8_t> planted = first;
            std::vector<ScalarLeaf> placed;
            for (ScalarLeaf& leaf : leaves)
            {
                if (IsReferenceLeaf(leaf))
                    continue;
                prefab::PropertyLocation loc;
                if (!prefab::LocatePropertyInStream(tree.type_tree, first, eastl::string(leaf.path.c_str()), loc) ||
                    !loc.valid() || static_cast<size_t>(loc.byte_size) != leaf.size ||
                    loc.byte_offset + leaf.size > planted.size())
                    continue;
                leaf.offset = loc.byte_offset;
                FillLeafValue(leaf, placed.size(), planted.data() + leaf.offset);
                placed.push_back(leaf);
            }

            if (!placed.empty() && first != second)
            {
                // The DEFAULT object already fails the symmetry check above. Then every
                // planted leaf would "fail" too, at the same bytes, and the report would
                // be N identical lines hiding the one real defect. Attribute it once
                // (the asymmetric line above) and skip the value pass for this type.
                std::fprintf(stderr, "    (value pass skipped for %s: its default object is not symmetric)\n", type->GetName());
            }
            else if (!placed.empty())
            {
                ++value_types_checked;
                value_leaves_checked += placed.size();

                // Plant ONE leaf at a time. Planting all of them together cannot tell a
                // misread field from a field the object legitimately re-derives on write
                // (Transform / TransformTRS write the float position copy FROM the
                // double: m_LocalPosition = m_LocalPositionD.ToVector3()). Alone, the two
                // look different:
                //   * a correct leaf comes back byte-for-byte;
                //   * a DERIVED leaf comes back as something else but nothing ELSE moves;
                //   * a MISREAD leaf (wrong field / wrong width / dropped) changes bytes
                //     somewhere it was not planted, or changes the stream length.
                for (size_t li = 0; li < placed.size(); ++li)
                {
                    const ScalarLeaf& leaf = placed[li];
                    std::vector<uint8_t> one = first;
                    FillLeafValue(leaf, li, one.data() + leaf.offset);
                    std::vector<uint8_t> one_in = one;

                    Object* c = om->Produce(type, 0);
                    if (c == nullptr)
                        continue;
                    ReadPositional(*c, one_in);
                    std::vector<uint8_t> echoed;
                    TransferUtility::WriteObjectToVector(*c, echoed);
                    om->Destroy(c);

                    // What SHOULD the stream look like after read -> write? Exactly what we
                    // planted -- except for the one derivation the engine really does:
                    // a position is stored twice, a double (m_positionD / m_LocalPositionD)
                    // and a legacy float copy, and the writer always regenerates the float
                    // from the double. So:
                    //   * plant the DOUBLE   -> the float copy next to it becomes float(double);
                    //   * plant the FLOAT    -> discarded: the double (still default) wins.
                    // Everything else must come back bit for bit. (A first version allowed a
                    // budget of "leaves rewritten on write" instead; that would also have
                    // waved through a reader that silently drops a field.)
                    std::vector<uint8_t> expected = one;
                    const std::string partner = DerivedFloatPartnerOfDouble(leaf);
                    bool derived_rule_applies = false;
                    if (!partner.empty())
                    {
                        for (const ScalarLeaf& other : placed)
                        {
                            if (other.path == partner && other.size == 4)
                            {
                                double d = 0;
                                std::memcpy(&d, one.data() + leaf.offset, sizeof(d));
                                const float f = static_cast<float>(d);
                                std::memcpy(expected.data() + other.offset, &f, sizeof(f));
                                derived_rule_applies = true;
                                break;
                            }
                        }
                    }
                    else if (IsDerivedFloatCopy(leaf, placed))
                    {
                        expected = first;  // the planted float is thrown away
                        derived_rule_applies = true;
                    }

                    if (echoed == expected)
                    {
                        if (derived_rule_applies)
                            ++derived_leaves;
                        continue;
                    }

                    std::string why = "stream length changed";
                    if (echoed.size() == expected.size())
                    {
                        why = "bytes differ at";
                        for (size_t k = 0; k < expected.size() && why.size() < 60; ++k)
                            if (echoed[k] != expected[k])
                                why += " +" + std::to_string(k);
                    }
                    std::fprintf(stderr, "    value mismatch: %s field %s (%s, %zu bytes at +%zu)%s: %s\n", type->GetName(),
                                 leaf.path.c_str(), leaf.type.c_str(), leaf.size, leaf.offset,
                                 derived_rule_applies ? " [derived-pair rule]" : "", why.c_str());
                    Check(false, "P18/values", "a planted field value did not come back from the field it was written to");
                }
            }
        }
        // ---- WHOLE-STREAM pass: dynamic content + wide / edge values ----
        // Only meaningful when the default object is itself symmetric (otherwise it is
        // already reported once above and this would just repeat the same defect).
        if (first != second || first.empty())
        {
            // Visible, not silent: a type that never reaches the whole-stream pass is a hole.
            std::fprintf(stderr, "    (whole-stream pass not run for %s: %s)\n", type->GetName(),
                         first.empty() ? "its default stream is empty" : "its default object is not symmetric");
        }
        if (first == second && !first.empty())
        {
            prefab::SerialisedObject full_tree;
            if (prefab::SerialiseObject(a, full_tree) && !full_tree.type_tree.Root().IsNull())
            {
                for (const SynthMode mode : {SynthMode::Wide, SynthMode::Edge})
                {
                    const char* mode_name = mode == SynthMode::Wide ? "wide" : "edge";
                    StreamSynth synth;
                    synth.mode = mode;
                    SynthNode(full_tree.type_tree.Root(), "", synth, 0);
                    if (synth.unsupported)
                    {
                        ++synth_unsupported;
                        std::fprintf(stderr, "    (whole-stream %s pass skipped for %s: %s)\n", mode_name, type->GetName(),
                                     synth.reason.c_str());
                        continue;
                    }
                    SynthMakeDerivedConsistent(synth);
                    if (synth.bytes.empty())
                        continue;

                    Object* c = om->Produce(type, 0);
                    if (c == nullptr)
                        continue;
                    std::vector<uint8_t> in = synth.bytes;
                    const size_t used = ReadPositional(*c, in);
                    std::vector<uint8_t> out;
                    TransferUtility::WriteObjectToVector(*c, out);
                    om->Destroy(c);

                    ++synth_runs;
                    synth_arrays += synth.arrays;
                    synth_strings += synth.strings;
                    synth_maps += synth.maps;
                    synth_elements += synth.elements;
                    synth_scalars += synth.spans.size();

                    if (used == synth.bytes.size() && out == synth.bytes)
                        continue;

                    // Localize: the first byte that differs, mapped back to the field it belongs to.
                    std::string where = "stream length " + std::to_string(synth.bytes.size()) + " -> " + std::to_string(out.size());
                    size_t first_diff = 0;
                    while (first_diff < synth.bytes.size() && first_diff < out.size() && synth.bytes[first_diff] == out[first_diff])
                        ++first_diff;
                    for (const SynthSpan& span : synth.spans)
                    {
                        if (first_diff >= span.offset && first_diff < span.offset + span.size)
                        {
                            where = "field " + span.path + " (" + span.type + ", " + std::to_string(span.size) + " bytes at +" +
                                    std::to_string(span.offset) + ")";
                            break;
                        }
                    }
                    if (where.rfind("stream length", 0) == 0 && out.size() == synth.bytes.size())
                        where = "bytes at +" + std::to_string(first_diff) + " (inside a length prefix or padding)";
                    std::fprintf(stderr, "    synth mismatch [%s]: %s %s; consumed %zu of %zu; wrote %s; planted %s\n", mode_name,
                                 type->GetName(), where.c_str(), used, synth.bytes.size(), HexRun(out, first_diff, 8).c_str(),
                                 HexRun(synth.bytes, first_diff, 8).c_str());
                    Check(false, "P18/synth", "a populated stream (arrays, strings, wide or edge values) did not round-trip");
                }
            }
        }
        ++checked;
        om->Destroy(a);
        om->Destroy(b);
    }
    std::fprintf(stderr, "    %d types round-tripped; field values planted in %d types (%zu scalar fields, %d via the double->float position rule)\n",
                 checked, value_types_checked, value_leaves_checked, derived_leaves);
    std::fprintf(stderr, "    whole-stream: %d populated round-trips (%d non-empty arrays / %d elements, incl. %d strings and %d maps; %zu scalars), %d types unsupported\n",
                 synth_runs, synth_arrays, synth_elements, synth_strings, synth_maps, synth_scalars, synth_unsupported);
    Check(checked > 20, "P18/types", "expected to round-trip the registered runtime types");
    // Premise of the value check: it must actually have had something to plant, or every
    // mutation of it "passes" vacuously (a mutation survived once exactly like that).
    Check(value_types_checked >= 3 && value_leaves_checked >= 6, "P18/values",
          "the value check planted almost nothing -- its premise (scalar leaves reachable) did not hold");
    // Same for the whole-stream pass: it must have populated real arrays AND strings, or
    // "dynamic content survives" is vacuously true.
    Check(synth_runs >= 6 && synth_arrays >= 4 && synth_elements >= 8 && synth_strings >= 2 && synth_scalars >= 20, "P18/synth",
          "the whole-stream pass populated almost nothing -- its premise (arrays / strings reachable) did not hold");
    // A type the synthesizer cannot lay out is a hole in coverage, not a pass. Keep the
    // hole small and visible rather than silently growing.
    Check(synth_unsupported <= 6, "P18/synth", "too many types have a TypeTree shape the stream synthesizer cannot lay out");
}

// ---- P19 helpers --------------------------------------------------------------------
namespace
{
    int CountMeshRenderers(GameObject* go)
    {
        if (go == nullptr)
            return -1;
        int n = 0;
        for (ImmediatePtr<Component>& slot : go->EditorGetComponentsRef())
            if (Cast<MeshRenderer>(static_cast<Component*>(slot)) != nullptr)
                ++n;
        return n;
    }

    MeshRenderer* FirstMeshRenderer(GameObject* go)
    {
        if (go == nullptr)
            return nullptr;
        for (ImmediatePtr<Component>& slot : go->EditorGetComponentsRef())
            if (auto* mr = Cast<MeshRenderer>(static_cast<Component*>(slot)))
                return mr;
        return nullptr;
    }

    /// First material name of a renderer ("<none>" when it has none) — the marker each
    /// MeshRenderer in P19 is told apart by.
    std::string MaterialOf(MeshRenderer* mr)
    {
        if (mr == nullptr)
            return "<null>";
        const auto& mats = mr->getSharedMaterialAssets();
        return mats.empty() ? std::string("<none>") : std::string(mats[0].c_str());
    }

    MeshRenderer* MakeMeshRenderer(ObjectManager* om, const char* material)
    {
        auto* mr = static_cast<MeshRenderer*>(om->Produce(TypeOf<MeshRenderer>(), 0));
        if (mr == nullptr)
            return nullptr;
        om->AllocateAndAssignInstanceID(mr);
        SubMeshRes sub;
        sub.m_MeshAsset = "Cube.obj";
        mr->SetSubMeshes({sub});
        mr->SetSharedMaterialAssets({material});
        return mr;
    }

    bool IsPaired(const PrefabInstance* instance, const Object* instance_object)
    {
        return instance != nullptr && instance_object != nullptr &&
               !instance->FindSourceForInstance(PPtr<Object>(const_cast<Object*>(instance_object))).IsNull();
    }
}  // namespace

// A8: the structural half of a prefab instance's life — components added / suppressed on
// an instance, and what Apply / Revert / Merge / the Inspector's row model do with them.
// Runs against a real Variant (base <- variant) because that is where structure got
// lost: A7 let a Variant carry added / removed components but Revert and Merge never
// looked at them, Revert put back a suppressed component with the BARE source value,
// and one field's Apply silently committed the instance's whole structure.
static void scenario_P19_variant_structure()
{
    std::fprintf(stderr, "[P19] Variant structure: Revert / Merge / Apply / Inspector rows\n");
    namespace M = PrefabOverrideModel;
    std::error_code ec;
    const std::filesystem::path base_path = "p19_base.prefab";
    const std::filesystem::path variant_path = "p19_variant.prefab";
    std::filesystem::remove(base_path, ec);
    std::filesystem::remove(variant_path, ec);

    ObjectManager* om = GET_SYSTEM(ObjectManager);
    GameObject* root = MakeGameObject(om, "P19Root");
    GameObject* child = MakeGameObject(om, "P19Child");
    MeshRenderer* base_mr = MakeMeshRenderer(om, "Base.mat");
    if (root == nullptr || child == nullptr || base_mr == nullptr)
    {
        Check(false, "P19", "authoring failed");
        return;
    }
    TransformOf(child)->SetParent(TransformOf(root), false);
    root->addComponent(base_mr);
    root->SyncSerializedComponents();
    base_mr->PostLoadResource(root);
    Check(PrefabUtility::SaveAsPrefabAsset(root, base_path), "P19", "saving the base prefab failed");

    PrefabAsset* base_asset = PrefabUtility::LoadPrefabAsset(base_path);
    PrefabAsset* variant = base_asset != nullptr ? PrefabUtilityEditor::CreateVariant(base_asset, variant_path) : nullptr;
    Check(base_asset != nullptr && variant != nullptr && variant->IsVariant(), "P19", "base / variant creation failed");
    if (base_asset == nullptr || variant == nullptr || !variant->IsVariant())
        return;
    GameObject* base_root = base_asset->GetRootGameObject();
    MeshRenderer* src_mr = FirstMeshRenderer(base_root);
    Check(src_mr != nullptr && MaterialOf(src_mr) == "Base.mat", "P19", "the base root should carry the Base.mat renderer");
    if (src_mr == nullptr)
        return;

    PrefabSceneBinding::Install();  // for the Level-based Merge below
    Level level;

    // ---- a fresh Variant instance mirrors the base ----
    PrefabInstance* a = PrefabUtilityEditor::InstantiateVariant(variant);
    PrefabInstance* c = PrefabUtilityEditor::InstantiateVariant(variant);  // kept un-applied: the Merge subject
    if (a == nullptr || c == nullptr)
    {
        Check(false, "P19", "InstantiateVariant failed");
        PrefabSceneBinding::Uninstall();
        return;
    }
    GameObject* a_root = a->GetRootGameObject();
    GameObject* a_child = FirstChild(a_root);
    GameObject* c_root = c->GetRootGameObject();
    GameObject* c_child = FirstChild(c_root);
    Check(CountMeshRenderers(a_root) == 1 && CountMeshRenderers(a_child) == 0, "P19/inst",
          "an un-applied Variant instance has the base's shape: renderer on the root, none on the child");
    Check(!M::HasOverrides(a) && M::ListOverrides(a).empty(), "P19/inst", "a fresh instance has no overrides");

    // ---- suppress + add, then the Inspector's row model ----
    Check(PrefabUtilityEditor::SuppressSourceComponent(a, PPtr<Component>(src_mr)), "P19/rows", "suppressing the base renderer");
    MeshRenderer* added = MakeMeshRenderer(om, "Added.mat");
    Check(added != nullptr && PrefabUtilityEditor::AddComponentOverride(a, PPtr<GameObject>(a_child), added), "P19/rows",
          "adding a renderer to the child");
    Check(CountMeshRenderers(a_root) == 0 && CountMeshRenderers(a_child) == 1, "P19/rows", "instance shape after suppress + add");
    Check(!IsPaired(a, added), "P19/rows", "an instance-only component has no source counterpart");

    const std::vector<M::OverrideRow> rows = M::ListOverrides(a);
    Check(M::HasOverrides(a) && rows.size() == 2, "P19/rows",
          "a purely structural override must show up (it used to read \"0 overrides\")");
    if (rows.size() == 2)
    {
        using Kind = M::OverrideRow::Kind;
        Check(rows[0].kind == Kind::AddedComponent && rows[1].kind == Kind::RemovedComponent, "P19/rows", "row kinds: added then removed");
        Check(rows[0].can_apply && rows[1].can_apply, "P19/rows", "on a Variant both can be applied");
        Check(rows[0].label.find("P19Child") != eastl::string::npos, "P19/rows", "the added row names the host GameObject");
        Check(rows[1].label.find("P19Root") != eastl::string::npos, "P19/rows",
              "the removed row names the INSTANCE-side host (its target is the shared source)");
    }

    // ---- structural Revert: row by row ----
    if (rows.size() == 2)
    {
        Check(M::RevertStructuralRow(a, rows[1]), "P19/revert", "reverting the removed-component row");
        Check(CountMeshRenderers(a_root) == 1 && MaterialOf(FirstMeshRenderer(a_root)) == "Base.mat", "P19/revert",
              "the suppressed renderer is back, with the base value");
        Check(IsPaired(a, FirstMeshRenderer(a_root)) && a->GetRemovedComponents().empty(), "P19/revert",
              "…paired with its source again, and no longer listed as removed");
        Check(PrefabUtilityEditor::SuppressSourceComponent(a, PPtr<Component>(src_mr)), "P19/revert", "suppress it a second time");
        int pairs_for_src = 0;
        for (const auto& e : a->GetCorrespondingSourceMap())
            if (e.source_object.GetInstanceID() == src_mr->GetInstanceID())
                ++pairs_for_src;
        Check(pairs_for_src == 0, "P19/revert",
              "a suppressed component must leave no stale pair (one round trip used to leave it paired twice)");
        Check(M::RevertStructuralRow(a, M::ListOverrides(a)[0]) && CountMeshRenderers(a_child) == 0 && a->GetAddedComponents().empty(),
              "P19/revert", "reverting the added-component row detaches it");
        Check(PrefabUtilityEditor::AddComponentOverride(a, PPtr<GameObject>(a_child), added), "P19/revert", "re-add for Apply below");
    }

    // ---- Revert All restores the whole shape ----
    {
        PrefabInstance* r = PrefabUtilityEditor::InstantiateVariant(variant);
        GameObject* r_root = r != nullptr ? r->GetRootGameObject() : nullptr;
        GameObject* r_child = FirstChild(r_root);
        Check(r != nullptr && PrefabUtilityEditor::SuppressSourceComponent(r, PPtr<Component>(src_mr)) &&
                  PrefabUtilityEditor::AddComponentOverride(r, PPtr<GameObject>(r_child), MakeMeshRenderer(om, "Local.mat")),
              "P19/revertall", "setup");
        const int n = PrefabUtilityEditor::Revert(r);
        Check(n == 2, "P19/revertall", "Revert counts one suppressed + one added component");
        Check(CountMeshRenderers(r_root) == 1 && CountMeshRenderers(r_child) == 0 && !M::HasOverrides(r), "P19/revertall",
              "Revert All: shape equals the source again and no override is left");
        Check(MaterialOf(FirstMeshRenderer(r_root)) == "Base.mat", "P19/revertall", "the re-introduced renderer carries the base value");
    }

    // ---- a re-introduced component carries the Variant chain's overrides on it ----
    // The Variant overrides a field of the BASE renderer. Suppress that renderer on an
    // instance, Revert: what comes back must be the Variant's value (9), not the bare
    // base's (1) — a plain clone of the source would silently drop the variant layer.
    {
        PropertyModification layer;
        // The locator cannot index into arrays of variable-size elements
        // ("shared_materials[0]" does not resolve), so override the whole array: one
        // string, laid out as [count=1][len=8]["Over.mat"] — the same 16 bytes as
        // ["Base.mat"], which is what a same-size splice needs.
        layer.target = PPtr<Object>(src_mr);
        layer.property_path = eastl::string("shared_materials");
        {
            const int32_t one = 1, eight = 8;
            layer.value_bytes.resize(16);
            std::memcpy(layer.value_bytes.data(), &one, 4);
            std::memcpy(layer.value_bytes.data() + 4, &eight, 4);
            std::memcpy(layer.value_bytes.data() + 8, "Over.mat", 8);
        }
        variant->AddVariantOverride(layer);

        PrefabInstance* q = PrefabUtilityEditor::InstantiateVariant(variant);
        GameObject* q_root = q != nullptr ? q->GetRootGameObject() : nullptr;
        Check(q != nullptr && MaterialOf(FirstMeshRenderer(q_root)) == "Over.mat", "P19/layer",
              "precondition: the Variant's override is applied on instantiate (Base.mat -> Over.mat)");
        Check(q != nullptr && PrefabUtilityEditor::SuppressSourceComponent(q, PPtr<Component>(src_mr)) &&
                  CountMeshRenderers(q_root) == 0,
              "P19/layer", "suppress the base renderer");
        Check(q != nullptr && PrefabUtilityEditor::Revert(q) == 1 && CountMeshRenderers(q_root) == 1, "P19/layer",
              "Revert brings it back");
        Check(MaterialOf(FirstMeshRenderer(q_root)) == "Over.mat", "P19/layer",
              "…carrying the Variant's override, not the bare base value (Base.mat)");

        // Take the layer back off so the Apply section below starts from a clean Variant.
        variant->GetVariantOverrides().pop_back();
    }

    // ---- Apply All commits the structure into the Variant ----
    const M::ApplyReport applied = M::ApplyAll(&level, a);
    Check(applied.applied == 2 && applied.saved, "P19/apply", "ApplyAll: one addition + one removal, file written");
    Check(variant->GetVariantRemovedComponents().size() == 1 && variant->GetVariantAddedComponents().size() == 1, "P19/apply",
          "the Variant now carries 1 removed / 1 added");
    Check(!M::HasOverrides(a), "P19/apply", "the instance has nothing left to override");
    Check(IsPaired(a, added), "P19/apply",
          "the live component the user added is now paired with the Variant's new template — otherwise "
          "the next Merge / Revert clones a SECOND copy onto the host");

    // ---- a fresh instance of the applied Variant ----
    PrefabInstance* b = PrefabUtilityEditor::InstantiateVariant(variant);
    GameObject* b_root = b != nullptr ? b->GetRootGameObject() : nullptr;
    GameObject* b_child = FirstChild(b_root);
    Check(CountMeshRenderers(b_root) == 0 && CountMeshRenderers(b_child) == 1, "P19/fresh",
          "a new instance: renderer gone from the root, one on the child");
    Check(MaterialOf(FirstMeshRenderer(b_child)) == "Added.mat", "P19/fresh", "…and it is the Variant's addition");
    Check(FirstMeshRenderer(b_child) != added && IsPaired(b, FirstMeshRenderer(b_child)), "P19/fresh",
          "a clone, not the user's live object; paired with the template");

    // ---- Merge: an instance made BEFORE the Apply catches up ----
    Check(CountMeshRenderers(c_root) == 1 && CountMeshRenderers(c_child) == 0, "P19/merge", "precondition: c still has the old shape");
    PrefabUtilityEditor::MergeFromSource(c);
    Check(CountMeshRenderers(c_root) == 0 && CountMeshRenderers(c_child) == 1, "P19/merge",
          "Merge propagates both the removal and the addition");
    Check(MaterialOf(FirstMeshRenderer(c_child)) == "Added.mat", "P19/merge", "the merged-in component has the Variant's value");
    PrefabUtilityEditor::MergeFromSource(c);
    Check(CountMeshRenderers(c_root) == 0 && CountMeshRenderers(c_child) == 1, "P19/merge",
          "Merge is idempotent: a second pass does not add another copy");

    // ---- an instance's own structure survives a Merge ----
    {
        PrefabInstance* k = PrefabUtilityEditor::InstantiateVariant(variant);
        GameObject* k_root = k->GetRootGameObject();
        GameObject* k_child = FirstChild(k_root);
        // The template of the Variant's addition = the source side of the live clone's pair.
        MeshRenderer* k_live = FirstMeshRenderer(k_child);
        Component* k_template =
            k_live != nullptr ? Cast<Component>(static_cast<Object*>(k->FindSourceForInstance(PPtr<Object>(k_live)))) : nullptr;
        Check(k_template != nullptr && PrefabUtilityEditor::SuppressSourceComponent(k, PPtr<Component>(k_template)),
              "P19/keep", "suppress the Variant's own addition on one instance");
        Check(PrefabUtilityEditor::AddComponentOverride(k, PPtr<GameObject>(k_root), MakeMeshRenderer(om, "Mine.mat")), "P19/keep",
              "add a renderer of its own");
        PrefabUtilityEditor::MergeFromSource(k);
        Check(CountMeshRenderers(k_child) == 0, "P19/keep", "Merge must not re-introduce what the instance suppressed");
        Check(CountMeshRenderers(k_root) == 1 && MaterialOf(FirstMeshRenderer(k_root)) == "Mine.mat", "P19/keep",
              "…nor drop what the instance added");
        // Reverting a suppressed VARIANT-ADDED component brings back exactly one, valued.
        PrefabUtilityEditor::Revert(k);
        Check(CountMeshRenderers(k_child) == 1 && MaterialOf(FirstMeshRenderer(k_child)) == "Added.mat" &&
                  CountMeshRenderers(k_root) == 0,
              "P19/keep", "Revert: the addition returns exactly once, the instance's own renderer goes");
    }

    // ---- single-field Apply does not take the structure along ----
    {
        PrefabInstance* d = PrefabUtilityEditor::InstantiateVariant(variant);
        GameObject* d_root = d->GetRootGameObject();
        GameObject* d_child = FirstChild(d_root);
        Check(PrefabUtilityEditor::AddComponentOverride(d, PPtr<GameObject>(d_root), MakeMeshRenderer(om, "Leak.mat")), "P19/field",
              "instance adds a renderer (a structural override)");
        Transform* tc = TransformOf(d_child);
        tc->SetLocalScale(Vector3(5.0f, 1.0f, 1.0f));
        Check(M::SyncFieldOverride(d, tc, "m_LocalScale") == 1, "P19/field", "one property override recorded");
        const size_t added_before = variant->GetVariantAddedComponents().size();
        const M::ApplyReport one = M::ApplyField(&level, d, tc, "m_LocalScale");
        Check(one.applied == 1 && one.saved, "P19/field", "ApplyField applied the scale");
        Check(variant->GetVariantAddedComponents().size() == added_before, "P19/field",
              "…and did NOT commit the instance's added renderer into the Variant");
        Check(d->GetAddedComponents().size() == 1 && CountMeshRenderers(d_root) == 1, "P19/field",
              "the structural override is still on the instance, still live");
        Check(M::ApplyAll(&level, d).applied == 1 && variant->GetVariantAddedComponents().size() == added_before + 1, "P19/field",
              "Apply All is what commits it");
    }

    // ---- a plain prefab has no structural Apply: Revert-only rows ----
    {
        PrefabInstance* plain = PrefabUtilityEditor::InstantiateAsPrefab(base_asset);
        GameObject* p_root = plain != nullptr ? plain->GetRootGameObject() : nullptr;
        GameObject* p_child = FirstChild(p_root);
        Check(plain != nullptr && PrefabUtilityEditor::AddComponentOverride(plain, PPtr<GameObject>(p_child), MakeMeshRenderer(om, "P.mat")),
              "P19/plain", "adding a renderer on a plain-prefab instance");
        const std::vector<M::OverrideRow> prows = M::ListOverrides(plain);
        Check(prows.size() == 1 && !prows[0].can_apply, "P19/plain", "listed, but not applicable");
        Check(M::ApplyAll(&level, plain).applied == 0 && plain->GetAddedComponents().size() == 1, "P19/plain",
              "ApplyAll applies nothing and keeps the override rather than losing it");
        Check(PrefabUtilityEditor::Revert(plain) == 1 && CountMeshRenderers(p_child) == 0, "P19/plain", "Revert still works");
    }

    PrefabSceneBinding::Uninstall();
    std::filesystem::remove(base_path, ec);
    std::filesystem::remove(variant_path, ec);
}

// A8 follow-up: structural Revert on the undo stack. Drives the real UndoSystem with the
// same records the Inspector offers (structure + override list in ONE transaction).
static void scenario_P20_structure_undo()
{
    std::fprintf(stderr, "[P20] structural Revert is undoable\n");
    namespace M = PrefabOverrideModel;
    std::error_code ec;
    const std::filesystem::path base_path = "p20_base.prefab";
    const std::filesystem::path variant_path = "p20_variant.prefab";
    std::filesystem::remove(base_path, ec);
    std::filesystem::remove(variant_path, ec);

    ObjectManager* om = GET_SYSTEM(ObjectManager);
    GameObject* root = MakeGameObject(om, "P20Root");
    GameObject* child = MakeGameObject(om, "P20Child");
    MeshRenderer* base_mr = MakeMeshRenderer(om, "Base.mat");
    if (root == nullptr || child == nullptr || base_mr == nullptr)
    {
        Check(false, "P20", "authoring failed");
        return;
    }
    TransformOf(child)->SetParent(TransformOf(root), false);
    root->addComponent(base_mr);
    root->SyncSerializedComponents();
    base_mr->PostLoadResource(root);
    Check(PrefabUtility::SaveAsPrefabAsset(root, base_path), "P20", "saving the base prefab failed");
    PrefabAsset* base_asset = PrefabUtility::LoadPrefabAsset(base_path);
    PrefabAsset* variant = base_asset != nullptr ? PrefabUtilityEditor::CreateVariant(base_asset, variant_path) : nullptr;
    PrefabInstance* inst = variant != nullptr ? PrefabUtilityEditor::InstantiateVariant(variant) : nullptr;
    if (inst == nullptr)
    {
        Check(false, "P20", "variant instantiation failed");
        return;
    }
    MeshRenderer* src_mr = FirstMeshRenderer(base_asset->GetRootGameObject());
    GameObject* i_root = inst->GetRootGameObject();
    GameObject* i_child = FirstChild(i_root);

    // Both a suppressed base component AND an added one, so a single Revert All has to
    // both re-introduce and detach.
    Check(PrefabUtilityEditor::SuppressSourceComponent(inst, PPtr<Component>(src_mr)), "P20", "suppress");
    MeshRenderer* added = MakeMeshRenderer(om, "Added.mat");
    Check(PrefabUtilityEditor::AddComponentOverride(inst, PPtr<GameObject>(i_child), added), "P20", "add");
    Check(CountMeshRenderers(i_root) == 0 && CountMeshRenderers(i_child) == 1, "P20", "precondition: root 0, child 1");

    // What the Inspector's Revert All does: both records, one transaction, then Revert.
    auto Revert_all = [&](UndoSystem& undo) {
        undo.BeginTransaction("Revert Prefab Overrides");
        undo.AddRecord(M::MakeOverrideListUndoRecord(inst));
        undo.AddRecord(M::MakeStructureUndoRecord(inst));
        PrefabUtilityEditor::Revert(inst);
        undo.EndTransaction();
    };

    UndoSystem undo;
    Revert_all(undo);
    Check(undo.GetUndoCount() == 1, "P20", "Revert All made exactly one undo step");
    Check(CountMeshRenderers(i_root) == 1 && CountMeshRenderers(i_child) == 0 && !M::HasOverrides(inst), "P20/revert",
          "after Revert All the instance matches its source");

    MeshRenderer* reintroduced = FirstMeshRenderer(i_root);

    // ---- Undo: the components, the lists and the pairs all come back ----
    Check(undo.Undo(), "P20/undo", "Undo succeeded");
    Check(CountMeshRenderers(i_root) == 0 && CountMeshRenderers(i_child) == 1, "P20/undo",
          "undo restores the shape: renderer off the root, added renderer back on the child");
    Check(FirstMeshRenderer(i_child) == added, "P20/undo", "...and it is the SAME added component object, not a copy");
    Check(inst->GetAddedComponents().size() == 1 && inst->GetRemovedComponents().size() == 1, "P20/undo",
          "undo restores the added / removed lists");
    Check(M::ListOverrides(inst).size() == 2, "P20/undo", "...so the Inspector lists both component rows again");
    int pairs_for_src = 0;
    for (const auto& e : inst->GetCorrespondingSourceMap())
        if (e.source_object.GetInstanceID() == src_mr->GetInstanceID())
            ++pairs_for_src;
    Check(pairs_for_src == 0, "P20/undo", "the suppressed component has no pair (as before the Revert)");

    // ---- Redo: and back ----
    Check(undo.Redo(), "P20/redo", "Redo succeeded");
    Check(CountMeshRenderers(i_root) == 1 && CountMeshRenderers(i_child) == 0 && !M::HasOverrides(inst), "P20/redo",
          "redo re-applies the Revert");
    Check(FirstMeshRenderer(i_root) == reintroduced, "P20/redo",
          "redo brings back the SAME re-introduced clone (it was parked, not destroyed)");
    pairs_for_src = 0;
    for (const auto& e : inst->GetCorrespondingSourceMap())
        if (e.source_object.GetInstanceID() == src_mr->GetInstanceID())
            ++pairs_for_src;
    Check(pairs_for_src == 1, "P20/redo", "exactly one pair for the source component after redo");

    // ---- a second round trip must be stable (no growth, no duplication) ----
    Check(undo.Undo() && undo.Redo() && undo.Undo(), "P20/cycle", "undo / redo / undo");
    Check(CountMeshRenderers(i_root) == 0 && CountMeshRenderers(i_child) == 1 && inst->GetCorrespondingSourceMap().size() >= 1,
          "P20/cycle", "state after repeated cycling is still the pre-Revert state");
    Check(undo.Redo(), "P20/cycle", "final redo");

    // ---- a Revert that changes nothing must not make an undo step ----
    {
        const size_t steps = undo.GetUndoCount();
        undo.BeginTransaction("No-op revert");
        undo.AddRecord(M::MakeStructureUndoRecord(inst));
        undo.EndTransaction();
        Check(undo.GetUndoCount() == steps, "P20/noop", "an unchanged structure must not make an undo step");
    }

    // ---- per-row Revert is undoable too ----
    {
        PrefabInstance* row_inst = PrefabUtilityEditor::InstantiateVariant(variant);
        GameObject* r_root = row_inst->GetRootGameObject();
        Check(PrefabUtilityEditor::SuppressSourceComponent(row_inst, PPtr<Component>(src_mr)), "P20/row", "suppress");
        const auto rows = M::ListOverrides(row_inst);
        Check(rows.size() == 1 && rows[0].kind == M::OverrideRow::Kind::RemovedComponent, "P20/row", "one removed row");
        UndoSystem row_undo;
        row_undo.BeginTransaction("Revert Prefab Component");
        row_undo.AddRecord(M::MakeStructureUndoRecord(row_inst));
        Check(rows.size() == 1 && M::RevertStructuralRow(row_inst, rows[0]), "P20/row", "revert the row");
        row_undo.EndTransaction();
        Check(CountMeshRenderers(r_root) == 1, "P20/row", "the suppressed renderer is back");
        Check(row_undo.Undo() && CountMeshRenderers(r_root) == 0 && row_inst->GetRemovedComponents().size() == 1, "P20/row",
              "undo suppresses it again and restores the removed list");
        Check(row_undo.Redo() && CountMeshRenderers(r_root) == 1 && row_inst->GetRemovedComponents().empty(), "P20/row",
              "redo un-suppresses it");
    }

    // ---- coalescing: two offers of the record in one transaction keep the OLDEST state ----
    {
        PrefabInstance* c_inst = PrefabUtilityEditor::InstantiateVariant(variant);
        UndoSystem cu;
        cu.BeginTransaction("two offers");
        Check(M::StructureUndoKey(c_inst) != 0 && M::StructureUndoKey(c_inst) != M::OverrideListUndoKey(c_inst), "P20/key",
              "the structure key is non-zero and distinct from the override-list key");
        cu.AddRecord(M::MakeStructureUndoRecord(c_inst));
        Check(cu.HasPendingRecord(M::StructureUndoKey(c_inst)), "P20/key", "the open transaction reports the pending record");
        PrefabUtilityEditor::SuppressSourceComponent(c_inst, PPtr<Component>(src_mr));
        cu.AddRecord(M::MakeStructureUndoRecord(c_inst));  // second offer, AFTER a change: must be ignored
        cu.EndTransaction();
        Check(cu.Undo() && CountMeshRenderers(c_inst->GetRootGameObject()) == 1, "P20/key",
              "undo goes back to the state at the FIRST offer (renderer present), not the second");
    }

    std::filesystem::remove(base_path, ec);
    std::filesystem::remove(variant_path, ec);
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
    scenario_P19_variant_structure();
    scenario_P20_structure_undo();

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
