# Text-serialized object graphs (YAML scenes & prefabs)

ZEngine's serialization is Unity-inspired: a single `Transfer()` method per
type drives every backend (binary `SerializedFile`, JSON, TypeTree). This doc
covers the **text object-graph** backend that persists *scenes* and *prefabs*
as human-readable, diff-friendly YAML, while DDC-imported assets (textures,
meshes, animations, ...) stay binary `.zasset`.

```
Authoring data (scene / prefab)   -> YAML   (.scene / .prefab)   text, VCS-friendly
Imported data (texture/mesh/anim) -> binary (.zasset)           SerializedFile
```

This mirrors Unity's split: `.unity` / `.prefab` are YAML; imported assets are
binary in the `Library/`.

---

## 1. Layers

| Layer | File | Role |
|-------|------|------|
| YAML emitter / parser | `Core/YamlSerialize/YamlText.{h,cpp}` | rapidjson DOM <-> block-YAML string. Constrained parser (no anchors/flow-merge); the emitter only produces what the parser accepts. |
| Single-object transfer backend | `Core/YamlSerialize/YAMLWrite.{h,cpp}`, `YAMLRead.{h,cpp}` | `Transfer()`-driven write/read against the shared rapidjson DOM (peer of `JSONWrite`/`JSONRead`). |
| Multi-object graph | `Core/YamlSerialize/YamlObjectGraph.{h,cpp}` | The text analogue of `SerializedFile`: many objects in one document, cross-referenced by local fileID, external refs by GUID. |
| Asset I/O | `Resource/Asset/AssetManager::{WriteObjectsToYaml,ReadObjectsFromYaml}` | Bridges the graph's external-ref hooks to the AssetRegistry / GUID system. |
| Scene routing | `Function/Framework/Level/Level::{save,load}` | `.scene` extension -> YAML graph; legacy `.level.json` / `.zasset` paths untouched. |

The reader's polymorphic entry point was dead before this work:
`TextTransferReadBase::TransferBase` was an empty stub, so
`VirtualRedirectTransfer` could not drive a text read for a runtime-typed
object. It now navigates into the type-string key the writer emits, symmetric
with `TextTransferWriteBase::TransferBase`.

---

## 2. On-disk shape (one block-YAML document)

```yaml
externals:
  - guid: "a1b2..."      # 0-based; referenced as m_FileID = index + 1
    type: "MaterialRes"
    path: "Assets/Foo.zasset"
objects:
  - fileID: 1            # Unity-style local file id (anchor)
    type: "LevelRes"     # class-name tag -> TypeManager::ClassNameToType
    data:
      LevelRes:          # body is exactly what YAMLWrite emits for the object
        gravity: {x: 0, y: 0, z: -9.8}
        character_name: "Player"
  - fileID: 2
    type: "GameObject"
    data:
      GameObject:
        m_Name: "Player"
        m_Component:
          - component: {m_FileID: 0, m_PathID: 3}   # local ref -> fileID 3
  - fileID: 3
    type: "TransformComponent"
    data:
      TransformComponent: { ... }
```

* `data` is the verbatim `YAMLWrite` output for that object: a single mapping
  keyed by the type string, fields underneath. `YamlObjectGraph` deep-copies
  this node across rapidjson allocators rather than round-tripping a YAML
  string per object (see `YAMLWrite::GetDocument()` /
  `YAMLRead(const Value&, flags)`).

---

## 3. Reference model (shared with the binary path)

