// =====================================================================
// MaterialCustomParamSmokeTest
// ---------------------------------------------------------------------
// Verifies the CPU half of the "dynamic property block" chain:
//
//   1. a *named* property resolves to the right (vec4 slot, component) pair
//   2. setting a scalar clobbers **only that component** -- its slot-mates
//      keep whatever the asset had (this is why the mask is per-component)
//   3. keyword overrides land in the desc's m_EnabledShaderKeywords, sorted,
//      and do NOT flag the part as needing its own material instance
//
// Why this test exists: the whole point of the feature is that a script can
// say SetFloat("_DissolveAmount", 0.5) without knowing that the shader reads
// customParams[0].x. A bug in the name table, or in the per-component mask,
// silently produces the wrong visual with no error anywhere -- nothing in the
// engine would notice that "_DissolveEdgeWidth" overwrote the threshold, or
// that the edge width got zeroed as a side effect. Those are the cases below
// (P2/P3 and K3).
//
// Cases:
//   N1 name table maps each documented name to the slot/component the
//      shaders actually read
//   N2 unknown name -> Find* returns nullptr, setters return false
//   P1 SetFloat lands on its own slot only
//   P2 SetFloat("_DissolveEdgeWidth") does not clobber "_DissolveAmount"
//      (same vec4 slot, different component)  <-- the per-component mask
//   P3 SetFloat on one slot does not touch another slot
//   P4 SetVector / SetColor overwrite all four components of the slot
//   P5 getters read back what will be drawn; unset falls back to the asset
//   P6 ClearCustomParams returns everything to the asset value
//   K1 EnableKeyword -> the keyword shows up in the submitted desc
//   K2 HasKeyword follows override-then-asset; DisableKeyword removes it
//   K3 a keyword-only override does NOT flag m_UseMaterialInstance
//      (keyword changes the material *variant*, not the uniform)  <-- the
//      batching trap this whole distinction exists for
//   K4 keyword application order does not matter (the list is sorted, so
//      the same set always yields the same MaterialSourceDesc hash)
//   K5 ClearKeywords returns the slot to "follow the asset"
//   B1 out-of-range slot and empty name are silently ignored
//   R1 SetSubMeshes re-sync preserves overrides
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt so
// /WHOLEARCHIVE cannot drag its main() into every consumer executable.
//
// Pure CPU: no project, no window, no GPU, no AssetManager -- same setup as
// MaterialInstanceOverrideSmokeTest (empty material asset paths keep
// populateMaterialDesc away from AssetManager, and SubmitRenderState
// early-returns while m_ParentObject is null).
// =====================================================================

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Framework/Component/Mesh/MaterialCustomParamNames.h"
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
            // No material asset on purpose: keeps populateMaterialDesc away
            // from AssetManager.
            sub_meshes.push_back(sub_mesh);
        }
        return sub_meshes;
    }

    bool HasKeywordIn(const GameObjectMaterialDesc& desc, const char* keyword)
    {
        for (const eastl::string& enabled : desc.m_EnabledShaderKeywords)
        {
            if (enabled == keyword)
            {
                return true;
            }
        }
        return false;
    }
}  // namespace

