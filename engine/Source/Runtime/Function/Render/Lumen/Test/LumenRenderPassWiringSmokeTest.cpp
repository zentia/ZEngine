// =============================================================================
// Lumen render-pass wiring smoke test.
//
// The GI probe grid (see LumenGlobalIlluminationSmokeTest) had real content, but
// nothing in the engine ever asked for it:
//
//   * LumenRenderPass::Render() -- the only function that drives Update /
//     ComputeGlobalIllumination / ComputeReflections -- had ZERO callers. Only
//     PreparePassData() was invoked, from RenderPipelineBase.
//   * m_LumenPass was created inside DesktopRenderPipelineModule under
//     #if defined(Z_HAS_VULKAN). Apple compiles Lumen but has no Vulkan, so the
//     pass simply did not exist there. Z_HAS_LUMEN is the right switch (it
//     mirrors which configurations strip Function/Render/Lumen from ZRuntime).
//   * Render() ended in a TODO, so even if it had run, the result went nowhere.
//   * There were no CVars at all: GI could not be turned off at runtime, and
//     probe density was a hard-coded constant.
//
// This test pins the wiring: the pass publishes its irradiance into
// RenderScene::m_IndirectLighting, the r.Lumen.* CVars actually reach the
// renderer and are live, and RenderPipelineBase::UpdateLumen dispatches to a
// Lumen pass and silently ignores anything else.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_LUMEN_RENDER_PASS_WIRING_SMOKE_TEST=ON
//   cmake --build build --target LumenRenderPassWiringSmokeTest
// =============================================================================

