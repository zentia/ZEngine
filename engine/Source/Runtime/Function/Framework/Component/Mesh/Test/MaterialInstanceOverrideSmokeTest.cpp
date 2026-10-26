// =====================================================================
// MaterialInstanceOverrideSmokeTest
// ---------------------------------------------------------------------
// Verifies the CPU half of the runtime material instance feature: that a
// per-sub-mesh override is applied to **only** its own slot, and that the
// slot is flagged `m_UseMaterialInstance` so RenderScene hands it its own
// GpuPBRMaterial instead of the asset-shared one.
//
// Why this test exists: before this feature the engine had exactly one
// GpuPBRMaterial per material *asset*, and the 8 scalars were baked into its
// MeshMaterialUniform only on the map's first insertion. Every part sharing
// the asset therefore shared one uniform -- tinting one part tinted all of
// them. The bug is invisible to any single-slot test; it only shows up when
// you assert the *other* slots are untouched, which is what O1/O2 below do.
//
// Cases:
//   S1 slot count == sub-mesh count
//   S2 no override -> every part keeps m_UseMaterialInstance=false (still
//      shares the asset material, so draws can batch by material)
//   O1 SetBaseColor on one slot -> that slot's desc carries the colour and
//      is flagged as an instance; the other slots are byte-identical to
//      what they were before (the actual bug this fixes)
//   O2 same for a scalar that is not the one being set: SetRoughness must
//      not move baseColor / metallic / emissive on that or any other slot
//   O3 override is applied AFTER populate, so un-overridden fields keep the
//      asset value rather than being reset to zero
//   G1 getter == what will actually be drawn (override first, else the
//      desc BuildRenderParts just produced)
//   B1 out-of-range slot is silently ignored (no crash, no override)
//   C1 ClearMaterialOverride -> slot goes back to shared (batchable again)
//   C2 HasMaterialOverride tracks IsActive, not "the slot exists"
//   R1 SetSubMeshes re-syncs the override array: growing preserves what was
//      there, and a slot that no longer exists is not readable
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt so
// /WHOLEARCHIVE cannot drag its main() into every consumer executable.
//
// Pure CPU: no project, no window, no GPU, no AssetManager. The sub-meshes
// below carry no material asset on purpose -- populateMaterialDesc returns
// before touching AssetManager when the asset path is empty -- and
// SubmitRenderState early-returns while m_ParentObject is null.
// =====================================================================

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Render/RenderObject.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            std::fprintf(stderr, "  [FAIL] %s\n", name);
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s\n", name);
        }
    }

    bool Nearly(float a, float b) { return std::fabs(a - b) < 1e-6f; }

    bool Nearly(const Vector4& a, const Vector4& b)
    {
        return Nearly(a.x, b.x) && Nearly(a.y, b.y) && Nearly(a.z, b.z) && Nearly(a.w, b.w);
    }

    /// BaseRenderer::BuildRenderParts is protected; a probe subclass is the
    /// least invasive way to read the descs that would be submitted.
    struct ProbeRenderer : BaseRenderer
    {
        std::vector<GameObjectPartDesc> Parts() const { return BuildRenderParts(nullptr); }
    };

    std::vector<SubMeshRes> MakeSubMeshes(int count)
    {
        std::vector<SubMeshRes> sub_meshes;
        sub_meshes.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            SubMeshRes sub_mesh;
            sub_mesh.m_MeshAsset = "test/mesh.zasset";
            // No m_LegacyMaterialAsset and no m_SharedMaterialAssets: every
            // slot resolves to an empty material asset, which keeps
            // populateMaterialDesc away from AssetManager.
            sub_meshes.push_back(sub_mesh);
        }
        return sub_meshes;
    }

    /// Two descs are "the same drawing state" for our purposes: same scalars,
    /// same flags, same instance-ness.
    bool SameMaterialState(const GameObjectMaterialDesc& a, const GameObjectMaterialDesc& b)
    {
        return a.m_UseMaterialInstance == b.m_UseMaterialInstance &&
               Nearly(a.m_BaseColorFactor, b.m_BaseColorFactor) &&
               Nearly(a.m_MetallicFactor, b.m_MetallicFactor) &&
               Nearly(a.m_RoughnessFactor, b.m_RoughnessFactor) &&
               Nearly(a.m_NormalScale, b.m_NormalScale) &&
               Nearly(a.m_OcclusionStrength, b.m_OcclusionStrength) &&
               a.m_IsBlend == b.m_IsBlend && a.m_IsDoubleSided == b.m_IsDoubleSided;
    }
}  // namespace

