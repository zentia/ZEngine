// =====================================================================
// PR-SE2: Schema-evolution smoke test for `.zasset` SerializedFile I/O.
// ---------------------------------------------------------------------
// Standalone executable that proves PR-SE1's Stage 2 change to
// `SerializedFile::ReadObject` -- namely, switching the read path to
// `SafeBinaryRead` when a per-type TypeTree is present in the file's
// metadata section -- actually delivers schema evolution at runtime.
//
// Why a standalone target instead of plumbing into a unit-test harness:
//   - The engine doesn't ship a test harness yet; precedent is
//     `dx12_bindless_smoke_test` / `vulkan_bindless_smoke_test` /
//     `shader_lab_test` -- all manually-invoked main()s opt-in
//     behind a CMake `option()`.
//   - The test deliberately runs OUTSIDE the editor / launcher: it
//     only needs the Core systems (TypeManager, ObjectManager,
//     TypeTreeCache, MemoryManager, FileSystem) wired up via
//     `RegisterCore()`. No window, no RHI, no asset registry.
//   - The demo project's `.zasset` files (e.g. ZEngineDemo) are NOT
//     SerializedFile-format -- their magic word is 0x00000000 and
//     they're an asset-bundle/JSON-index format that bypasses
//     SerializedFile entirely. So we can't validate the new code
//     path by "just opening the demo project". We need to author
//     and consume bytes ourselves, here.
//
// What this test proves (6 scenarios):
//   S1 round-trip:    write+read with identical Transfer() layout ->
//                     all fields exact-match.
//   S2 drop field:    write with N fields, read with (N-1) Transfer()
//                     layout -> survives, surviving fields exact-match,
//                     dropped field is silently skipped.
//   S3 add field:     write with N fields, read with (N+1) Transfer()
//                     layout -> survives, original fields exact-match,
//                     newly-added field is default-initialised (0 / "").
//   S4 reorder:       write with order [a,b,c], read with Transfer()
//                     order [c,a,b] -> survives, every field matches by
//                     NAME (not by position). This is the load-bearing
//                     property of TypeTree-driven reads.
//   M1 (PR-SE3a):     real MaterialRes round-trip with both m_Shader
//                     and m_ShaderGuid populated. Guards against
//                     positioning regressions when the schema gains a
//                     new field.
//   M2 (PR-SE3a):     write a MaterialRes-shaped .zasset using the OLD
//                     pre-PR-SE3a layout (no m_ShaderGuid), read it
//                     back as the new MaterialRes. Asserts m_Shader is
//                     intact and m_ShaderGuid stays at default "" --
//                     the binary-compat contract for MaterialRes
//                     schema extensions.
//
// What this test does NOT cover:
//   - Pre-PR-SE1 `.zasset` files lacking a TypeTree (the
//     StreamedBinaryRead fallback branch). That branch is the
//     "nothing changed" path of Stage 2 -- it's exercised by every
//     existing in-tree consumer that already runs through SerializedFile
//     today, and synthesising a TypeTree-stripped on-disk byte stream
//     here would require either reaching into private metadata
//     plumbing or post-processing the file bytes. Both are
//     disproportionate to what the fallback branch actually adds (a
//     log line + the literal old code). Future work: a unit test that
//     hex-edits the metadata blob length to 0 and asserts the log
//     appears. Out of scope for PR-SE2.
//   - Validation that header layout is unchanged. Covered transitively:
//     the writer side hasn't been touched in PR-SE1, and ZASS-prefix /
//     SerializedFileHeader code paths are unmodified. If those broke
//     S1 wouldn't even round-trip.
//
// Exit codes:
//   0  -- all four scenarios passed.
//   1  -- at least one scenario failed (test failure -- investigate).
//   77 -- environmental (file IO, system bring-up) failure -- skipped,
//         not a regression of the change under test.
//
// Build: only when `-DZENGINE_BUILD_SCHEMA_EVOLUTION_SMOKE_TEST=ON` is
// passed to CMake. Default OFF, no normal build is affected. See the
// CMakeLists block right next to this file.
// =====================================================================

// Pull in Object's serialisation infrastructure. Object.h itself only
// forward-declares the four Transfer types; IMPLEMENT_OBJECT_SERAILIZE
// expands `transfer.TransferBase(*this)` for each of them, so we need
// the full definitions here (pch.h covers EASTL / containers / mimalloc
// / etc., but does NOT cover Transfer-class headers -- those are
// runtime-internal and we include them explicitly).
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectDefines.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/JsonSerialize/JSONRead.h"
#include "Runtime/Core/JsonSerialize/JSONWrite.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedReader.h"
#include "Runtime/Core/Serialize/SerializationCaching/CachedWriter.h"
#include "Runtime/Core/Serialize/SerializationCaching/FileCacherWrite.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Core/Serialize/SerializedFile.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"
#include "Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryWrite.h"

// PR-SE3a: real-world MaterialRes coverage. We exercise the schema-aware
// read path on the actual production res_type whose schema we just
// extended (added m_ShaderGuid alongside the legacy m_Shader). The
// goal is twofold:
//   * prove that loading a .zasset written by today's MaterialRes
//     round-trips perfectly (no off-by-one offset bug from the new field);
//   * prove that loading a .zasset written by a "previous version" of
//     MaterialRes (one without m_ShaderGuid) succeeds, with m_Shader
//     intact and m_ShaderGuid left at its default empty string.
// The second case is what really matters -- it's the contract every
// PR-SE2 future schema migration depends on.
#include "RegisterRuntime.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/Shader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

