# PPtr Resurrection — Design Doc (PR-SE3a-refine)

> Status: **APPROVED 2026-05-19** — design signed off, implementation may
> begin under PR-SE3a-refine.
> Authors: agent (drafted 2026-05-18) / approved by human reviewer 2026-05-19.
> Scope: PR-SE3a-refine. Subsequent migration to `MaterialRes::m_shader_guid →
> PPtr<ShaderRes>` happens in **PR-SE3a-migrate**, after this design's
> runtime contract is in place.

---

## 1. Why this doc exists

ZEngine carries a `PPtr<T>` template at
`engine/Source/Runtime/BaseClasses/PPtr.h`, modelled on Unity's `PPtr<T>` but
**its `Transfer()` method is an empty stub** (line 96-99 of that file).
Concretely:

```cpp
// PPtr.h — present body
template<typename T>
template<typename TransferFunction>
inline void PPtr<T>::Transfer(TransferFunction& transfer)
{
    // <--- nothing here
}
```

That means **39 fields of `PPtr<X>` scattered across 27 res_type / Prefab /
Component / Animation / Render / Editor source files have never participated
in serialization**. They round-trip a 4-byte `m_InstanceID` in memory, but
the `.zasset` writer never emits anything for them and the reader never
reconstructs anything for them. Every cross-asset reference today is
laundered through `eastl::string` shader names, texture paths, or prefab
GUID strings, because the reference layer that *should* solve this — `PPtr`
— is non-functional.

`MaterialRes::m_shader_guid` (added in PR-SE3a-shadow as a sibling string
field next to `m_shader`) is the **canary** that surfaced this gap. Today it
works as a string-typed GUID slot (`""` means "use legacy `m_shader` name").
The *eventual* shape we want is `PPtr<ShaderRes> m_shader`, but reaching
that requires:

1. A real binary representation for `PPtr<T>` — Unity's
   `(m_FileID:int32, m_PathID:int64)` pair, where `m_FileID` indexes an
   **External References table** in the file header and `m_PathID` is a
   `LocalIdentifierInFileType` within the referenced file.
2. An External References table in the `.zasset` header — currently absent.
   `engine/Source/Runtime/Core/Serialize/SerializedFile.h` declares
   `struct FileIdentifier { enum InsertMode { ... }; }` with **no fields**,
   confirming nothing has ever been wired.
3. A resolver layer that maps `(fileIndex → guid → loaded Object* → InstanceID)`
   on read and the inverse on write. Unity calls this `ILSOIResolver`
   (`LocalSerializedObjectIdentifier`); ZEngine has no equivalent today.
4. A schema-evolution path so existing `.zasset` files (which today serialize
   `m_shader` as a string and have no concept of External Refs) continue to
   load.

This document specifies all four.

---

## 2. Reference designs

### 2.1 Unity (primary reference)

The contract we copy is documented in Unity's own internal docs, mirrored at
`../unity2023.1/Documentation/InternalDocs/docs/Runtime/docs/ObjectTracking/
serialization-of-references.md`. Distilled:

* In binary `.asset` files, every `PPtr<T>` field serializes as **two
  fields**: `m_FileID` (int32) + `m_PathID` (int64).
* `m_FileID = 0` is reserved and means "this very file" — the referenced
  object lives in the same SerializedFile, look up `m_PathID` directly in
  the local object table.
* `m_FileID >= 1` is a **1-based index** into the file's `m_Externals` table
  (`FileIdentifierArray` in `SerializedFile.h` line 277), where each entry
  stores at minimum `(guid, type, pathName)`. `pathName` is empty in editor
  output (guid is the lookup key) and replaced by a path in player builds.
* `m_PathID` is a `LocalIdentifierInFileType` (int64) — a stable identifier
  *within* the target file. For ZEngine's MVP we have one Object per
  `.zasset`, so the only legal value is the well-known root LFID
  (we use `1` to match Unity convention and leave room for sub-assets).