int main()
{
    ProbeRenderer renderer;
    renderer.SetSubMeshes(MakeSubMeshes(3));

    // ---- S1 / S2: baseline ---------------------------------------------
    {
        Check(renderer.GetMaterialSlotCount() == 3, "S1 slot count == sub-mesh count (3)");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(parts.size() == 3, "S1b BuildRenderParts emits one part per sub-mesh");

        bool all_shared = true;
        for (const GameObjectPartDesc& part : parts)
        {
            all_shared = all_shared && !part.m_MaterialDesc.m_UseMaterialInstance;
        }
        Check(all_shared, "S2 no override -> m_UseMaterialInstance=false everywhere (still batchable)");
    }

    const std::vector<GameObjectPartDesc> baseline = renderer.Parts();

    // ---- O1: tinting one slot must not touch the others ----------------
    const Vector4 k_HitFlash {1.0f, 0.2f, 0.2f, 1.0f};
    {
        renderer.SetBaseColor(1, k_HitFlash);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();

        Check(parts[1].m_MaterialDesc.m_UseMaterialInstance,
              "O1 overridden slot is flagged m_UseMaterialInstance (gets its own GpuPBRMaterial)");
        Check(Nearly(parts[1].m_MaterialDesc.m_BaseColorFactor, k_HitFlash),
              "O1 overridden slot carries the tint");
        Check(!parts[0].m_MaterialDesc.m_UseMaterialInstance &&
                  !parts[2].m_MaterialDesc.m_UseMaterialInstance,
              "O1b the other two slots stay on the shared material (the actual bug fixed)");
        Check(SameMaterialState(parts[0].m_MaterialDesc, baseline[0].m_MaterialDesc) &&
                  SameMaterialState(parts[2].m_MaterialDesc, baseline[2].m_MaterialDesc),
              "O1c the other two slots' drawing state is byte-identical to baseline");
    }

    // ---- O2: a scalar override only moves its own scalar ---------------
    {
        renderer.SetBaseColor(1, k_HitFlash);  // still set from O1
        renderer.SetRoughness(2, 0.25f);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();

        Check(Nearly(parts[2].m_MaterialDesc.m_RoughnessFactor, 0.25f),
              "O2 SetRoughness lands on slot 2");
        Check(Nearly(parts[2].m_MaterialDesc.m_BaseColorFactor, baseline[2].m_MaterialDesc.m_BaseColorFactor),
              "O2b SetRoughness did NOT touch slot 2's base colour");
        Check(Nearly(parts[2].m_MaterialDesc.m_MetallicFactor, baseline[2].m_MaterialDesc.m_MetallicFactor),
              "O2c SetRoughness did NOT touch slot 2's metallic");
        Check(Nearly(parts[1].m_MaterialDesc.m_RoughnessFactor, baseline[1].m_MaterialDesc.m_RoughnessFactor),
              "O2d slot 1's roughness is untouched by slot 2's override");
        Check(Nearly(parts[1].m_MaterialDesc.m_BaseColorFactor, k_HitFlash),
              "O2e slot 1 still carries its tint from O1");
    }

    // ---- O3: un-overridden fields keep the asset value -----------------
    {
        // Slot 0 has never been overridden. Whatever populateMaterialDesc
        // produced (including the m_IsDoubleSided fallback for the
        // built-in-texture path) must survive untouched -- the override is
        // applied after populate, not instead of it.
        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(SameMaterialState(parts[0].m_MaterialDesc, baseline[0].m_MaterialDesc),
              "O3 never-overridden slot keeps the populated asset state verbatim");
        Check(parts[0].m_MaterialDesc.m_IsDoubleSided == baseline[0].m_MaterialDesc.m_IsDoubleSided,
              "O3b asset-side flag (m_IsDoubleSided fallback) not reset by a neighbouring override");
    }

    // ---- G1: getter == what will actually be drawn ---------------------
    {
        const std::vector<GameObjectPartDesc> parts = renderer.Parts();

        Check(Nearly(renderer.GetBaseColor(1), parts[1].m_MaterialDesc.m_BaseColorFactor),
              "G1 GetBaseColor(1) == the desc that will be submitted for slot 1");
        Check(Nearly(renderer.GetBaseColor(0), parts[0].m_MaterialDesc.m_BaseColorFactor),
              "G1b GetBaseColor(0) == the desc that will be submitted for slot 0");
        Check(Nearly(renderer.GetRoughness(2), parts[2].m_MaterialDesc.m_RoughnessFactor),
              "G1c GetRoughness(2) == submitted roughness for slot 2");
        Check(Nearly(renderer.GetRoughness(0), parts[0].m_MaterialDesc.m_RoughnessFactor),
              "G1d GetRoughness(0) == submitted roughness for slot 0 (asset value, no override)");
        Check(Nearly(renderer.GetBaseColor(1).x, 1.0f) && Nearly(renderer.GetBaseColor(1).y, 0.2f),
              "G1e GetBaseColor(1) returns the override, not the asset");
        Check(renderer.GetDoubleSided(0) == parts[0].m_MaterialDesc.m_IsDoubleSided,
              "G1f GetDoubleSided(0) agrees with the submitted flag");
    }

    // ---- B1: out-of-range slot is silently ignored ---------------------
    {
        renderer.SetBaseColor(-1, Vector4(9.f, 9.f, 9.f, 9.f));
        renderer.SetBaseColor(99, Vector4(9.f, 9.f, 9.f, 9.f));
        renderer.SetRoughness(99, 0.9f);
        renderer.ClearMaterialOverride(-1);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        bool all_shared_still = !parts[0].m_MaterialDesc.m_UseMaterialInstance;
        Check(all_shared_still, "B1 out-of-range SetBaseColor did not flag slot 0");
        Check(Nearly(parts[0].m_MaterialDesc.m_BaseColorFactor, baseline[0].m_MaterialDesc.m_BaseColorFactor),
              "B1b out-of-range setter left slot 0's colour alone");
        Check(!renderer.HasMaterialOverride(99), "B1c HasMaterialOverride(99) == false");
        Check(!renderer.HasMaterialOverride(-1), "B1d HasMaterialOverride(-1) == false");
        Check(renderer.HasMaterialOverride(1), "B1e HasMaterialOverride(1) still true");
    }

    // ---- C1 / C2: clearing puts the slot back on the shared material ---
    {
        renderer.ClearMaterialOverride(1);
        Check(!renderer.HasMaterialOverride(1), "C1 HasMaterialOverride(1) == false after Clear");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(!parts[1].m_MaterialDesc.m_UseMaterialInstance,
              "C1b cleared slot goes back to m_UseMaterialInstance=false (batchable again)");
        Check(SameMaterialState(parts[1].m_MaterialDesc, baseline[1].m_MaterialDesc),
              "C1c cleared slot's drawing state returns to the asset value");
        Check(Nearly(renderer.GetBaseColor(1), baseline[1].m_MaterialDesc.m_BaseColorFactor),
              "C1d GetBaseColor(1) falls back to the asset value after Clear");

        Check(renderer.HasMaterialOverride(2), "C2 slot 2's roughness override survived slot 1's Clear");
        Check(parts[2].m_MaterialDesc.m_UseMaterialInstance,
              "C2b slot 2 is still flagged as an instance (overrides are per-slot)");
    }

    // ---- R1: SetSubMeshes re-syncs the override array ------------------
    {
        renderer.SetSubMeshes(MakeSubMeshes(5));
        Check(renderer.GetMaterialSlotCount() == 5, "R1 growing to 5 sub-meshes -> 5 slots");
        Check(renderer.HasMaterialOverride(2), "R1b slot 2's override survives the resize");

        renderer.SetSubMeshes(MakeSubMeshes(1));
        Check(renderer.GetMaterialSlotCount() == 1, "R1c shrinking to 1 sub-mesh -> 1 slot");
        Check(!renderer.HasMaterialOverride(2), "R1d slot 2 no longer exists after shrink");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(parts.size() == 1, "R1e BuildRenderParts follows the new sub-mesh count");
    }

    std::fprintf(stdout, "\nMaterialInstanceOverride: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