// =====================================================================
// Test-only Object subclasses.
//
// Each class below is a standalone Object that the SerializedFile read
// /write path treats as a first-class registered type. The four
// classes are *deliberately distinct types* rather than one type with
// multiple Transfer() overloads, because:
//
//   * SerializedFile keys metadata's TypeTree blob by `Type*` -- two
//     different on-disk Transfer() layouts MUST come from two
//     different `Type` registrations. Bolting two Transfer()s onto
//     one C++ class would break the cache and is alien to how the
//     engine works in practice (every res_type/data file is one
//     class, one Transfer()).
//   * Each class therefore models a "version of a schema". This is
//     exactly the situation a real material/prefab would face after
//     a developer renames or adds a field across a release.
//
// A subtlety: SafeBinaryRead matches fields by NAME (via TypeTree
// node `m_Name`). For schema evolution to work, all four classes
// MUST use IDENTICAL Transfer() string keys for fields they share.
// We name them "id" / "alpha" / "beta" / "gamma" verbatim across
// every class.
//
// Another subtlety: REGISTER_CLASS / IMPLEMENT_REGISTER_CLASS expand
// using `TYPE_NAME_::Foo` token-pasted into namespace-prefixed symbol
// names. Wrapping these in a `namespace SchemaEvoTest {}` block
// breaks IMPLEMENT_REGISTER_CLASS (it tries to reach
// `SchemaEvoTest::RowFoo_TypeRegistrationDesc`, which doesn't exist
// because the macro pastes it as `_TypeRegistrationDesc` at global
// scope). That matches every existing res_type/data class -- they
// all live at global scope. To stay consistent and to keep the
// macro tooling happy we follow the same convention: prefix the
// names with `SchemaEvo` instead of namespacing them.
// =====================================================================

// ---- V0_AB:  schema  { "id", "alpha", "beta" } ----
//
// This is the "old" version we'll WRITE TO DISK in scenarios 2/3,
// and also the schema we'll round-trip in scenario S1 (write V0,
// read V0 -> exact match).
class SchemaEvoRowV0 : public Object
{
    REGISTER_CLASS(SchemaEvoRowV0)
    DECLARE_OBJECT_SERIALIZE(SchemaEvoRowV0)

public:
    eastl::string id;
    int32_t alpha {0};
    float beta {0.0f};
};

// ---- V1_ABC:  schema  { "id", "alpha", "beta", "gamma" } ----
//
// Adds a "gamma" field after the original three. Used as the READ
// schema in S3 (add-field): the bytes on disk only contain the
// first three; SafeBinaryRead must default-initialise gamma.
class SchemaEvoRowV1AddField : public Object
{
    REGISTER_CLASS(SchemaEvoRowV1AddField)
    DECLARE_OBJECT_SERIALIZE(SchemaEvoRowV1AddField)

public:
    eastl::string id;
    int32_t alpha {0};
    float beta {0.0f};
    int32_t gamma {-12345};  // sentinel != default -- proves init
};

// ---- V2_A:  schema  { "id", "alpha" } ----
//
// Drops the "beta" field. Used as the READ schema in S2
// (drop-field): the bytes on disk DO contain "beta"; SafeBinaryRead
// must skip it cleanly without corrupting subsequent reads.
class SchemaEvoRowV2DropField : public Object
{
    REGISTER_CLASS(SchemaEvoRowV2DropField)
    DECLARE_OBJECT_SERIALIZE(SchemaEvoRowV2DropField)

public:
    eastl::string id;
    int32_t alpha {0};
};

// ---- V3_REORDER:  schema  { "beta", "id", "alpha" } ----
//
// Identical field set as V0 but Transfer()'d in a different order.
// Used as the READ schema in S4: SafeBinaryRead must resolve every
// field by name, not by file offset, and the resulting object must
// hold values identical to S1.
class SchemaEvoRowV3Reorder : public Object
{
    REGISTER_CLASS(SchemaEvoRowV3Reorder)
    DECLARE_OBJECT_SERIALIZE(SchemaEvoRowV3Reorder)

public:
    eastl::string id;
    int32_t alpha {0};
    float beta {0.0f};
};

// =====================================================================
// PR-SE3a MaterialRes coverage.
// ---------------------------------------------------------------------
// `MaterialResLikeOld` is a deliberate replica of the *previous* version
// of MaterialRes (i.e. before PR-SE3a added the `m_ShaderGuid` field).
// We use it to PRODUCE on-disk bytes that look exactly like a .zasset
// written by an older build of the engine -- the writer embeds this
// class's TypeTree (no shader_guid node) into the file metadata, and
// the byte layout matches what real ZEngineDemo material .zasset files
// produced before this PR contain. We then read the same bytes back
// using the new MaterialRes class and assert:
//   * m_Shader survives byte-for-byte;
//   * m_ShaderGuid is left at the default empty string (SafeBinaryRead
//     returned kNotFound for the missing node and never wrote anything
//     into the field);
//   * every other surviving field matches.
//
// This is the production smoke-test for the schema-evolution contract:
// any old .zasset on disk must remain loadable after a MaterialRes
// schema extension.
//
// Critical invariants:
//   * Every Transfer() string key MUST match the corresponding key in
//     the real MaterialRes::Transfer(). If a key drifts here, the
//     "schema evolution" the test claims to exercise becomes "schema
//     replacement", which is a different (uninteresting) thing.
//   * Field types and order through Transfer() must match too: the
//     basic types are read positionally inside their respective
//     TypeTree nodes, and the Transfer order seeds the parent node's
//     child-list order, which SafeBinaryRead's first-pass linear scan
//     uses to skip ahead efficiently. A mismatch here would just cost
//     us perf, not correctness, but it'd be a misleading test.
//   * Sub-struct types (MaterialFloatProperty, MaterialColorProperty,
//     etc.) are reused VERBATIM from Material.h -- redefining them here
//     would generate two type registrations for the same name string
//     and break TypeManager's uniqueness check at static init time.
//     We #include "Runtime/Resource/ResType/Data/Material.h" above
//     to pull them in.
// =====================================================================
class MaterialResLikeOld : public Object
{
    REGISTER_CLASS(MaterialResLikeOld)
    DECLARE_OBJECT_SERIALIZE(MaterialResLikeOld)

public:
    eastl::string m_Shader {"StandardLit"};
    // NOTE: NO m_ShaderGuid here -- this is precisely what makes this
    // class "the old layout".
    std::vector<MaterialFloatProperty> m_FloatProperties;
    std::vector<MaterialColorProperty> m_ColorProperties;
    std::vector<MaterialTextureProperty> m_TextureProperties;
    std::vector<MaterialToggleProperty> m_ToggleProperties;

    Vector3 m_BaseColorFactor {1.0f, 1.0f, 1.0f};

    float m_AlphaFactor {1.0f};
    float m_MetallicFactor {1.0f};
    float m_RoughnessFactor {1.0f};
    float m_NormalScale {1.0f};
    float m_OcclusionStrength {1.0f};
    Vector3 m_EmissiveFactor {0.0f, 0.0f, 0.0f};
    bool m_IsBlend {false};
    bool m_IsDoubleSided {false};