* The Transfer code is `TransferPPtr` in
  `../unity2023.1/Runtime/BaseClasses/PPtr.h` line 346-386. Note Unity reads
  *both* `m_FileID` and `m_PathID` even when not remapping, so the binary
  layout is uniform.

### 2.2 Unreal Engine (secondary reference)

UE's equivalent is `FPackageIndex` (int32, 0=null, +N=imported, -N=exported)
plus `FName`-based `FSoftObjectPath` for cross-package soft references.
This is **not** what we copy because:

* ZEngine's asset registry is GUID-based, not name-based;
* UE collapses the External Refs table into a per-package import/export
  table that is mostly relevant for `.umap`/`.uasset` cooking, which we don't
  do today.

We borrow exactly one idea from UE: **redirector semantics**. When a
referenced asset is renamed/moved, the GUID stays stable, so PPtrs survive
without rewriting. Same property holds in our design because the External
table holds GUIDs, not paths.

---

## 3. Current ZEngine state (what exists, what doesn't)

| Component | State | Where |
|---|---|---|
| `PPtr<T>` template | Has `m_InstanceID:int32`, `Transfer()` empty stub | `BaseClasses/PPtr.h:97-99` |
| `FileIdentifier` struct | Empty shell — only `enum InsertMode { ... }`, no fields | `Core/Serialize/SerializedFile.h:16-24` |
| External Refs table | **Does not exist** in any form | — |
| `AssetFileHeader` ZASS prefix | 176 bytes, `reserved[4]` (32 bytes) free for future fields | `Runtime/asset/asset_file.h:47-58` |
| `m_AssetGuid` on SerializedFile | Read/write, stamped into header | `Core/Serialize/SerializedFile.h:130-133` |
| `MaterialRes::m_shader_guid` | `eastl::string` sibling of `m_shader`, default `""` | `res_type/data/material.h:90` |
| ScriptRegistry | Working, JSON-backed `path↔guid` map | `Scripting/ScriptRegistry.{h,cpp}` |
| ShaderRegistry | **Does not exist** — shaders resolved by name via `AssetManager::getAssetsByType("ShaderRes")` (PR-SE3b chose strategy B2: reuse AssetRegistry type index) |
| AssetRegistry GUID lookup | Working in editor, scans `.zasset` headers | `Editor/asset_registry/asset_registry.cpp` |

Two facts to internalise:

* **`AssetFileHeader.reserved[4]` stays untouched.** Earlier drafts of this
  doc planned to repurpose 12 of those 32 bytes for `externals_offset` /
  `externals_count`. **Implementation prep (2026-05-19) revealed a simpler
  path that follows Unity 1:1**: Unity writes its externals table inside
  the metadata buffer (`SerializedFile.cpp:1814-1825`), not in the outer
  prefix. ZEngine does the same — see §4.2. This means
  `scanSingleAsset` keeps reading exactly 176 bytes of zero-modified prefix
  and is **completely unaffected** by this PR.
* **The on-disk version bump moves into `SerializedFileHeader.version`**
  (the inner header at offset 176, not the ZASS prefix). Today the writer
  zero-inits this field via `metadataBuffer.resize(sizeof(SerializedFileHeader))`
  and the reader passes the value through to `ReadMetadata` but never
  inspects it. So `version` is currently 0 in every existing `.zasset`, and
  bumping it to 1 in the writer and gating the externals block on
  `version >= 1` in the reader gives us a clean forward/backward compat
  boundary with zero changes to the outer prefix.
* **Schema-evolution machinery already works for missing-field fallthrough**
  (proven by S2 of `schema_evolution_smoke_test.cpp`). What it has *never*
  been asked to do is **type-change** for an existing field. Section 6
  below explains why we sidestep that for `m_shader_guid` and what the
  long-term plan is.

---

## 4. Target binary contract

### 4.1 PPtr field layout (binary, post-PR-SE3a-refine)