#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Render/Lumen/LumenGlobalIllumination.h"
#include "Runtime/Function/Render/Lumen/LumenRenderPass.h"
#include "Runtime/Function/Render/Lumen/LumenSettings.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderPipelineBase.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Function/Render/RenderScene.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    // Only the CPU geometry cache matters here; every GPU upload is a no-op.
    class StubRenderResource : public RenderResourceBase
    {
    public:
        void clear() override { ClearMeshGeometryCache(); }

        void UploadGlobalRenderResource(RHI*, LevelResourceDesc) override {}

        void UploadGameObjectRenderResource(RHI*, RenderEntity, RenderMeshData, RenderMaterialData) override {}

        void UploadGameObjectRenderResource(RHI*, RenderEntity, RenderMeshData) override {}

        void UploadGameObjectRenderResource(RHI*, RenderEntity, RenderMaterialData) override {}

        void UpdatePerFrameBuffer(std::shared_ptr<RenderScene>, std::shared_ptr<RenderCamera>) override {}

        void RegisterMesh(size_t asset_id, const RenderMeshData& mesh_data, const AxisAlignedBox& local_bounds)
        {
            CacheMeshGeometry(asset_id, mesh_data, local_bounds);
        }
    };

    RenderMeshData MakeQuad(const Vector3& v0, const Vector3& v1, const Vector3& v2, const Vector3& v3)
    {
        RenderMeshData mesh_data;
        mesh_data.m_StaticMeshData.m_VertexBuffer =
            std::make_shared<BufferData>(4 * sizeof(MeshVertexDataDefinition));
        std::memset(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data, 0, 4 * sizeof(MeshVertexDataDefinition));
        MeshVertexDataDefinition* vertices =
            static_cast<MeshVertexDataDefinition*>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data);

        const Vector3 corners[4] = {v0, v1, v2, v3};
        for (size_t i = 0; i < 4; ++i)
        {
            vertices[i].x = corners[i].x;
            vertices[i].y = corners[i].y;
            vertices[i].z = corners[i].z;
        }

        mesh_data.m_StaticMeshData.m_IndexBuffer = std::make_shared<BufferData>(6 * sizeof(uint16_t));
        uint16_t* indices = static_cast<uint16_t*>(mesh_data.m_StaticMeshData.m_IndexBuffer->m_Data);
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        indices[3] = 0;
        indices[4] = 2;
        indices[5] = 3;

        return mesh_data;
    }

    AxisAlignedBox MakeBox(const Vector3& min_corner, const Vector3& max_corner)
    {
        AxisAlignedBox box;
        box.Merge(min_corner);
        box.Merge(max_corner);
        return box;
    }

    RenderEntity MakeFloorEntity(uint32_t instance_id, size_t mesh_asset_id, const Vector4& base_color)
    {
        RenderEntity entity;
        entity.m_InstanceId = instance_id;
        entity.m_MeshAssetId = mesh_asset_id;
        entity.m_ModelMatrix = Matrix4x4::IDENTITY;
        entity.m_BoundingBox = MakeBox(Vector3(-6.0f, 0.0f, -6.0f), Vector3(6.0f, 0.0f, 6.0f));
        entity.m_BaseColorFactor = base_color;
        entity.m_RoughnessFactor = 0.8f;
        entity.m_MetallicFactor = 0.0f;
        entity.m_EmissiveFactor = Vector3(0.0f, 0.0f, 0.0f);
        return entity;
    }

    // A pass that is not Lumen: UpdateLumen must ignore it instead of crashing on
    // the dynamic_cast.
    class DummyPass : public RenderPassBase
    {
    public:
        void Initialize(const RenderPassInitInfo*) override {}
    };

    // RenderPipelineBase is abstract; this is the thinnest possible host so the
    // test can reach the protected m_LumenPass slot the real modules fill.
    class TestPipeline : public RenderPipelineBase
    {
    public:
        TestPipeline() { m_Rhi = nullptr; }

        void Initialize(RenderPipelineInitInfo) override {}

        uint32_t GetGuidOfPickedMesh(const Vector2&) override { return 0; }

        void SetLumenPass(std::shared_ptr<RenderPassBase> pass) { m_LumenPass = std::move(pass); }
    };

    bool SetCvar(ConsoleManager& console, const std::string& name, const std::string& value)
    {
        auto variable = console.FindVariable(name);
        if (!variable)
        {
            std::printf("[FAIL] cvar %s is not registered\n", name.c_str());
            ++g_Checks;
            ++g_Failures;
            return false;
        }
        return variable->setStringValue(value);
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // -------------------------------------------------------------------------
    // 0. The CVar layer must not silently change the defaults.
    // -------------------------------------------------------------------------
    ConsoleManager console;
    LumenSettings::RegisterConsoleVariables(console);

    Check(console.FindVariable("r.Lumen.Enable") != nullptr, "r.Lumen.Enable is registered");
    Check(console.FindVariable("r.Lumen.ProbeSpacing") != nullptr, "r.Lumen.ProbeSpacing is registered");
    Check(console.FindVariable("r.Lumen.TemporalBlendRate") != nullptr, "r.Lumen.TemporalBlendRate is registered");

    const LumenConfig defaults {};
    const LumenConfig from_cvars = LumenSettings::BuildConfig();
    Check(LumenSettings::IsEnabled(), "Lumen is enabled by default");
    Check(LumenSettings::IsGlobalIlluminationEnabled(), "GI is enabled by default");
    Check(from_cvars.gi_probe_spacing == defaults.gi_probe_spacing &&
              from_cvars.gi_probe_update_budget == defaults.gi_probe_update_budget &&
              from_cvars.gi_temporal_blend_rate == defaults.gi_temporal_blend_rate &&
              from_cvars.gi_sample_count == defaults.gi_sample_count,
          "an untouched CVar layer reproduces LumenConfig's own defaults");

    // -------------------------------------------------------------------------
    // Scene: a red floor at y = 0 inside a known world box.
    // -------------------------------------------------------------------------
    auto render_scene = std::make_shared<RenderScene>();
    render_scene->m_RenderEntities.push_back(
        MakeFloorEntity(/*instance_id=*/1, /*mesh_asset_id=*/1, Vector4(1.0f, 0.0f, 0.0f, 1.0f)));

    StubRenderResource render_resource;
    const AxisAlignedBox quad_local_bounds = MakeBox(Vector3(-6.0f, 0.0f, -6.0f), Vector3(6.0f, 0.0f, 6.0f));
    render_resource.RegisterMesh(/*asset_id=*/1,
                                 MakeQuad(Vector3(-6.0f, 0.0f, -6.0f),
                                          Vector3(6.0f, 0.0f, -6.0f),
                                          Vector3(6.0f, 0.0f, 6.0f),
                                          Vector3(-6.0f, 0.0f, 6.0f)),
                                 quad_local_bounds);

    auto camera = std::make_shared<RenderCamera>();
    camera->m_Position = Vector3(0.0f, 2.0f, 0.0f);

    // -------------------------------------------------------------------------
    // 1. The pass drives the whole chain and publishes a result.
    //
    // Render() had zero callers, so the distance field -> surface cache -> probe
    // grid chain had never run inside a real frame.
    // -------------------------------------------------------------------------
    Check(render_scene->m_IndirectLighting.weight == 0.0f, "a fresh scene has no indirect lighting");

    LumenRenderPass lumen_pass;
    RenderPassCommonInfo common_info;
    common_info.rhi = nullptr;  // Lumen is CPU-only today; a null RHI must not disable it
    common_info.render_resource = std::shared_ptr<RenderResourceBase>(&render_resource, [](RenderResourceBase*) {});
    lumen_pass.SetCommonInfo(common_info);
    lumen_pass.Initialize(nullptr);

    Check(lumen_pass.getLumenRenderer() != nullptr, "the pass owns a renderer");

    lumen_pass.Render(render_scene, camera, 1.0f / 60.0f);

    Check(lumen_pass.hasPublishedIndirectLighting(), "Render published a result (was: the function ended in a TODO)");
    Check(render_scene->m_IndirectLighting.weight == 1.0f,
          "the published weight is 1, so deferred lighting will use it instead of the baked cubemap");
    Check(render_scene->m_IndirectLighting.irradiance.length() > 0.0f, "the published irradiance is non-zero");
    Check(render_scene->m_IndirectLighting.irradiance.x > render_scene->m_IndirectLighting.irradiance.z,
          "the published irradiance is red-dominant, matching the red floor");
    Check(render_scene->m_IndirectLighting.frame_index ==
              lumen_pass.getLumenRenderer()->getGlobalIllumination()->getFrameIndex(),
          "the published result is stamped with the GI frame index");

    const Vector3 published_red = render_scene->m_IndirectLighting.irradiance;

    // -------------------------------------------------------------------------
    // 2. r.Lumen.IndirectIntensity scales the published result.
    // -------------------------------------------------------------------------
    Check(SetCvar(console, "r.Lumen.IndirectIntensity", "2"), "r.Lumen.IndirectIntensity can be set");
    lumen_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(std::fabs(render_scene->m_IndirectLighting.irradiance.x / published_red.x - 2.0f) < 0.35f,
          "doubling r.Lumen.IndirectIntensity doubles the published irradiance");
    Check(SetCvar(console, "r.Lumen.IndirectIntensity", "1"), "r.Lumen.IndirectIntensity restored");

    // -------------------------------------------------------------------------
    // 3. r.Lumen.Enable=0 stops the publish instead of leaving stale light.
    // -------------------------------------------------------------------------
    Check(SetCvar(console, "r.Lumen.Enable", "0"), "r.Lumen.Enable can be set");
    Check(!LumenSettings::IsEnabled(), "the CVar layer reports Lumen as disabled");

    lumen_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(!lumen_pass.hasPublishedIndirectLighting(), "a disabled Lumen publishes nothing");
    Check(render_scene->m_IndirectLighting.weight == 0.0f,
          "disabling Lumen zeroes the weight instead of leaving the old irradiance behind");

    Check(SetCvar(console, "r.Lumen.Enable", "1"), "r.Lumen.Enable restored");
    lumen_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(render_scene->m_IndirectLighting.weight == 1.0f, "re-enabling Lumen publishes again");
    Check(render_scene->m_IndirectLighting.irradiance.length() > 0.0f, "the re-enabled result is non-zero");

    // -------------------------------------------------------------------------
    // 4. r.Lumen.GI=0 stops just the probe grid.
    // -------------------------------------------------------------------------
    Check(SetCvar(console, "r.Lumen.GlobalIllumination", "0"), "r.Lumen.GlobalIllumination can be set");
    lumen_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(render_scene->m_IndirectLighting.weight == 0.0f, "turning off only GI also zeroes the weight");
    Check(SetCvar(console, "r.Lumen.GlobalIllumination", "1"), "r.Lumen.GlobalIllumination restored");

    // -------------------------------------------------------------------------
    // 5. A structural CVar is live: changing the probe spacing resizes the grid.
    // -------------------------------------------------------------------------
    const uint32_t resolution_before = lumen_pass.getLumenRenderer()
                                           ->getGlobalIllumination()
                                           ->getProbeGrid()
                                           .resolution_x;
    Check(SetCvar(console, "r.Lumen.ProbeSpacing", "1"), "r.Lumen.ProbeSpacing can be set");
    lumen_pass.Render(render_scene, camera, 1.0f / 60.0f);
    const uint32_t resolution_after = lumen_pass.getLumenRenderer()
                                          ->getGlobalIllumination()
                                          ->getProbeGrid()
                                          .resolution_x;
    Check(resolution_after > resolution_before,
          "halving the probe spacing raises the grid resolution without restarting the pipeline");
    Check(render_scene->m_IndirectLighting.weight == 1.0f, "the resized grid still publishes");
    Check(SetCvar(console, "r.Lumen.ProbeSpacing", "4"), "r.Lumen.ProbeSpacing restored");

    // -------------------------------------------------------------------------
    // 6. RenderPipelineBase::UpdateLumen dispatches -- and ignores what it should.
    // -------------------------------------------------------------------------
    TestPipeline pipeline;

    auto fresh_scene = std::make_shared<RenderScene>();
    fresh_scene->m_RenderEntities = render_scene->m_RenderEntities;

    // No Lumen pass at all: a no-op, not a crash.
    pipeline.UpdateLumen(fresh_scene, camera, 1.0f / 60.0f);
    Check(fresh_scene->m_IndirectLighting.weight == 0.0f, "UpdateLumen without a Lumen pass is a no-op");

    // A pass that is not Lumen: the dynamic_cast fails and nothing happens.
    pipeline.SetLumenPass(std::make_shared<DummyPass>());
    pipeline.UpdateLumen(fresh_scene, camera, 1.0f / 60.0f);
    Check(fresh_scene->m_IndirectLighting.weight == 0.0f, "UpdateLumen ignores a non-Lumen pass");

    // Degenerate arguments must not be dereferenced.
    pipeline.UpdateLumen(nullptr, camera, 1.0f / 60.0f);
    pipeline.UpdateLumen(fresh_scene, nullptr, 1.0f / 60.0f);
    Check(true, "UpdateLumen rejects a null scene and a null camera");

    // A real Lumen pass: this is the wiring that did not exist.
    auto wired_pass = std::make_shared<LumenRenderPass>();
    wired_pass->SetCommonInfo(common_info);
    wired_pass->Initialize(nullptr);
    pipeline.SetLumenPass(wired_pass);
    pipeline.UpdateLumen(fresh_scene, camera, 1.0f / 60.0f);
    Check(fresh_scene->m_IndirectLighting.weight == 1.0f,
          "UpdateLumen drives the Lumen pass (was: Render() had zero callers)");
    Check(fresh_scene->m_IndirectLighting.irradiance.x > fresh_scene->m_IndirectLighting.irradiance.z,
          "the scene-driven result is still red-dominant");

    // -------------------------------------------------------------------------
    // 7. Degenerate input into the pass itself.
    // -------------------------------------------------------------------------
    lumen_pass.Render(nullptr, camera, 1.0f / 60.0f);
    lumen_pass.Render(render_scene, nullptr, 1.0f / 60.0f);
    Check(true, "Render rejects a null scene and a null camera");

    // Without a render resource the pass must stay inert rather than crash.
    LumenRenderPass orphan_pass;
    RenderPassCommonInfo orphan_info;
    orphan_info.rhi = nullptr;
    orphan_info.render_resource = nullptr;
    orphan_pass.SetCommonInfo(orphan_info);
    orphan_pass.Initialize(nullptr);
    auto orphan_scene = std::make_shared<RenderScene>();
    orphan_scene->m_RenderEntities = render_scene->m_RenderEntities;
    orphan_pass.Render(orphan_scene, camera, 1.0f / 60.0f);
    Check(!orphan_pass.hasPublishedIndirectLighting() && orphan_scene->m_IndirectLighting.weight == 0.0f,
          "a pass with no render resource publishes nothing (no CPU geometry to trace)");

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