    eastl::string m_BaseColourTextureFile;
    eastl::string m_MetallicRoughnessTextureFile;
    eastl::string m_NormalTextureFile;
    eastl::string m_OcclusionTextureFile;
    eastl::string m_EmissiveTextureFile;
};

// ---------------------------------------------------------------------
// Texture2D schema-evolution coverage (Phase 1 of the texture cook).
//
// `Texture2DLikeOld` replicates the PRE-mip Texture2D layout
// (width/height/format/pixels, no "mip_offsets" node) so we can produce
// bytes exactly like a .zasset written before the mip schema bump, then
// read them back with the new Texture2D and assert the new m_MipOffsets
// field stays empty (kNotFound) and the accessors degrade to a single
// mip 0 covering the whole blob. Transfer string keys MUST match the
// real Texture2D::Transfer() for the shared fields.
// ---------------------------------------------------------------------
class Texture2DLikeOld : public Object
{
    REGISTER_CLASS(Texture2DLikeOld)
    DECLARE_OBJECT_SERIALIZE(Texture2DLikeOld)

public:
    uint32_t m_Width {0};
    uint32_t m_Height {0};
    uint32_t m_Format {0};
    std::vector<uint8_t> m_Pixels;
    // NOTE: NO m_MipOffsets here -- that absence is what makes it "old".
};

// ---------------------------------------------------------------------
// Implementations + Transfer<>() bodies.
//
// IMPLEMENT_OBJECT_SERAILIZE (engine-internal typo, kept for
// consistency with every other res_type/data file -- see ScriptAsset.cpp's
// note) wires up VirtualRedirectTransfer for JSON / binary paths.
// INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED forces the Transfer<> bodies
// to be emitted in this TU for StreamedBinaryWrite / StreamedBinaryRead /
// SafeBinaryRead / GenerateTypeTreeTransfer -- without it, the writer
// would fail to find a Transfer specialisation at link time.
// ---------------------------------------------------------------------
IMPLEMENT_REGISTER_CLASS(SchemaEvoRowV0)
IMPLEMENT_OBJECT_SERAILIZE(SchemaEvoRowV0)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SchemaEvoRowV0)

template<typename TransferFunction>
void SchemaEvoRowV0::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(id, "id");
    transfer.Transfer(alpha, "alpha");
    transfer.Transfer(beta, "beta");
}

IMPLEMENT_REGISTER_CLASS(SchemaEvoRowV1AddField)
IMPLEMENT_OBJECT_SERAILIZE(SchemaEvoRowV1AddField)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SchemaEvoRowV1AddField)

template<typename TransferFunction>
void SchemaEvoRowV1AddField::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(id, "id");
    transfer.Transfer(alpha, "alpha");
    transfer.Transfer(beta, "beta");
    transfer.Transfer(gamma, "gamma");
}

IMPLEMENT_REGISTER_CLASS(SchemaEvoRowV2DropField)
IMPLEMENT_OBJECT_SERAILIZE(SchemaEvoRowV2DropField)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SchemaEvoRowV2DropField)

template<typename TransferFunction>
void SchemaEvoRowV2DropField::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(id, "id");
    transfer.Transfer(alpha, "alpha");
}

IMPLEMENT_REGISTER_CLASS(SchemaEvoRowV3Reorder)
IMPLEMENT_OBJECT_SERAILIZE(SchemaEvoRowV3Reorder)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SchemaEvoRowV3Reorder)

template<typename TransferFunction>
void SchemaEvoRowV3Reorder::Transfer(TransferFunction& transfer)
{
    // Deliberately reordered: beta first, then id, then alpha.
    transfer.Transfer(beta, "beta");
    transfer.Transfer(id, "id");
    transfer.Transfer(alpha, "alpha");
}

// ---------------------------------------------------------------------
// MaterialResLikeOld implementation.
//
// CRITICAL: every Transfer() call below must use the EXACT SAME string
// key as the corresponding call in `MaterialRes::Transfer()` (see
// Material.cpp). The whole point of this mock is to produce on-disk
// bytes that the new MaterialRes can read field-for-field by name, so
// any key drift would defeat the test.
//
// The ONLY intentional difference vs MaterialRes::Transfer() is the
// absence of the `transfer.Transfer(m_ShaderGuid, "shader_guid")`
// call -- this is what makes the produced bytes look "old".
// ---------------------------------------------------------------------
IMPLEMENT_REGISTER_CLASS(MaterialResLikeOld)
IMPLEMENT_OBJECT_SERAILIZE(MaterialResLikeOld)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(MaterialResLikeOld)

IMPLEMENT_REGISTER_CLASS(Texture2DLikeOld)
IMPLEMENT_OBJECT_SERAILIZE(Texture2DLikeOld)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Texture2DLikeOld)

template<typename TransferFunction>
void Texture2DLikeOld::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Width, "width");
    transfer.Transfer(m_Height, "height");
    transfer.Transfer(m_Format, "format");
    transfer.Transfer(m_Pixels, "pixels");
    // ^^ NO "mip_offsets" -- that absence is the "old layout".
}

template<typename TransferFunction>
void MaterialResLikeOld::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shader, "shader");
    // ^^ NO m_ShaderGuid here. That is the "old" part of "old layout".
    transfer.Transfer(m_FloatProperties, "float_properties");
    transfer.Transfer(m_ColorProperties, "color_properties");
    transfer.Transfer(m_TextureProperties, "texture_properties");
    transfer.Transfer(m_ToggleProperties, "toggle_properties");
    transfer.Transfer(m_BaseColorFactor, "base_color_factor");
    transfer.Transfer(m_AlphaFactor, "alpha_factor");
    transfer.Transfer(m_MetallicFactor, "metallic_factor");
    transfer.Transfer(m_RoughnessFactor, "roughness_factor");
    transfer.Transfer(m_NormalScale, "normal_scale");
    transfer.Transfer(m_OcclusionStrength, "occlusion_strength");
    transfer.Transfer(m_EmissiveFactor, "emissive_factor");
    transfer.Transfer(m_IsBlend, "is_blend");
    transfer.Transfer(m_IsDoubleSided, "is_double_sided");
    transfer.Transfer(m_BaseColourTextureFile, "base_colour_texture_file");
    transfer.Transfer(m_MetallicRoughnessTextureFile, "metallic_roughness_texture_file");
    transfer.Transfer(m_NormalTextureFile, "normal_texture_file");
    transfer.Transfer(m_OcclusionTextureFile, "occlusion_texture_file");
    transfer.Transfer(m_EmissiveTextureFile, "emissive_texture_file");
}