Cross-object references use the **same per-thread `IPPtrResolver`** as the
binary `SerializedFile`, so `PPtr<T>` and `ImmediatePtr<T>` need zero
backend-specific code. The wire format is Unity's `(m_FileID:int32,
m_PathID:int64)` pair:

* **Local** target (same document): `m_FileID = 0`, `m_PathID = target fileID`.
* **External** target (imported `.zasset`): `m_FileID = externals index + 1`,
  `m_PathID = target local id`; the `externals` table carries GUID/type/path.

`YamlGraphResolver` (anonymous in `YamlObjectGraph.cpp`) is a self-contained
`IPPtrResolver` that owns its own externals vector (so we don't stand up a
partially-initialized binary `SerializedFile` just to host the table). It
mirrors `SerializedFilePPtrResolver`'s dedup semantics.

`ImmediatePtr<T>::Transfer` was a stub before this work; it now resolves
through `IPPtrResolver` exactly like `PPtr<T>` (helpers
`ImmediatePtrResolveWrite` / `ImmediatePtrResolveRead` in `ImmediatePtr.cpp`
keep the header free of `Object`/`ObjectManager` includes).

### Write / read flow

* **Write**: every object is assigned a stable `InstanceID`
  (`ObjectManager::AllocateAndAssignInstanceID`) and registered as local
  (`InstanceID <-> fileID`) before any field is serialized, so forward
  references resolve. Each object is serialized under a `ScopedPPtrResolver`.
* **Read**: a **two-pass** walk -- first produce + register every object by
  its class-name tag (`TypeManager::ClassNameToType` -> `ObjectManager::Produce`),
  then deserialize fields under the active resolver so references (which may
  point forward in document order) all resolve.

---

## 4. GameObject component split

A live `GameObject` keeps two component containers:

* `m_Components` -- the runtime list (`std::vector<ImmediatePtr<Component>>`).
* `m_Component`  -- the serialized `ImmediatePtr` container that `Transfer()`
  touches.

`SyncSerializedComponents()` (before write) and `RebuildRuntimeComponents()`
(after read) bridge the two. `Level::save` appends each `GameObject` and each
of its `Component`s as **separate top-level graph entries**; the
`GameObject -> Component` link is a local fileID reference. On load,
`Level::load` calls `RebuildRuntimeComponents()` then funnels each `GameObject`
through the existing `CreateObject(const GameObject&)` template path.

---

## 5. Gotchas discovered while landing this

* **`TypeManager::ClassNameToType` was pointer-keyed.** `m_KlassNameToType`
  is `std::unordered_map<const char*, const Type*>` -- `std::hash<const char*>`
  hashes the *address*, so a lookup only matched when the caller passed the
  exact string literal `REGISTER_CLASS` stored. Deserialization-by-name hands
  in freshly parsed buffers, which never matched. Fixed by adding a
  `std::string`-keyed mirror `m_ClassNameStringToType` consulted first.
  (Two pre-existing callers -- the asset-type label in `AssetManager.cpp` and
  the DataTable inspector -- were silently relying on the broken behavior and
  are now correct.)

* **`Type::IsBaseOf` depends on a fully-built DFS type tree.** In a minimal
  standalone process (the smoke test) a leaf type can come back with
  `descendantCount == 0`, so `Is(self)` is `0 < 0 == false`. Scene/graph code
  that only needs *exact* type discrimination (LevelRes vs GameObject vs
  Component) therefore compares `GetType() == TypeOf<T>()` instead of `Is()`.

---

## 6. Status / scope

| Phase | Status | Summary |
|-------|--------|---------|
| P1 scenes | **done** | `.scene` YAML graph; `Level::save`/`load` routed; ProjectWindow creates `.scene`; round-trip smoke test (`YamlRoundTripSmokeTest` scenario Y5). |
| P2 prefabs | **done** | `.prefab` YAML graph via the same `YamlObjectGraph`. `PrefabUtility::SaveAsPrefabAsset` / `InstantiateFromPath` branch on the `.prefab` extension; `BuildUniquePrefabAssetPath` now emits `.prefab`; `EditorScenePlacement` accepts `.prefab` drops (extension is authoritative, no header read). Round-trip smoke test scenario Y6 (PrefabAsset header + `m_RootGameObject` ImmediatePtr). |

### Prefab specifics (P2)

`SaveAsPrefabAsset` builds the identical ordered object list it always did
(slot 0 = `PrefabAsset` header @ fileID 1, then DFS GameObjects + Components);
only the sink changes (`WriteObjectsToYaml` vs `WriteObjectsToDiskThreadSafe`).
`InstantiateFromPath` reads the graph, finds the `PrefabAsset` entry (its
`m_RootGameObject` ImmediatePtr already resolved to the live root), calls
`GameObject::RebuildRuntimeComponents()` on every read GameObject (the binary
asset pipeline does this internally; the graph reader doesn't), then runs the
existing `PrefabPostLoadDriver`.

**Multi-level children (YAML path) — Phase-1 limitation lifted.**
`PrefabPostLoadDriver::Run` now takes an optional `known_gameobjects` vector.
The YAML `InstantiateFromPath` already has the complete flat GameObject set
(every fileID entry that `GetType() == TypeOf<GameObject>()`), so it passes it
in; `BuildOwnerMap` seeds `TransformComponent -> owning-GameObject` from that
list, letting `VisitGameObject` resolve child Transforms at arbitrary depth
(root -> child -> grandchild -> ...) instead of just the root's direct
children. Each GO's components are rebuilt via `RebuildRuntimeComponents()`
before the driver runs, so `tryGetComponent(TransformComponent)` is valid
during the seed. The legacy **binary** path still calls `Run(root)` with an
empty list (the binary `PrefabAsset` carries no flat list), so its single-level
Phase-1 limitation is unchanged — only the text path is fixed. Depth-3
resolution is covered by smoke scenario Y7.

DDC-imported assets stay binary `.zasset` by design -- only authoring graphs
(scene/prefab) move to text.

### Test

`engine/Source/Runtime/Core/Serialize/Test/YamlRoundTripSmokeTest.cpp`
(scenarios Y1-Y4 = scalar/nested/sequence/tricky-string encoding; Y5 =
multi-object graph with a resolved local `ImmediatePtr`; Y6 = `PrefabAsset`
root round-trip; Y7 = multi-level Transform hierarchy root->child->grandchild,
proving the child-Transform PPtr sequence resolves at depth, which is what the
`PrefabPostLoadDriver` owner-map seed relies on; Y8 = full **disk** round-trip
through `AssetManager::WriteObjectsToYaml`/`ReadObjectsFromYaml` -- mirrors
`Level::save`/`Level::load` exactly (LevelRes header @ fileID 1 carrying
gravity + character name, then a 3-deep GameObject hierarchy + their
TransformComponents), written to a temp `.scene` file and read back, asserting
header settings + GameObject count + the root->child->grandchild chain all
survive. Y8 exercises the real editor save/load code path headlessly via the
concrete `RuntimeAssetManager`, so no GUI is needed to validate scene I/O).
Build with `-DZENGINE_BUILD_YAML_ROUND_TRIP_SMOKE_TEST=ON`, target
`YamlRoundTripSmokeTest`.