Every `PPtr<T>` field, when serialized through a writing TransferFunction
where `NeedsInstanceIDRemapping()` is true, emits two consecutive TypeTree
nodes:

```
+----+----------+----------+
| TT |  m_FileID  : int32  |  // 0 = local; >=1 = 1-based index into m_Externals
+----+----------+----------+
| TT |  m_PathID  : int64  |  // LocalIdentifierInFileType inside the target file
+----+----------+----------+
```

Both children are written under a parent TT node named after the field
(e.g. `"m_shader"`). The two child node names are **exactly** `m_FileID`
and `m_PathID` (matching Unity binary; *not* `fileID`/`pathID`, which is
Unity's YAML convention).

`AllowTransferOptimization() = false` stays — PPtr cannot be memcpy'd in
or out because the `(InstanceID ↔ FileID/PathID)` mapping has to run.

### 4.2 ExternalReferences table layout (inside metadata buffer)

The externals table is appended to the existing metadata buffer, **after**
the `(typeCount, types[], objectCount, objects[])` records that
`BuildMetadataSection` already writes (`SerializedFile.cpp:464-484`). So
the on-disk layout becomes:

```
[ZASS prefix 176B, untouched]
[SerializedFileHeader (inner)]   <-- header.version becomes 1 (was 0)
[metadata: typeCount, types[], objectCount, objects[],
           externalsCount:int32, externals[]]   <-- NEW tail
[ data block: aligned-up object payloads ]
```

`SerializedFileHeader.metadataSize` naturally grows to cover the externals
tail. `SerializedFileHeader.dataOffset` already points past
`metadataBuffer.size()` rounded up to `kSectionAlignment`, so the data
section's offset auto-adjusts. **No outer-prefix bytes change.**

### 4.3 ExternalReferences entry on-disk format

For each entry, in metadata buffer order:

```
+--------------------------+
| guid_len    : int32       |   // number of bytes in guid string (typically 32)
| guid_bytes  : char[guid_len]
| pad to 4-byte align
| asset_type  : uint8       |   // 0=Self(unused), 2=kSerializedAssetType, 3=kMetaAssetType
| has_path    : uint8       |   // 0=guid-only (editor); 1=path follows (player)
| pad         : uint16
| path_len    : int32       |   // 0 unless has_path == 1
| path_bytes  : char[path_len]
| pad to 4-byte align
+--------------------------+
```

Editor builds emit `asset_type = 2`, `has_path = 0`, `path_len = 0`. Player
builds (future) will emit `has_path = 1` with path strings. The on-disk
encoding is little-endian to match every other ZEngine binary surface; we
do not call SwapEndianBytes on this region (Unity's reader does only when
the host platform endianness disagrees with `header.endianess`, which on
ZEngine is always little).

`m_FileID = 0` is **never** in the table — it always means "current file".

### 4.4 Reader fallthrough for legacy files

`SerializedFile::ReadMetadata` already takes `version` as its first
argument and ignores it today. This PR teaches it to branch:

* `version == 0` (every `.zasset` written before this PR): the metadata
  cursor reaches end-of-buffer right after the last `ObjectInfo` record.
  Don't attempt to read `externalsCount`. Leave `m_Externals` empty.
* `version >= 1`: after the object table, read `externalsCount:int32`
  followed by that many entries. Buffer-end iterator check
  (`iterator == end`) at function exit must still pass — already enforced
  by `ReadMetadata`'s final `return iterator == end;`.

Forward compat: an unknown future version that adds a field after externals
will fail the `iterator == end` check on this PR's reader and surface as
`kSerializedFileLoadError_InvalidMetadata`. That is acceptable — we don't
guarantee ZEngine N reads files written by ZEngine N+1.

---

## 5. Runtime contract

### 5.1 Resolver interface (`IPPtrResolver`)

Added at `Runtime/BaseClasses/IPPtrResolver.h` (new file). Mirrors Unity's
`ILSOIResolver` minus the bits we don't need:

```cpp
struct LocalSerializedObjectIdentifier
{
    int32_t localSerializedFileIndex;  // m_FileID (0 = self)
    int64_t localIdentifierInFile;     // m_PathID
};

class IPPtrResolver
{
public:
    virtual ~IPPtrResolver() = default;
    // Reading: (fileIndex, pathID) -> InstanceID. Loads target asset on miss.
    virtual int32_t LSOIToInstanceID(const LocalSerializedObjectIdentifier& lsoi) = 0;
    // Writing: InstanceID -> (fileIndex, pathID). Inserts new External entry on miss.
    virtual void    InstanceIDToLSOI(int32_t instanceID, LocalSerializedObjectIdentifier& out) = 0;
};
```

Concrete implementation `SerializedFilePPtrResolver` lives next to
`SerializedFile.cpp` and:

* On **read**: `LSOIToInstanceID` looks up `lsoi.fileIndex` in the file's
  `m_Externals` table, gets the GUID, asks `AssetManager` to load that
  asset (which assigns an InstanceID), then composes the final InstanceID.
  If `fileIndex == 0`, looks up `lsoi.pathID` in the current file's local
  object table.
* On **write**: `InstanceIDToLSOI` reverse-maps the InstanceID via
  `ObjectManager` to find the owning asset, gets that asset's GUID via
  AssetRegistry, and either finds an existing entry in `m_Externals` or
  appends a new one. Returns the (fileIndex, well-known LFID=1) pair.

#### 5.1.1 Threading model — per-thread (TLS) active resolver

`PPtr<T>::Transfer()` (§5.2) does **not** receive the resolver as an argument
— it pulls it from `transfer.GetPPtrResolver()`. The naive implementation
would store a single `IPPtrResolver*` on the `TransferFunction` (or globally
on `SerializedFile`) and call it a day, which is what Unity does in
single-threaded editor flows. **ZEngine will not.**

Editor-side asset I/O in ZEngine is multi-thread by design:

- **AssetRegistry scan** (`asset_registry.cpp`) walks `Assets/` on a worker
  pool — each thread opens its own `ifstream` over different `.zasset` files
  to read just the 176-byte header.
- **AssetManager bulk loads** (e.g. on project open) parallelise reading
  across the asset graph; today this works only because PPtr serialization
  is dead, so threads never need a shared mutable resolver.
- **Editor preview / inspector** can trigger a read on a UI thread while a
  background thread is writing.

A single shared resolver pointer would force every PPtr-touching call site
through one mutex, serializing what is currently the fastest part of the
editor open path. Worse, a global `IPPtrResolver*` is a lifetime trap:
nested loads (Material → Shader → Texture) need to *push* a different
resolver onto the stack and pop it on return.

**Decision**: the active resolver is **thread-local**, owned by an RAII
scope object:

```cpp
// In IPPtrResolver.h
class ScopedPPtrResolver
{
public:
    explicit ScopedPPtrResolver(IPPtrResolver* r);  // pushes onto TLS stack
    ~ScopedPPtrResolver();                           // pops
    ScopedPPtrResolver(const ScopedPPtrResolver&)            = delete;
    ScopedPPtrResolver& operator=(const ScopedPPtrResolver&) = delete;
};

// Internal — accessed by TransferFunction::GetPPtrResolver()
IPPtrResolver* GetCurrentPPtrResolver() noexcept;  // returns top-of-TLS-stack or nullptr
```

Implementation: `thread_local std::vector<IPPtrResolver*> g_resolverStack;`
in `IPPtrResolver.cpp`. Push / pop is O(1); read is one TLS load. No mutex.

Call sites:

- `SerializedFile::ReadObject(...)` opens
  `ScopedPPtrResolver scope(&m_OwnedResolver);` for the duration of the
  read. Nested loads triggered by `LSOIToInstanceID` push their own
  resolver and pop on return (the outer scope is restored automatically).
- `SerializedFile::WriteObject(...)` does the symmetric push for writes.
- `TransferFunction::GetPPtrResolver()` is a thin inline that calls
  `GetCurrentPPtrResolver()`. The transfer object itself does not store
  the pointer — this avoids stale pointers when one TransferFunction
  outlives a load (it doesn't today, but the contract is cleaner).

Consequences:

- Two threads loading two different `.zasset` files never contend. Each
  has its own resolver-stack TLS; the underlying `AssetManager`
  load-cache is the only shared structure and it already has its own
  locking.
- Re-entrant loads on one thread (Material loads its referenced Shader
  loads its referenced Texture) work via the stack: the inner load
  pushes the *target* file's resolver; the outer pop restores the
  *source* file's resolver. If the inner load fails, the destructor
  still pops cleanly (RAII).
- Callers that do raw `Transfer()` outside a `Read/WriteObject` (e.g.
  unit tests, clipboard / undo) get `GetCurrentPPtrResolver() == nullptr`
  and fall through to the "memory transfer" branch in §5.2 (which
  preserves `m_InstanceID` directly and never touches the resolver).
  This is the contract that makes **P4 (null PPtr) and the clipboard
  path** work without any test scaffolding.
- Worker threads spawned by `AssetManager` for nested async loads
  inherit nothing — they must open their own `ScopedPPtrResolver` for
  the file they're reading. This is the same contract as a fresh thread
  entering the editor, so no special case in code; just a call-site
  discipline that we cover with smoke test **P7** (see §7.1).

`SerializedFilePPtrResolver` itself does **not** need internal locking:
each instance is owned by exactly one `SerializedFile`, and the TLS
contract guarantees only one thread's `Transfer()` calls can reach it
through `GetPPtrResolver()` at any given time (because that thread is
the only one with that resolver on its stack top). The shared mutable
state — `m_Externals`, the InstanceID-to-LFID forward / reverse maps —
is therefore single-writer-per-instance by construction.

### 5.2 `PPtr<T>::Transfer()` body (replaces the empty stub)

```cpp
template<typename T>
template<typename TransferFunction>
inline void PPtr<T>::Transfer(TransferFunction& transfer)
{
    LocalSerializedObjectIdentifier lsoi {};
    if (transfer.NeedsInstanceIDRemapping())
    {
        if (transfer.IsWriting())
        {
            transfer.GetPPtrResolver()->InstanceIDToLSOI(m_InstanceID, lsoi);
        }
        transfer.Transfer(lsoi.localSerializedFileIndex, "m_FileID");
        transfer.Transfer(lsoi.localIdentifierInFile,    "m_PathID");
        if (transfer.IsReading())
        {
            m_InstanceID = transfer.GetPPtrResolver()->LSOIToInstanceID(lsoi);
        }
    }
    else
    {
        // Memory transfer (clipboard, undo) — preserve InstanceID directly.
        // Layout matches binary so a runtime swap doesn't break.
        int32_t fileIDInPlaceOfInstance = m_InstanceID;
        int64_t pathIDUnused            = 0;
        transfer.Transfer(fileIDInPlaceOfInstance, "m_FileID");
        transfer.Transfer(pathIDUnused,            "m_PathID");
        if (transfer.IsReading())
            m_InstanceID = fileIDInPlaceOfInstance;
    }
}
```

This matches Unity's `TransferPPtr` (`PPtr.h:346-386` in the Unity tree)
1:1 — same field names, same fallthrough, same dual-write to keep the
binary layout uniform across the remap-or-not boundary.

### 5.3 Touchpoints on existing code

| File | Change | Reason |
|---|---|---|
| `BaseClasses/PPtr.h` | Replace empty `Transfer()` body with §5.2 | The whole point |
| `Core/Serialize/SerializedFile.h` | `FileIdentifier` gets fields (guid, asset_type, has_path, path); add `m_Externals: std::vector<FileIdentifier>`; add `m_Resolver: IPPtrResolver*` (per-thread context, see §5.1.1); getter/setter; `AddExternalRef`; `GetExternalRefs` | Mirror Unity 277, 395, 400 |
| `Core/Serialize/SerializedFile.cpp` | (a) `BuildMetadataSection`: append externals block after object table; (b) `WriteHeaderAndMetadata`: set `header.version = 1`; (c) `ReadMetadata`: branch on version, populate `m_Externals` after object table when version >= 1 | Metadata-tail externals plumbing |
| `asset/asset_file.h` | **No change**. `k_zasset_version` stays at 1, `reserved[4]` stays untouched. The version bump is in `SerializedFileHeader.version` (inner header), not the ZASS prefix. | Outer prefix stays binary-compatible |
| `Editor/asset_registry/asset_registry.cpp::scanSingleAsset` | **No change**. Reads the unchanged 176-byte ZASS prefix only. | Outer prefix stays binary-compatible |
| `Resource/Asset/AssetManager.cpp` | In `WriteFile` and `ReadObject(path,type)` open a `ScopedPPtrResolver` over a `SerializedFilePPtrResolver` bound to the SerializedFile, for the duration of the read/write call (see §5.1.1) | Wires the per-thread resolver context |
| New: `BaseClasses/IPPtrResolver.h` | Interface from §5.1 | New |
| New: `Core/Serialize/SerializedFilePPtrResolver.{h,cpp}` | Concrete impl | New |

Estimated diff: ~600 LoC, **0 file moves/renames**, **0 schema changes to
existing res_types** (PR-SE3a-refine adds capability only; it does NOT
flip `m_shader` to `PPtr<ShaderRes>` — that's PR-SE3a-migrate).

---

## 6. Migration path for `MaterialRes::m_shader`

### 6.1 Why we don't flip the field type in this PR

`MaterialRes::m_shader` is currently `eastl::string`. Flipping it directly
to `PPtr<ShaderRes>` would mean **a TypeTree node changes type from
`string` to `{int32, int64}` pair**. SafeBinaryRead's schema-evolution
implementation today handles three transitions: drop-field, add-field,
reorder-field — see scenarios S2/S3/S4 in
`engine/Source/Runtime/Core/Serialize/test/schema_evolution_smoke_test.cpp`.
It does **not** handle "field with same name changed type" because that is
an inherently lossy conversion (a string holding `"StandardLit"` cannot
mechanically become a `(fileID, pathID)` pair without an out-of-band table
mapping name → guid → InstanceID).

The cleaner staging is **PR-SE3a-shadow** (already landed) → **this PR
(PR-SE3a-refine)** → **PR-SE3a-migrate**:

1. ✅ **PR-SE3a-shadow** (landed): added `m_shader_guid: eastl::string`
   as a sibling, preferred over `m_shader` when non-empty.
2. 🟡 **PR-SE3a-refine** (this doc): build the PPtr binary contract +
   resolver. **Does not modify any res_type.** Provable end-to-end via a
   new `PptrSmokeTest.cpp` (see §7) using a synthetic test class.
3. ✅ **PR-SE3a-migrate + PR-SE3b** (DONE, merged):
   `MaterialRes` now has `PPtr<ShaderRes> m_shader_pptr` alongside the
   legacy `m_shader` string (shadow phase -- both written, reader prefers
   PPtr when valid, falls back to `m_shader`). `GetShaderName()` /
   `SetShaderByName()` accessors encapsulate resolution. Downstream
   consumers switched to `GetShaderName()`. `ShaderImporter` does
   first-time seeding of `ShaderRes .zasset` files from `Shaders/*.shader`
   (A2 strategy, no continuous watch). Name resolution reuses
   `AssetManager::getAssetsByType("ShaderRes")` instead of a separate
   ShaderRegistry JSON (B2 strategy). M3a + M3b scenarios in
   `schema_evolution_smoke_test.cpp` cover old-layout→new read and
   PPtr round-trip. All 7 smoke-test scenarios pass.

### 6.2 Read priority on legacy assets

After PR-SE3a-migrate, the resolution priority for which shader a material
binds to becomes:

```
1. m_shader_pptr.IsValid()           -> use it (post-migrate writes)
2. m_shader != ""                    -> name-based lookup (legacy + built-in, via GetShaderName())
3. fallback "StandardLit"            -> default shader
```

This priority lets us hot-cut from name to PPtr **per-asset on each
re-save**, without a global migration tool.

---

## 7. Test plan

### 7.1 New: `PptrSmokeTest.cpp`

Lives next to `schema_evolution_smoke_test.cpp` at
`engine/Source/Runtime/Core/Serialize/Test/PptrSmokeTest.cpp`. Six
scenarios:

| # | Name | What it proves |
|---|---|---|
| P1 | Local PPtr round-trip | A PPtr to an Object in the **same** file: `m_FileID=0`, `m_PathID=lfid`. Read back resolves to original Object. |
| P2 | External PPtr round-trip | Save object A in file fA (PPtr<B> field) referencing object B in file fB. Reload fA, dereference PPtr — auto-loads fB and resolves. |
| P3 | Externals table dedup | Two PPtr<X> fields pointing to the same target file — `m_Externals.size() == 1`. |
| P4 | Null PPtr | A PPtr default-constructed (m_InstanceID=0) writes `m_FileID=0, m_PathID=0`, reads back IsNull(). |
| P5 | Backward compat: legacy file | Manually craft a SerializedFile blob whose inner `SerializedFileHeader.version = 0` and whose metadata buffer ends right after the object table (no externals tail). Confirm reader doesn't crash and `m_Externals` is empty. |
| P6 | Forward compat: dangling external | Save fA referencing fB, delete fB before reload. PPtr resolves to null + a single warning, no crash. |
| P7 | Concurrent read on N threads | Spawn 8 worker threads, each opens a different fA<sub>i</sub> -> fB<sub>i</sub> pair built in setup, runs round-trip 100 iterations. Asserts (a) all 800 reads see correct GUIDs; (b) no data race on `m_Externals` (run with `-fsanitize=thread` in CI when available); (c) `GetCurrentPPtrResolver()` on each worker is restored to `nullptr` after the outer scope drops. This is the test that proves the §5.1.1 TLS contract. |

### 7.2 Regression coverage

`schema_evolution_smoke_test.cpp` continues to run as-is — its 7 scenarios
exercise the SafeBinaryRead schema-evolution path, including M3a (old layout
without `m_shader_pptr` → new `MaterialRes` with PPtr reads back, PPtr null,
`GetShaderName()` falls back to `m_shader`) and M3b (PPtr round-trip null,
surrounding fields intact).

### 7.3 Manual editor smoke

After this PR lands but before PR-SE3a-migrate:

1. Open `I:\ZEngineDemo\ZEngineDemo.zproject`.
2. Inspect any `.zasset` material — should still load identically (its
   `MaterialRes` has zero `PPtr` fields today, so v2 header writes
   `externals_count=0` and the file is byte-identical apart from the
   bumped version field).
3. Save a material — diff against pre-PR save: only `header.version`
   changes from `1` → `2`, and the four bytes at
   `offsetof(AssetFileHeader, externals_offset)` change from `0` to a
   real offset (still pointing to a count=0 block).

---

## 8. Risk register

| Risk | Mitigation |
|---|---|
| Header v2 writer accidentally breaks `scanSingleAsset` reader | Outer ZASS prefix is **not modified** in this PR (see §3 fact-check). The version bump lives in `SerializedFileHeader.version` (inside the metadata buffer at offset 176), and `scanSingleAsset` only ever reads bytes [0..176). Belt-and-braces test: `PptrSmokeTest.cpp` P5 boots `AssetRegistry` against a freshly-written .zasset that has externals and asserts the registry still indexes the file's GUID correctly. |
| `m_Externals` accumulates stale entries across re-saves | Rebuild `m_Externals` from scratch on every `WriteObject` — never carry entries from the prior load. Same as Unity. |
| Circular asset references | Resolver detects in-flight loads via a thread-local set, returns the partially-constructed Object* (matches Unity). Document explicitly. |
| Diff size scope-creep | Hard cap: this PR may not modify any file under `Resource/res_type/**` or `Function/**`. Anyone tempted to "while I'm here, flip MaterialRes::m_shader" must move that to PR-SE3a-migrate. |
| GUID re-derivation on rename breaks dangling refs | Already guaranteed by `SerializedFile::SetAssetGuid` semantics — GUID is path-derived but the AssetRegistry pins `path↔guid` once stamped. Test P6 covers the dangling case. |

---

## 9. Out of scope (explicitly)

* **Sub-assets**: Unity's LFID scheme allows multiple Objects per file
  (sub-meshes inside an .fbx, multiple materials inside an imported model,
  etc.). ZEngine has one Object per `.zasset` today, so we hard-code
  `m_PathID = 1` on the writer side. Sub-asset support is a separate
  design (PR-SE-future).
* **YAML / text serialization of PPtr**: scenes are not YAML in ZEngine.
  If text scenes ever land, the YAML form follows Unity's
  `{fileID, guid, type}` triplet trivially because the in-memory
  representation is already `LocalSerializedObjectIdentifier`.
* **Player build** (path-mode externals): `has_path=1` codepath is
  designed in §4.3 but not implemented. Add when the first standalone
  player build needs to ship.
* **`PPtr` in serialized C# managed types** (Unity calls these
  `ScriptTypes`): we don't have a managed scripting layer; TypeScript
  components reference assets through `m_shader_guid`-style strings, not
  through `PPtr`. Out of scope until ScriptingManager grows GUID-typed
  fields.

---

## 10. Approval checklist

Before code lands, the following must be signed off:

- [x] §4.2 `AssetFileHeader` layout change — does the 32-byte reuse plan
      conflict with any in-flight feature touching `reserved[]`?
      **Resolved 2026-05-19**: no conflict. Proceed with `externals_offset:u64
      + externals_count:u32` reuse, 176-byte total preserved.
- [x] §5.1 `IPPtrResolver` interface — is the (single resolver per
      SerializedFile) granularity right, or do we need per-thread?
      **Resolved 2026-05-19**: per-thread (TLS) — see §5.1.1 for the
      `ScopedPPtrResolver` RAII contract and the rationale (multi-threaded
      AssetRegistry scan and AssetManager bulk loads). Test P7 in §7.1
      validates the TLS contract end-to-end.
- [x] §6 migration timeline — is the 3-PR split (shadow → refine →
      migrate) acceptable, or do we want to fold migrate into refine?
      **Resolved 2026-05-19**: keep split. Refine is testable in isolation
      via §7.1's synthetic test class; migrate touches one res_type and
      adds one schema-evolution scenario.
- [x] §7.1 test scenarios — any missing failure mode?
      **Resolved 2026-05-19**: P1-P7 accepted. P5/P6 cover version downgrade
      and dangling target; P3 covers dedup; P7 covers the per-thread
      resolver contract. These are the four bug classes Unity has
      historically had in this code path.

---

## 11. Cross-references

* **Schema evolution**: `doc/asset_management/SCHEMA_EVOLUTION_AND_TYPETREE.md`
* **Asset GUID derivation**: P2 #6 comment block in
  `engine/Source/Runtime/Core/Serialize/SerializedFile.h:102-129`
* **Unity TransferPPtr**: `../unity2023.1/Runtime/BaseClasses/PPtr.h:346-386`
* **Unity FileIdentifier**: `../unity2023.1/Runtime/Serialize/SerializedFile.h:31-80`
* **Unity reference-serialization spec**: `../unity2023.1/Documentation/InternalDocs/docs/Runtime/docs/ObjectTracking/serialization-of-references.md`