// =====================================================================
// Test harness primitives.
// ---------------------------------------------------------------------
// Cache size is the same `kCacheSizeForWriting` AssetManager uses --
// 1 MiB, which is comfortably larger than anything we round-trip in
// these tests but matches the production reader's expectations.
// =====================================================================
namespace
{
    constexpr size_t kCacheSize = 1024 * 1024;

    int g_failures = 0;

    void reportFail(const char* scenario, const char* detail)
    {
        ++g_failures;
        std::fprintf(stderr,
                     "[FAIL] %s: %s\n",
                     scenario,
                     detail);
    }

    void reportOK(const char* scenario)
    {
        std::fprintf(stdout, "[ OK ] %s\n", scenario);
    }

    // Row arguments are passed by pointer rather than reference because
    // every test row MUST be heap-allocated through MemoryManager::
    // CreateObject<T>(): that is the only path that calls
    // Object::InitializeRuntimeTypeInfo(), which seeds m_CachedTypeIndex.
    // A stack-allocated Object subclass leaves m_CachedTypeIndex at
    // garbage and the very first call to row->GetType() access-violates
    // when it indexes TypeManager::GetRuntimeTypes().types[<garbage>].
    // Taking T* here makes that contract impossible to violate
    // accidentally from a scenario.
    template<typename T>
    bool writeRowToFile(const std::filesystem::path& path, T* row)
    {
        std::filesystem::create_directories(path.parent_path());

        FileCacherWrite cacher;
        if (!cacher.InitWriteFile(path, kCacheSize))
        {
            std::fprintf(stderr,
                         "writeRowToFile: failed to open %s for writing\n",
                         path.generic_string().c_str());
            return false;
        }

        CachedWriter writer;
        writer.InitWrite(cacher);

        SerializedFile* sf = MemoryManager::CreateObject<SerializedFile>();
        sf->InitializeWrite(writer);
        // Path-derived deterministic GUID is fine for tests -- we never
        // query it back, but the writer requires SOMETHING here so the
        // ZASS prefix construction doesn't synthesise a divergent default.
        sf->SetAssetGuid(std::string("00000000000000000000000000000000"));
        if (row->GetType() != nullptr && row->GetType()->GetName() != nullptr)
        {
            sf->SetAssetTypeName(std::string(row->GetType()->GetName()));
        }

        sf->WriteObject(*row, /*fileID=*/1);

        FileSize dataOffset {};
        const bool ok = sf->FinishWriting(&dataOffset);
        MemoryManager::DestroyObject(sf);
        return ok;
    }

    template<typename T>
    bool readRowFromFile(const std::filesystem::path& path, T* outRow)
    {
        SerializedFile* sf = MemoryManager::CreateObject<SerializedFile>();
        if (sf->InitializeRead(path, kCacheSize) != kSerializedFileLoadError_None)
        {
            std::fprintf(stderr,
                         "readRowFromFile: InitializeRead failed for %s\n",
                         path.generic_string().c_str());
            MemoryManager::DestroyObject(sf);
            return false;
        }
        sf->ReadObject(/*fileID=*/1, *outRow);
        MemoryManager::DestroyObject(sf);
        return true;
    }

    // Reusable scratch path. Each scenario re-writes its own file so
    // leftover bytes from a previous run never matter.
    std::filesystem::path scratchFile(const char* tag)
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    "zengine_schema_evo_smoke";
        std::filesystem::create_directories(dir);
        return dir / (std::string("row_") + tag + ".zasset");
    }
}  // namespace

// =====================================================================
// Scenarios.
// =====================================================================