int main()
{
    ProbeRenderer renderer;
    renderer.SetSubMeshes(MakeSubMeshes(3));

    // ---- N1: the name table matches what the shaders read --------------
    {
        const MaterialCustomParamLocation* amount = MaterialCustomParams::FindFloat("_DissolveAmount");
        const MaterialCustomParamLocation* width = MaterialCustomParams::FindFloat("_DissolveEdgeWidth");
        Check(amount != nullptr && amount->slot == 0 && amount->component == 0,
              "N1 _DissolveAmount -> customParams[0].x (the shader's dissolve_params.x)");
        Check(width != nullptr && width->slot == 0 && width->component == 1,
              "N1b _DissolveEdgeWidth -> customParams[0].y (dissolve_params.y)");

        const MaterialCustomParamLocation* edge = MaterialCustomParams::FindVector("_DissolveEdgeColor");
        const MaterialCustomParamLocation* custom0 = MaterialCustomParams::FindVector("_CustomParam0");
        const MaterialCustomParamLocation* custom1 = MaterialCustomParams::FindVector("_CustomParam1");
        Check(edge != nullptr && edge->slot == 1, "N1c _DissolveEdgeColor -> customParams[1]");
        Check(custom0 != nullptr && custom0->slot == 2, "N1d _CustomParam0 -> customParams[2]");
        Check(custom1 != nullptr && custom1->slot == 3, "N1e _CustomParam1 -> customParams[3]");
    }

    // ---- N2: unknown names are rejected, not silently stored -----------
    {
        Check(MaterialCustomParams::FindFloat("_NoSuchThing") == nullptr, "N2 unknown float name -> nullptr");
        Check(MaterialCustomParams::FindVector("_NoSuchThing") == nullptr, "N2b unknown vector name -> nullptr");
        Check(!renderer.SetFloat(0, "_NoSuchThing", 1.0f), "N2c SetFloat with an unknown name returns false");
        Check(!renderer.SetVector(0, "_NoSuchThing", Vector4(1.f, 1.f, 1.f, 1.f)),
              "N2d SetVector with an unknown name returns false");
        Check(Nearly(renderer.GetFloat(0, "_NoSuchThing"), 0.0f), "N2e GetFloat of an unknown name == 0");
        // A scalar name is not a vector name: SetFloat must not be reachable
        // through SetVector (otherwise the slot semantics are ambiguous).
        Check(MaterialCustomParams::FindVector("_DissolveAmount") == nullptr,
              "N2f a scalar name is not registered as a vector name");
        Check(MaterialCustomParams::FindFloat("_DissolveEdgeColor") == nullptr,
              "N2g a vector name is not registered as a scalar name");
    }

    // ---- P1/P2/P3: per-component, per-slot isolation ------------------
    {
        renderer.SetFloat(1, "_DissolveAmount", 0.5f);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(Nearly(parts[1].m_MaterialDesc.m_CustomParams[0].x, 0.5f),
              "P1 SetFloat(_DissolveAmount) landed on slot 1's customParams[0].x");
        Check(Nearly(parts[0].m_MaterialDesc.m_CustomParams[0].x, 0.0f),
              "P3 slot 0 was not touched by slot 1's SetFloat");
        Check(Nearly(parts[2].m_MaterialDesc.m_CustomParams[0].x, 0.0f),
              "P3b slot 2 was not touched either");
        Check(parts[1].m_MaterialDesc.m_UseMaterialInstance,
              "P1b a custom-slot override does flag m_UseMaterialInstance (it changes the UBO)");
    }
    {
        // The whole reason the mask is per-component: both names live in
        // customParams[0], and setting the edge width must leave the
        // threshold alone.
        renderer.SetFloat(1, "_DissolveEdgeWidth", 0.125f);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(Nearly(parts[1].m_MaterialDesc.m_CustomParams[0].y, 0.125f),
              "P2 SetFloat(_DissolveEdgeWidth) landed on customParams[0].y");
        Check(Nearly(parts[1].m_MaterialDesc.m_CustomParams[0].x, 0.5f),
              "P2b ... and did NOT clobber customParams[0].x (the threshold from P1)");
        Check(Nearly(parts[1].m_MaterialDesc.m_CustomParams[0].z, 0.0f) &&
                  Nearly(parts[1].m_MaterialDesc.m_CustomParams[0].w, 0.0f),
              "P2c the two untouched components of the same slot stay at the asset value (0)");
    }

    // ---- P4/P5: vector / colour setters and getters --------------------
    const Vector4 k_EdgeColor {1.0f, 0.4f, 0.0f, 1.0f};
    {
        renderer.SetColor(1, "_DissolveEdgeColor", k_EdgeColor);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(Nearly(parts[1].m_MaterialDesc.m_CustomParams[1], k_EdgeColor),
              "P4 SetColor overwrote all four components of customParams[1]");
        Check(Nearly(renderer.GetColor(1, "_DissolveEdgeColor"), k_EdgeColor),
              "P5 GetColor reads back what will be drawn");
        Check(Nearly(renderer.GetVector(1, "_DissolveEdgeColor"), k_EdgeColor),
              "P5b GetVector == GetColor (they are the same storage)");
        Check(Nearly(renderer.GetFloat(1, "_DissolveAmount"), 0.5f),
              "P5c GetFloat returns the override");
        Check(Nearly(renderer.GetFloat(0, "_DissolveAmount"), 0.0f),
              "P5d GetFloat on a never-overridden slot falls back to the asset value");
        Check(Nearly(renderer.GetVector(0, "_DissolveEdgeColor"), Vector4(0.f, 0.f, 0.f, 0.f)),
              "P5e GetVector on a never-overridden slot falls back to the asset value");
    }

    // ---- P6: ClearCustomParams ----------------------------------------
    {
        renderer.ClearCustomParams(1);

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        bool all_zero = true;
        for (uint32_t slot = 0; slot < k_MaterialCustomParamCount; ++slot)
        {
            all_zero = all_zero && Nearly(parts[1].m_MaterialDesc.m_CustomParams[slot],
                                          Vector4(0.f, 0.f, 0.f, 0.f));
        }
        Check(all_zero, "P6 ClearCustomParams returned every slot to the asset value");
        Check(!parts[1].m_MaterialDesc.m_UseMaterialInstance,
              "P6b cleared slot is batchable again (m_UseMaterialInstance=false)");
        Check(Nearly(renderer.GetFloat(1, "_DissolveAmount"), 0.0f), "P6c GetFloat falls back after Clear");
    }

    // ---- K1/K2: keyword override --------------------------------------
    {
        renderer.EnableKeyword(2, "_DISSOLVE_ON");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(HasKeywordIn(parts[2].m_MaterialDesc, "_DISSOLVE_ON"),
              "K1 EnableKeyword put the keyword in the submitted desc");
        Check(!HasKeywordIn(parts[0].m_MaterialDesc, "_DISSOLVE_ON") &&
                  !HasKeywordIn(parts[1].m_MaterialDesc, "_DISSOLVE_ON"),
              "K1b the other slots were not given the keyword");

        Check(renderer.HasKeyword(2, "_DISSOLVE_ON"), "K2 HasKeyword == true after Enable");
        Check(!renderer.HasKeyword(2, "_NEVER_SET"), "K2b HasKeyword of an unmentioned keyword == false");
        Check(!renderer.HasKeyword(0, "_DISSOLVE_ON"), "K2c slot 0 does not have slot 2's keyword");

        renderer.DisableKeyword(2, "_DISSOLVE_ON");
        Check(!renderer.HasKeyword(2, "_DISSOLVE_ON"), "K2d HasKeyword == false after Disable");

        const std::vector<GameObjectPartDesc> after = renderer.Parts();
        Check(!HasKeywordIn(after[2].m_MaterialDesc, "_DISSOLVE_ON"),
              "K2e a disabled keyword is removed from the desc, not left in");
    }

    // ---- K3: keyword alone must NOT cost us a material instance -------
    {
        renderer.EnableKeyword(0, "_EMISSION_ON");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(HasKeywordIn(parts[0].m_MaterialDesc, "_EMISSION_ON"), "K3 the keyword reached the desc");
        Check(!parts[0].m_MaterialDesc.m_UseMaterialInstance,
              "K3b keyword-only override does NOT flag m_UseMaterialInstance "
              "(it changes the shared-material variant, not the uniform)");
    }

    // ---- K4: order-independence of the keyword set --------------------
    {
        ProbeRenderer a;
        a.SetSubMeshes(MakeSubMeshes(1));
        a.EnableKeyword(0, "_B_ON");
        a.EnableKeyword(0, "_A_ON");

        ProbeRenderer b;
        b.SetSubMeshes(MakeSubMeshes(1));
        b.EnableKeyword(0, "_A_ON");
        b.EnableKeyword(0, "_B_ON");

        // Parts() 返回的是**值**。以前这里写成
        // `const eastl::string* ka = a.Parts()[0]...data();`，那个临时 vector 在
        // 分号处就析构了，ka/kb 全是悬空指针 —— K4b 于是读已释放内存，
        // 读出来的顺序是随机的（实测打出乱码），跟被测的排序逻辑毫无关系。
        // 必须先落到具名局部量，再从它取引用。
        const std::vector<GameObjectPartDesc> parts_a = a.Parts();
        const std::vector<GameObjectPartDesc> parts_b = b.Parts();
        const eastl::vector<eastl::string>& keywords_a = parts_a[0].m_MaterialDesc.m_EnabledShaderKeywords;
        const eastl::vector<eastl::string>& keywords_b = parts_b[0].m_MaterialDesc.m_EnabledShaderKeywords;
        const size_t na = keywords_a.size();
        const size_t nb = keywords_b.size();
        Check(na == 2 && nb == 2, "K4 both renderers ended up with 2 keywords");
        Check(na == 2 && nb == 2 && keywords_a[0] == keywords_b[0] && keywords_a[1] == keywords_b[1],
              "K4b enabling A-then-B and B-then-A produce the same (sorted) keyword list -- "
              "otherwise MaterialSourceDesc::getHashValue would mint two identical shared materials");
        Check(na == 2 && keywords_a[0] == "_A_ON" && keywords_a[1] == "_B_ON",
              "K4c the list is actually sorted, not merely equal by accident");
    }

    // ---- K5: ClearKeywords -------------------------------------------
    {
        renderer.ClearKeywords(2);
        Check(!renderer.HasKeyword(2, "_DISSOLVE_ON"), "K5 ClearKeywords -> HasKeyword == false");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(!HasKeywordIn(parts[2].m_MaterialDesc, "_DISSOLVE_ON"),
              "K5b cleared slot's desc no longer carries the override");
        Check(HasKeywordIn(parts[0].m_MaterialDesc, "_EMISSION_ON"),
              "K5c ClearKeywords on slot 2 did not touch slot 0's keyword");
    }

    // ---- B1: bad input is ignored -------------------------------------
    {
        renderer.SetFloat(-1, "_DissolveAmount", 1.0f);
        renderer.SetFloat(99, "_DissolveAmount", 1.0f);
        renderer.EnableKeyword(-1, "_DISSOLVE_ON");
        renderer.EnableKeyword(99, "_DISSOLVE_ON");
        renderer.EnableKeyword(0, "");

        const std::vector<GameObjectPartDesc> parts = renderer.Parts();
        Check(Nearly(parts[0].m_MaterialDesc.m_CustomParams[0].x, 0.0f),
              "B1 out-of-range SetFloat did not write slot 0");
        Check(!HasKeywordIn(parts[0].m_MaterialDesc, "_DISSOLVE_ON"),
              "B1b out-of-range EnableKeyword did not write slot 0");
        Check(!renderer.HasKeyword(0, ""), "B1c HasKeyword of an empty name == false");
    }

    // ---- R1: resize preserves overrides -------------------------------
    {
        renderer.SetFloat(1, "_DissolveAmount", 0.75f);
        renderer.SetSubMeshes(MakeSubMeshes(5));
        Check(renderer.GetMaterialSlotCount() == 5, "R1 growing to 5 sub-meshes -> 5 slots");
        Check(Nearly(renderer.GetFloat(1, "_DissolveAmount"), 0.75f),
              "R1b slot 1's custom param survived the resize");

        renderer.SetSubMeshes(MakeSubMeshes(1));
        Check(renderer.GetMaterialSlotCount() == 1, "R1c shrinking to 1 sub-mesh -> 1 slot");
        Check(Nearly(renderer.GetFloat(1, "_DissolveAmount"), 0.0f),
              "R1d slot 1 no longer exists after the shrink (reads as the fallback, not a crash)");
    }

    std::fprintf(stdout, "\nMaterialCustomParam: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