// S1: write V0(id="hello", alpha=42, beta=3.14), read V0 -> exact
//     match. This is the round-trip baseline that proves
//     SafeBinaryRead can at minimum reproduce StreamedBinaryRead's
//     behaviour when nothing has changed.
static void scenario_S1_round_trip()
{
    const auto path = scratchFile("s1");

    SchemaEvoRowV0* src = MemoryManager::CreateObject<SchemaEvoRowV0>();
    src->id = "hello";
    src->alpha = 42;
    src->beta = 3.14f;

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("S1 round-trip", "write failed");
        return;
    }

    SchemaEvoRowV0* dst = MemoryManager::CreateObject<SchemaEvoRowV0>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("S1 round-trip", "read failed");
        return;
    }

    const bool match = (dst->id == "hello" && dst->alpha == 42 && dst->beta == 3.14f);
    if (!match)
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "id='%s' alpha=%d beta=%.6f (expected 'hello' 42 3.14)", dst->id.c_str(), dst->alpha, static_cast<double>(dst->beta));
        reportFail("S1 round-trip", detail);
    }
    else
    {
        reportOK("S1 round-trip");
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

// S2: write V0 on disk, read with V2 schema (no "beta" field) ->
//     surviving fields ("id", "alpha") match exactly. Proves that
//     SafeBinaryRead skips fields the new code doesn't know about
//     without corrupting subsequent reads.
static void scenario_S2_drop_field()
{
    const auto path = scratchFile("s2");

    SchemaEvoRowV0* src = MemoryManager::CreateObject<SchemaEvoRowV0>();
    src->id = "drop-test";
    src->alpha = 7;
    src->beta = 99.5f;  // will be silently skipped by reader

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("S2 drop-field", "write failed");
        return;
    }

    SchemaEvoRowV2DropField* dst = MemoryManager::CreateObject<SchemaEvoRowV2DropField>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("S2 drop-field", "read failed");
        return;
    }

    const bool match = (dst->id == "drop-test" && dst->alpha == 7);
    if (!match)
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "id='%s' alpha=%d (expected 'drop-test' 7)", dst->id.c_str(), dst->alpha);
        reportFail("S2 drop-field", detail);
    }
    else
    {
        reportOK("S2 drop-field");
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

// S3: write V0 on disk, read with V1 schema (extra "gamma" field) ->
//     existing fields match, gamma is default-initialised. Proves that
//     adding a field doesn't break old files.
static void scenario_S3_add_field()
{
    const auto path = scratchFile("s3");

    SchemaEvoRowV0* src = MemoryManager::CreateObject<SchemaEvoRowV0>();
    src->id = "add-test";
    src->alpha = 11;
    src->beta = 2.5f;

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("S3 add-field", "write failed");
        return;
    }

    SchemaEvoRowV1AddField* dst = MemoryManager::CreateObject<SchemaEvoRowV1AddField>();
    // gamma's ctor sentinel = -12345 -- absent on disk, SafeBinaryRead
    // leaves the default-constructed value untouched. The sentinel
    // proves we did NOT silently overwrite with a garbage byte pattern
    // from the wrong file offset (the most common failure mode if
    // SafeBinaryRead were broken).
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("S3 add-field", "read failed");
        return;
    }

    const bool match = (dst->id == "add-test" && dst->alpha == 11 &&
                        dst->beta == 2.5f && dst->gamma == -12345);
    if (!match)
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "id='%s' alpha=%d beta=%.6f gamma=%d (expected 'add-test' "
                                              "11 2.5 -12345 [ctor-default])",
                      dst->id.c_str(),
                      dst->alpha,
                      static_cast<double>(dst->beta),
                      dst->gamma);
        reportFail("S3 add-field", detail);
    }
    else
    {
        reportOK("S3 add-field");
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

// S4: write V0 on disk (Transfer order id, alpha, beta), read with
//     V3 schema (Transfer order beta, id, alpha) -> all fields match
//     by NAME. Proves the schema-aware path doesn't degenerate into
//     positional reads when a developer reorders Transfer().
static void scenario_S4_reorder()
{
    const auto path = scratchFile("s4");

    SchemaEvoRowV0* src = MemoryManager::CreateObject<SchemaEvoRowV0>();
    src->id = "reorder-test";
    src->alpha = 555;
    src->beta = 1.0f / 3.0f;

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("S4 reorder", "write failed");
        return;
    }

    SchemaEvoRowV3Reorder* dst = MemoryManager::CreateObject<SchemaEvoRowV3Reorder>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("S4 reorder", "read failed");
        return;
    }

    const bool match = (dst->id == "reorder-test" && dst->alpha == 555 &&
                        dst->beta == (1.0f / 3.0f));
    if (!match)
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "id='%s' alpha=%d beta=%.9f (expected 'reorder-test' 555 0.333...)", dst->id.c_str(), dst->alpha, static_cast<double>(dst->beta));
        reportFail("S4 reorder", detail);
    }
    else
    {
        reportOK("S4 reorder");
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

// =====================================================================
// PR-SE3a: real-world MaterialRes scenarios.
// ---------------------------------------------------------------------
// M1: round-trip a MaterialRes with both m_Shader and m_ShaderGuid
//     filled. Proves the new field doesn't disturb writer / reader
//     positioning of the surrounding fields. This is structurally the
//     same proof S1 gives for SchemaEvoRowV0, but on the real production
//     class -- so a future schema bug in MaterialRes (e.g. someone adds
//     a third field with a typo'd key) trips here.
// M2: write bytes using the OLD layout (MaterialResLikeOld -- no
//     m_ShaderGuid), read them back using the new MaterialRes class.
//     This is the binary-compat smoke test for PR-SE3a's contract:
//     pre-existing .zasset files on disk must remain loadable. We
//     assert m_Shader survives byte-for-byte and m_ShaderGuid stays
//     at its default empty string (i.e. SafeBinaryRead returned
//     kNotFound for the missing field rather than reading garbage
//     from a wrong file offset).
// =====================================================================

static void scenario_M1_material_round_trip()
{
    const auto path = scratchFile("m1_material_rt");

    Material* src = MemoryManager::CreateObject<Material>();
    src->m_Shader = "MyCoolShader";
    src->m_ShaderGuid = "0123456789abcdef0123456789abcdef";
    src->m_ShaderPptr = PPtr<ShaderRes>();  // null in test (no real ShaderRes .zasset)
    src->m_AlphaFactor = 0.75f;
    src->m_MetallicFactor = 0.25f;
    src->m_IsBlend = true;

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("M1 material round-trip", "write failed");
        return;
    }

    Material* dst = MemoryManager::CreateObject<Material>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("M1 material round-trip", "read failed");
        return;
    }

    const bool match =
        (dst->m_Shader == "MyCoolShader" &&
         dst->m_ShaderGuid == "0123456789abcdef0123456789abcdef" &&
         dst->m_ShaderPptr.IsNull() &&
         dst->GetShaderName() == "MyCoolShader" &&
         dst->m_AlphaFactor == 0.75f &&
         dst->m_MetallicFactor == 0.25f &&
         dst->m_IsBlend == true);

    if (!match)
    {
        char detail[512];
        std::snprintf(detail, sizeof(detail), "m_shader='%s' m_shader_guid='%s' alpha=%.6f metallic=%.6f is_blend=%d",
                      dst->m_Shader.c_str(),
                      dst->m_ShaderGuid.c_str(),
                      static_cast<double>(dst->m_AlphaFactor),
                      static_cast<double>(dst->m_MetallicFactor),
                      dst->m_IsBlend ? 1 : 0);
        reportFail("M1 material round-trip", detail);
    }
    else
    {
        reportOK("M1 material round-trip");
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

static void scenario_M2_material_old_layout_compat()
{
    const auto path = scratchFile("m2_material_old");

    // Step 1: produce on-disk bytes that look like what an older build
    // of the engine (pre-PR-SE3a, without m_ShaderGuid) would write.
    // We're using MaterialResLikeOld, whose Transfer() omits the new
    // field on purpose. From the file's perspective, this is just a
    // .zasset whose embedded TypeTree has no "shader_guid" node.
    MaterialResLikeOld* src = MemoryManager::CreateObject<MaterialResLikeOld>();
    src->m_Shader = "Lit/Standard";  // distinctive sentinel
    src->m_AlphaFactor = 0.42f;
    src->m_MetallicFactor = 0.84f;
    src->m_IsDoubleSided = true;
    src->m_NormalTextureFile = "T_Norm.zasset";

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("M2 material old-layout-compat", "write failed");
        return;
    }

    // Step 2: read those bytes using the *new* MaterialRes class.
    // SafeBinaryRead must:
    //   * resolve every old field by name (m_Shader, m_AlphaFactor,
    //     etc.) -> success;
    //   * fail to find "shader_guid" -> return kNotFound -> leave
    //     m_ShaderGuid at its default empty string.
    // If the schema-aware path were broken (e.g. fell back to
    // positional StreamedBinaryRead for a renamed/added field), the
    // file's m_Shader bytes would land in some unrelated field of
    // MaterialRes and the assertion below would fail loudly.
    Material* dst = MemoryManager::CreateObject<Material>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("M2 material old-layout-compat", "read failed");
        return;
    }

    const bool old_fields_intact =
        (dst->m_Shader == "Lit/Standard" &&
         dst->m_AlphaFactor == 0.42f &&
         dst->m_MetallicFactor == 0.84f &&
         dst->m_IsDoubleSided == true &&
         dst->m_NormalTexturePptr.IsNull());

    // The CRITICAL assertion: m_ShaderGuid is the field added in
    // PR-SE3a. Old bytes don't contain it. Default-init must hold.
    const bool new_field_default = dst->m_ShaderGuid.empty();

    // PR-SE3a-migrate: m_ShaderPptr must also be null (kNotFound from
    // the even older pre-SE3a-migrate schema that didn't have this field).
    const bool pptr_null = dst->m_ShaderPptr.IsNull();

    // GetShaderName() must fall back to m_Shader when PPtr is null.
    const bool getter_ok = (dst->GetShaderName() == "Lit/Standard");

    if (!old_fields_intact || !new_field_default || !pptr_null || !getter_ok)
    {
        char detail[512];
        std::snprintf(detail, sizeof(detail), "m_shader='%s' m_shader_guid='%s' (expected empty) "
                                              "pptr_null=%d getter='%s' "
                                              "alpha=%.6f metallic=%.6f double_sided=%d",
                      dst->m_Shader.c_str(),
                      dst->m_ShaderGuid.c_str(),
                      pptr_null ? 1 : 0,
                      dst->GetShaderName().c_str(),
                      static_cast<double>(dst->m_AlphaFactor),
                      static_cast<double>(dst->m_MetallicFactor),
                      dst->m_IsDoubleSided ? 1 : 0);
        reportFail("M2 material old-layout-compat", detail);
    }
    else
    {
        reportOK("M2 material old-layout-compat");
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

// =====================================================================
// M3: string -> PPtr schema evolution (PR-SE3a-migrate).
//
// Simulates reading an old .zasset (written by the pre-SE3a-migrate
// engine that had m_Shader + m_ShaderGuid but NO m_ShaderPptr) using
// the new engine that has m_ShaderPptr. This proves:
//   * SafeBinaryRead returns kNotFound for the new "m_ShaderPptr" field
//     -> PPtr stays null (m_InstanceID == 0).
//   * The old "shader" and "shader_guid" string fields are still readable.
//   * GetShaderName() correctly falls back to m_Shader when PPtr is null.
//   * Writing a new .zasset produces a valid file with m_ShaderPptr
//     included (and readable on round-trip).
// =====================================================================

// "Old" MaterialRes layout: m_Shader + m_ShaderGuid, but NO m_ShaderPptr.
class MaterialResLikeOldNoPPtr : public Object
{
    REGISTER_CLASS(MaterialResLikeOldNoPPtr)
    DECLARE_OBJECT_SERIALIZE(MaterialResLikeOldNoPPtr)

public:
    eastl::string m_Shader {"StandardLit"};
    eastl::string m_ShaderGuid;
    // NOTE: NO m_ShaderPptr here -- this is the pre-SE3a-migrate layout.
    std::vector<MaterialFloatProperty> m_FloatProperties;
    std::vector<MaterialColorProperty> m_ColorProperties;
    std::vector<MaterialTextureProperty> m_TextureProperties;
    std::vector<MaterialToggleProperty> m_ToggleProperties;

    Vector3 m_BaseColorFactor {1.0f, 1.0f, 1.0f};

    float m_AlphaFactor {1.0f};
    float m_MetallicFactor {1.0f};
    float m_RoughnessFactor {1.0f};
    float m_NormalScale {1.0f};
    float m_OcclusionStrength {1.0f};
    Vector3 m_EmissiveFactor {0.0f, 0.0f, 0.0f};
    bool m_IsBlend {false};
    bool m_IsDoubleSided {false};

    eastl::string m_BaseColourTextureFile;
    eastl::string m_MetallicRoughnessTextureFile;
    eastl::string m_NormalTextureFile;
    eastl::string m_OcclusionTextureFile;
    eastl::string m_EmissiveTextureFile;
};

IMPLEMENT_REGISTER_CLASS(MaterialResLikeOldNoPPtr)
IMPLEMENT_OBJECT_SERAILIZE(MaterialResLikeOldNoPPtr)

template<typename TransferFunction>
void MaterialResLikeOldNoPPtr::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Shader, "shader");
    transfer.Transfer(m_ShaderGuid, "shader_guid");
    // NO m_ShaderPptr -- this is the "old" schema.
    transfer.Transfer(m_FloatProperties, "float_properties");
    transfer.Transfer(m_ColorProperties, "color_properties");
    transfer.Transfer(m_TextureProperties, "texture_properties");
    transfer.Transfer(m_ToggleProperties, "toggle_properties");

    transfer.Transfer(m_BaseColorFactor, "base_color_factor");
    transfer.Transfer(m_AlphaFactor, "alpha_factor");
    transfer.Transfer(m_MetallicFactor, "metallic_factor");
    transfer.Transfer(m_RoughnessFactor, "roughness_factor");
    transfer.Transfer(m_NormalScale, "normal_scale");
    transfer.Transfer(m_OcclusionStrength, "occlusion_strength");
    transfer.Transfer(m_EmissiveFactor, "emissive_factor");
    transfer.Transfer(m_IsBlend, "is_blend");
    transfer.Transfer(m_IsDoubleSided, "is_double_sided");

    transfer.Transfer(m_BaseColourTextureFile, "base_colour_texture_file");
    transfer.Transfer(m_MetallicRoughnessTextureFile, "metallic_roughness_texture_file");
    transfer.Transfer(m_NormalTextureFile, "normal_texture_file");
    transfer.Transfer(m_OcclusionTextureFile, "occlusion_texture_file");
    transfer.Transfer(m_EmissiveTextureFile, "emissive_texture_file");
}

INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(MaterialResLikeOldNoPPtr)

static void scenario_M3_material_pptr_evolution()
{
    // ---- Part A: old bytes → new class ----
    // Write bytes with the old (no-PPtr) layout, then read with the
    // new MaterialRes class that has m_ShaderPptr.
    {
        const auto path = scratchFile("m3a_old_to_pptr");

        MaterialResLikeOldNoPPtr* src = MemoryManager::CreateObject<MaterialResLikeOldNoPPtr>();
        src->m_Shader = "CustomShader";
        src->m_ShaderGuid = "aabbccdd11223344";
        src->m_AlphaFactor = 0.6f;
        src->m_IsBlend = true;

        if (!writeRowToFile(path, src))
        {
            MemoryManager::DestroyObject(src);
            reportFail("M3a old→pptr", "write failed");
        }
        else
        {
            Material* dst = MemoryManager::CreateObject<Material>();
            if (!readRowFromFile(path, dst))
            {
                reportFail("M3a old→pptr", "read failed");
            }
            else
            {
                // Old string fields survive the transition
                const bool strings_ok = (dst->m_Shader == "CustomShader" &&
                                         dst->m_ShaderGuid == "aabbccdd11223344");
                // PPtr is null (kNotFound from old schema)
                const bool pptr_null = dst->m_ShaderPptr.IsNull();
                // GetShaderName() falls back to m_Shader
                const bool getter_ok = (dst->GetShaderName() == "CustomShader");
                // Other fields also survived
                const bool rest_ok = (dst->m_AlphaFactor == 0.6f &&
                                      dst->m_IsBlend == true);

                if (strings_ok && pptr_null && getter_ok && rest_ok)
                {
                    reportOK("M3a old→pptr (legacy string survives, PPtr null, GetShaderName fallback)");
                }
                else
                {
                    char detail[512];
                    std::snprintf(detail, sizeof(detail), "m_shader='%s' m_shader_guid='%s' pptr_null=%d "
                                                          "getter='%s' alpha=%.6f blend=%d",
                                  dst->m_Shader.c_str(),
                                  dst->m_ShaderGuid.c_str(),
                                  pptr_null ? 1 : 0,
                                  dst->GetShaderName().c_str(),
                                  static_cast<double>(dst->m_AlphaFactor),
                                  dst->m_IsBlend ? 1 : 0);
                    reportFail("M3a old→pptr", detail);
                }
            }
            MemoryManager::DestroyObject(dst);
        }
        MemoryManager::DestroyObject(src);
    }

    // ---- Part B: new class round-trip ----
    // Write a MaterialRes with m_ShaderPptr (null — no actual ShaderRes
    // .zasset in the test's AssetManager), read it back. Proves the PPtr
    // field doesn't corrupt surrounding fields on write/read.
    {
        const auto path = scratchFile("m3b_pptr_roundtrip");

        Material* src = MemoryManager::CreateObject<Material>();
        src->m_Shader = "RoundTripShader";
        src->m_ShaderGuid = "deadbeefcafebabe";
        src->m_ShaderPptr = PPtr<ShaderRes>();  // null, no ShaderRes in test
        src->m_AlphaFactor = 0.33f;
        src->m_MetallicFactor = 0.77f;

        if (!writeRowToFile(path, src))
        {
            MemoryManager::DestroyObject(src);
            reportFail("M3b pptr round-trip", "write failed");
        }
        else
        {
            Material* dst = MemoryManager::CreateObject<Material>();
            if (!readRowFromFile(path, dst))
            {
                reportFail("M3b pptr round-trip", "read failed");
            }
            else
            {
                const bool match =
                    (dst->m_Shader == "RoundTripShader" &&
                     dst->m_ShaderGuid == "deadbeefcafebabe" &&
                     dst->m_ShaderPptr.IsNull() &&
                     dst->GetShaderName() == "RoundTripShader" &&
                     dst->m_AlphaFactor == 0.33f &&
                     dst->m_MetallicFactor == 0.77f);

                if (match)
                {
                    reportOK("M3b pptr round-trip (PPtr field round-trips null, surrounding fields intact)");
                }
                else
                {
                    char detail[512];
                    std::snprintf(detail, sizeof(detail), "m_shader='%s' m_shader_guid='%s' pptr_null=%d "
                                                          "getter='%s' alpha=%.6f metallic=%.6f",
                                  dst->m_Shader.c_str(),
                                  dst->m_ShaderGuid.c_str(),
                                  dst->m_ShaderPptr.IsNull() ? 1 : 0,
                                  dst->GetShaderName().c_str(),
                                  static_cast<double>(dst->m_AlphaFactor),
                                  static_cast<double>(dst->m_MetallicFactor));
                    reportFail("M3b pptr round-trip", detail);
                }
            }
            MemoryManager::DestroyObject(dst);
        }
        MemoryManager::DestroyObject(src);
    }
}

// T1: write a Texture2D using the OLD pre-mip layout (no "mip_offsets"),
//     read it back as the new Texture2D. The new m_MipOffsets must stay
//     empty (kNotFound) and the accessors must degrade to a single mip 0
//     spanning the whole pixel blob. This is the on-disk backward-compat
//     contract for every Texture2D .zasset authored before the mip bump.
static void scenario_T1_texture_old_layout_compat()
{
    const auto path = scratchFile("t1_texture_old");

    Texture2DLikeOld* src = MemoryManager::CreateObject<Texture2DLikeOld>();
    src->m_Width = 2;
    src->m_Height = 2;
    src->m_Format = 37;  // arbitrary RHIFormat ordinal sentinel (e.g. RGBA8_UNORM)
    src->m_Pixels = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};  // 2x2 RGBA8

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("T1 texture old-layout-compat", "write failed");
        return;
    }

    Texture2D* dst = MemoryManager::CreateObject<Texture2D>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("T1 texture old-layout-compat", "read failed");
        return;
    }

    const bool fields_intact = (dst->m_Width == 2 && dst->m_Height == 2 && dst->m_Format == 37 &&
                                dst->m_Pixels.size() == 16);
    // CRITICAL: the new field must be absent -> empty after read.
    const bool offsets_empty = dst->m_MipOffsets.empty();
    // Accessors degrade to a single mip 0 spanning the whole blob.
    const Texture2D::MipSpan mip0 = dst->GetMipSpan(0);
    const bool single_mip = (dst->GetMipCount() == 1 && mip0.data == dst->m_Pixels.data() && mip0.size == 16);
    const bool oob_safe = (dst->GetMipSpan(1).data == nullptr && dst->GetMipSpan(1).size == 0);

    if (fields_intact && offsets_empty && single_mip && oob_safe)
    {
        reportOK("T1 texture old-layout-compat (legacy single-mip RGBA8 loads, mip accessors degrade)");
    }
    else
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "w=%u h=%u fmt=%u pixels=%zu offsets_empty=%d mipcount=%u single=%d oob_safe=%d",
                      dst->m_Width, dst->m_Height, dst->m_Format, dst->m_Pixels.size(),
                      offsets_empty ? 1 : 0, dst->GetMipCount(), single_mip ? 1 : 0, oob_safe ? 1 : 0);
        reportFail("T1 texture old-layout-compat", detail);
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}

// T2: round-trip a multi-mip (compressed-format) Texture2D through the new
//     schema. Proves the appended "mip_offsets" field serialises and the
//     GetMipSpan() boundaries reconstruct exactly.
static void scenario_T2_texture_mip_round_trip()
{
    const auto path = scratchFile("t2_texture_mips");

    Texture2D* src = MemoryManager::CreateObject<Texture2D>();
    src->m_Width = 4;
    src->m_Height = 4;
    src->m_Format = 137;  // arbitrary BC7-like sentinel ordinal
    // Two mips: mip0 = 16 bytes (one BC7 block for 4x4), mip1 = 16 bytes
    // (a 2x2 still rounds up to one 4x4 block for BCn). Distinct byte
    // patterns so a boundary slip would be visible.
    src->m_Pixels.assign(32, 0);
    for (int i = 0; i < 16; ++i)
    {
        src->m_Pixels[i] = static_cast<uint8_t>(0xA0 + i);
        src->m_Pixels[16 + i] = static_cast<uint8_t>(0xB0 + i);
    }
    src->m_MipOffsets = {0, 16};

    if (!writeRowToFile(path, src))
    {
        MemoryManager::DestroyObject(src);
        reportFail("T2 texture mip round-trip", "write failed");
        return;
    }

    Texture2D* dst = MemoryManager::CreateObject<Texture2D>();
    if (!readRowFromFile(path, dst))
    {
        MemoryManager::DestroyObject(src);
        MemoryManager::DestroyObject(dst);
        reportFail("T2 texture mip round-trip", "read failed");
        return;
    }

    const Texture2D::MipSpan mip0 = dst->GetMipSpan(0);
    const Texture2D::MipSpan mip1 = dst->GetMipSpan(1);
    const bool meta_ok = (dst->m_Width == 4 && dst->m_Height == 4 && dst->m_Format == 137 &&
                          dst->GetMipCount() == 2 && dst->m_MipOffsets.size() == 2 &&
                          dst->m_MipOffsets[0] == 0 && dst->m_MipOffsets[1] == 16);
    const bool spans_ok = (mip0.size == 16 && mip1.size == 16 && mip0.data != nullptr && mip1.data != nullptr &&
                           mip0.data[0] == 0xA0 && mip1.data[0] == 0xB0 && mip1.data[15] == 0xBF);

    if (meta_ok && spans_ok)
    {
        reportOK("T2 texture mip round-trip (mip_offsets serialises, GetMipSpan boundaries exact)");
    }
    else
    {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "w=%u h=%u fmt=%u mipcount=%u offsets=%zu mip0=%zu mip1=%zu meta=%d spans=%d",
                      dst->m_Width, dst->m_Height, dst->m_Format, dst->GetMipCount(),
                      dst->m_MipOffsets.size(), mip0.size, mip1.size, meta_ok ? 1 : 0, spans_ok ? 1 : 0);
        reportFail("T2 texture mip round-trip", detail);
    }

    MemoryManager::DestroyObject(src);
    MemoryManager::DestroyObject(dst);
}
// ---------------------------------------------------------------------
// We bring up just the Core systems via RegisterCore() (TypeManager,
// ObjectManager, MemoryManager, TypeTreeCache, FileSystem,
// AsyncReadManagerThreaded, CommonStringTable, LLMTracker) -- this is
// the same scaffolding that production code relies on. The full
// runtime (RHI, world, scripting, etc.) is intentionally NOT brought
// up: SerializedFile is a Core-tier component and any test that
// requires more than RegisterCore() to exercise it would be exposing
// a layering bug.
// =====================================================================
int main()
{
    // Write to stderr for progress markers -- BqLog redirects stdout into
    // its own log channel, so anything we put on stdout never reaches the
    // console. stderr stays direct, which is what we want for "did we get
    // past line N?" debugging.
    std::fprintf(stderr,
                 "ZEngine schema-evolution smoke test (PR-SE2)\n"
                 "============================================\n");
    std::fflush(stderr);

    // Bring up the Core engine systems we depend on. RegisterCore()
    // also runs RegisterRuntimeType() -> AutoTypeRegistration
    // -> registers all REGISTER_CLASS-decorated types (including the
    // four test rows above, since their static auto-registrar
    // instances are linked into this TU).
    RegisterCore();

    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::fprintf(stderr,
                     "FATAL: SystemRegistry::InitializeAll(false) failed; "
                     "cannot run schema-evolution smoke test.\n");
        return 77;
    }

    scenario_S1_round_trip();
    scenario_S2_drop_field();
    scenario_S3_add_field();
    scenario_S4_reorder();
    // PR-SE3a additions: real-world MaterialRes schema-evolution.
    scenario_M1_material_round_trip();
    scenario_M2_material_old_layout_compat();
    // PR-SE3a-migrate: string → PPtr schema evolution.
    scenario_M3_material_pptr_evolution();
    // Texture cook Phase 1: Texture2D mip/format schema evolution.
    scenario_T1_texture_old_layout_compat();
    scenario_T2_texture_mip_round_trip();

    SystemRegistry::GetInstance().ShutdownAll();

    if (g_failures > 0)
    {
        std::fprintf(stderr,
                     "\n%d scenario(s) FAILED. Inspect [FAIL] lines above.\n",
                     g_failures);
        return 1;
    }

    std::fprintf(stderr, "\nAll 9 scenarios passed.\n");
    return 0;
}
