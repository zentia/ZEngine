// =============================================================================
// Nanite scene-wiring smoke test.
//
// Everything Nanite does now works (cluster builder produces real geometry and a
// real hierarchy; frustum culling culls; SelectLOD points the right way), but
// nothing in the engine ever asked for any of it:
//
//   * NaniteRenderer had no way to see a scene. Instances could only arrive via
//     RegisterInstance(), and that function had ZERO callers in the whole repo.
//   * NaniteRenderPass was never constructed anywhere, and its Draw() was an
//     empty TODO behind a null check on a renderer nobody ever set.
//   * There was no pipeline hook, no frame command, no CVar: even a hand-built
//     scene could not reach the renderer from a real frame.
//
// This test pins the wiring end to end:
//
//   RenderScene -> RenderResourceBase CPU mesh cache -> Nanite clusters
//               -> NaniteInstance list -> culling -> render batches
//
// and the two dispatchers above it (NaniteRenderPass::Render and
// RenderPipelineBase::UpdateNanite), plus the r.Nanite.* CVars.
//
// Headless: no RHI, no window, no project.
//
//   cmake -B build -DZENGINE_BUILD_NANITE_SCENE_WIRING_SMOKE_TEST=ON
//   cmake --build build --target NaniteSceneWiringSmokeTest
// =============================================================================

#include "Runtime/Function/Console/ConsoleManager.h"
#include "Runtime/Function/Render/Nanite/NaniteRenderer.h"
#include "Runtime/Function/Render/Nanite/NaniteSettings.h"
#include "Runtime/Function/Render/Nanite/NaniteTypes.h"
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

    // 一张 cells × cells 的平面网格（y = 0），按 loader 的格式写：MeshVertexDataDefinition
    // 顶点缓冲 + uint16_t 索引缓冲。资产 id 0 会被缓存拒绝，所以测试里从 1 开始。
    RenderMeshData MakeGridMesh(uint32_t cells_per_side, float extent)
    {
        const uint32_t vertex_side = cells_per_side + 1;
        const float step = extent / static_cast<float>(cells_per_side);
        const float origin = -extent * 0.5f;

        std::vector<Vector3> corners;
        for (uint32_t z = 0; z < vertex_side; ++z)
        {
            for (uint32_t x = 0; x < vertex_side; ++x)
            {
                corners.push_back(
                    Vector3(origin + static_cast<float>(x) * step, 0.0f, origin + static_cast<float>(z) * step));
            }
        }

        std::vector<uint16_t> indices;
        for (uint32_t z = 0; z < cells_per_side; ++z)
        {
            for (uint32_t x = 0; x < cells_per_side; ++x)
            {
                const uint16_t i0 = static_cast<uint16_t>(z * vertex_side + x);
                const uint16_t i1 = static_cast<uint16_t>(i0 + 1);
                const uint16_t i2 = static_cast<uint16_t>(i0 + vertex_side);
                const uint16_t i3 = static_cast<uint16_t>(i2 + 1);

                indices.push_back(i0);
                indices.push_back(i2);
                indices.push_back(i1);
                indices.push_back(i1);
                indices.push_back(i2);
                indices.push_back(i3);
            }
        }

        RenderMeshData mesh_data;
        mesh_data.m_StaticMeshData.m_VertexBuffer =
            std::make_shared<BufferData>(corners.size() * sizeof(MeshVertexDataDefinition));
        std::memset(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data,
                    0,
                    corners.size() * sizeof(MeshVertexDataDefinition));
        MeshVertexDataDefinition* vertices =
            static_cast<MeshVertexDataDefinition*>(mesh_data.m_StaticMeshData.m_VertexBuffer->m_Data);
        for (size_t i = 0; i < corners.size(); ++i)
        {
            vertices[i].x = corners[i].x;
            vertices[i].y = corners[i].y;
            vertices[i].z = corners[i].z;
        }

        mesh_data.m_StaticMeshData.m_IndexBuffer = std::make_shared<BufferData>(indices.size() * sizeof(uint16_t));
        std::memcpy(mesh_data.m_StaticMeshData.m_IndexBuffer->m_Data, indices.data(), indices.size() * sizeof(uint16_t));

        return mesh_data;
    }

    AxisAlignedBox MakeBox(const Vector3& min_corner, const Vector3& max_corner)
    {
        AxisAlignedBox box;
        box.Merge(min_corner);
        box.Merge(max_corner);
        return box;
    }

    RenderEntity MakeEntity(uint32_t instance_id, size_t mesh_asset_id, const Vector3& translation)
    {
        RenderEntity entity;
        entity.m_InstanceId = instance_id;
        entity.m_MeshAssetId = mesh_asset_id;
        entity.m_ModelMatrix = Matrix4x4::GetTrans(translation);
        entity.m_BoundingBox = MakeBox(Vector3(-3.0f, 0.0f, -3.0f), Vector3(3.0f, 0.0f, 3.0f));
        return entity;
    }

    // RenderCamera 的默认状态是退化的（fovy = 0、znear > zfar），所以测试自己搭相机。
    // 不要把相机对着 ±Y：LookAt 的 up 会和视线共线，叉乘退化。
    std::shared_ptr<RenderCamera> MakeCamera(const Vector3& eye, const Vector3& target)
    {
        auto camera = std::make_shared<RenderCamera>();
        camera->m_Position = eye;
        camera->LookAt(eye, target, Vector3(0.0f, 1.0f, 0.0f));
        camera->SetAspect(16.0f / 9.0f);
        camera->setFOVx(60.0f);
        camera->m_Znear = 0.1f;
        camera->m_Zfar = 1000.0f;
        return camera;
    }

    Vector3 TranslationOf(const Matrix4x4& matrix)
    {
        const Vector4 origin = matrix * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        return Vector3(origin.x, origin.y, origin.z);
    }

    // A pass that is not Nanite: UpdateNanite must ignore it instead of crashing on
    // the dynamic_cast.
    class DummyPass : public RenderPassBase
    {
    public:
        void Initialize(const RenderPassInitInfo*) override {}
    };

    // RenderPipelineBase is abstract; this is the thinnest possible host so the
    // test can reach the protected m_NanitePass slot the real modules fill.
    class TestPipeline : public RenderPipelineBase
    {
    public:
        TestPipeline() { m_Rhi = nullptr; }

        void Initialize(RenderPipelineInitInfo) override {}

        uint32_t GetGuidOfPickedMesh(const Vector2&) override { return 0; }

        void SetNanitePass(std::shared_ptr<RenderPassBase> pass) { m_NanitePass = std::move(pass); }
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
    NaniteSettings::RegisterConsoleVariables(console);

    Check(console.FindVariable("r.Nanite.Enable") != nullptr, "r.Nanite.Enable is registered");
    Check(console.FindVariable("r.Nanite.MaxScreenError") != nullptr, "r.Nanite.MaxScreenError is registered");

    const NaniteConfig defaults {};
    const NaniteConfig from_cvars = NaniteSettings::BuildConfig();
    Check(NaniteSettings::IsEnabled(), "Nanite is enabled by default");
    Check(from_cvars.enabled == defaults.enabled && from_cvars.enable_lod == defaults.enable_lod &&
              from_cvars.enable_culling == defaults.enable_culling &&
              from_cvars.max_screen_error == defaults.max_screen_error && from_cvars.lod_bias == defaults.lod_bias,
          "an untouched CVar layer reproduces NaniteConfig's own defaults");

    // -------------------------------------------------------------------------
    // 1. Scene + CPU geometry cache.
    // -------------------------------------------------------------------------
    StubRenderResource render_resource;
    render_resource.RegisterMesh(/*asset_id=*/1, MakeGridMesh(4, 6.0f), MakeBox(Vector3(-3, 0, -3), Vector3(3, 0, 3)));
    render_resource.RegisterMesh(/*asset_id=*/2, MakeGridMesh(2, 2.0f), MakeBox(Vector3(-1, 0, -1), Vector3(1, 0, 1)));

    // 非拥有型 shared_ptr：render_resource 是栈上的，通道只借用。
    const std::shared_ptr<RenderResourceBase> resource_ptr(&render_resource, [](RenderResourceBase*) {});

    auto render_scene = std::make_shared<RenderScene>();
    render_scene->m_RenderEntities.push_back(MakeEntity(/*instance_id=*/1, /*mesh_asset_id=*/1, Vector3(0, 0, 0)));
    // 两台实例都放在相机前方（相机在 z = +12 朝原点看），否则"转开相机就全裁掉"这条
    // 会因为本来就看不见而蒙对。
    render_scene->m_RenderEntities.push_back(MakeEntity(/*instance_id=*/2, /*mesh_asset_id=*/1, Vector3(0, 0, -6)));
    // 引用了从没上传过的资产：必须跳过，不能崩。
    render_scene->m_RenderEntities.push_back(MakeEntity(/*instance_id=*/3, /*mesh_asset_id=*/99, Vector3(0, 0, 0)));

    // -------------------------------------------------------------------------
    // 2. UpdateFromScene turns the scene into instances + clusters.
    //
    // This is the gap: RegisterInstance had zero callers, so Nanite had never
    // seen a real scene.
    // -------------------------------------------------------------------------
    NaniteRenderer renderer;
    Check(renderer.Initialize(nullptr, resource_ptr, NaniteSettings::BuildConfig()),
          "initialize succeeds without an RHI (Nanite is CPU-only today)");
    Check(renderer.GetInstances().empty(), "a fresh renderer has no instances");

    Check(renderer.UpdateFromScene(render_scene, &render_resource), "UpdateFromScene accepts the scene");
    Check(renderer.GetInstances().size() == 2,
          "two of three entities became instances (the one without geometry is skipped)");
    Check(renderer.GetClusterCount(1) > 0, "the mesh asset got a real cluster set (was: nothing was ever built)");
    Check(renderer.GetClusterCount(99) == 0, "an asset that was never uploaded builds nothing");

    const NaniteInstance& first = renderer.GetInstances()[0];
    Check(first.instance_id == 1 && first.mesh_id == 1, "the instance carries the scene's ids");
    Check(first.local_bounds.IsValid(), "the instance got a usable local bounds box (was: a default empty box)");
    Check(renderer.GetInstances().size() == 2 && TranslationOf(renderer.GetInstances()[1].transform).z == -6.0f,
          "each instance took its own entity's transform");

    Check(!renderer.UpdateFromScene(nullptr, &render_resource), "a null scene is rejected");
    Check(!renderer.UpdateFromScene(render_scene, nullptr), "a null render resource is rejected");

    // -------------------------------------------------------------------------
    // 3. An unchanged scene topology only advances transforms; it must not rebuild.
    // -------------------------------------------------------------------------
    render_scene->m_RenderEntities[0].m_ModelMatrix = Matrix4x4::GetTrans(Vector3(0.0f, 4.0f, 0.0f));
    Check(renderer.UpdateFromScene(render_scene, &render_resource), "a transform-only change still succeeds");
    Check(renderer.GetInstances().size() == 2, "a transform change does not churn the instance list");
    Check(TranslationOf(renderer.GetInstances()[0].transform).y == 4.0f, "the instance transform followed the scene");

    // 再动一次：prev_transform 必须真的跟着走，否则"沿用上一帧"这条只能靠初值蒙对。
    render_scene->m_RenderEntities[0].m_ModelMatrix = Matrix4x4::GetTrans(Vector3(0.0f, 9.0f, 0.0f));
    Check(renderer.UpdateFromScene(render_scene, &render_resource), "a second transform change succeeds");
    Check(TranslationOf(renderer.GetInstances()[0].transform).y == 9.0f, "the transform followed again");
    Check(TranslationOf(renderer.GetInstances()[0].prev_transform).y == 4.0f,
          "prev_transform keeps last frame's matrix (motion blur needs it)");

    // -------------------------------------------------------------------------
    // 4. A topology change rebuilds.
    // -------------------------------------------------------------------------
    render_scene->m_RenderEntities.push_back(MakeEntity(/*instance_id=*/4, /*mesh_asset_id=*/2, Vector3(5, 0, 0)));
    Check(renderer.UpdateFromScene(render_scene, &render_resource), "adding an entity succeeds");
    Check(renderer.GetInstances().size() == 3, "the new entity became an instance");
    Check(renderer.GetClusterCount(2) > 0, "the new mesh asset got its own cluster set");

    render_scene->m_RenderEntities.pop_back();
    Check(renderer.UpdateFromScene(render_scene, &render_resource), "removing an entity succeeds");
    Check(renderer.GetInstances().size() == 2, "the removed entity's instance is gone");

    // clear() 之后必须重新认场景：否则哈希还匹配、实例列表却是空的，Nanite 永久静默。
    renderer.clear();
    Check(renderer.GetInstances().empty(), "clear drops the instances");
    Check(renderer.UpdateFromScene(render_scene, &render_resource), "clear resets the scene tracking, so it rebuilds");
    Check(renderer.GetInstances().size() == 2, "the instances came back after clear");

    // -------------------------------------------------------------------------
    // 5. The pass drives the whole chain and publishes batches.
    // -------------------------------------------------------------------------
    // 变换实验把 0 号实例挪到了相机视野外，先放回去再测通道。
    render_scene->m_RenderEntities[0].m_ModelMatrix = Matrix4x4::GetTrans(Vector3(0.0f, 0.0f, 0.0f));

    auto camera = MakeCamera(Vector3(0.0f, 3.0f, 12.0f), Vector3(0.0f, 0.0f, 0.0f));

    NaniteRenderPass nanite_pass;
    RenderPassCommonInfo common_info;
    common_info.rhi = nullptr;  // Nanite is CPU-only today; a null RHI must not disable it
    common_info.render_resource = resource_ptr;
    nanite_pass.SetCommonInfo(common_info);
    nanite_pass.Initialize(nullptr);

    Check(nanite_pass.getNaniteRenderer() != nullptr, "the pass owns a renderer");

    nanite_pass.Render(render_scene, camera, 1.0f / 60.0f);

    Check(nanite_pass.hasVisibleClusters(), "Render produced visible clusters (was: Draw() was an empty TODO)");
    Check(!nanite_pass.GetRenderBatches().empty(), "Render produced render batches");

    const std::vector<NaniteRenderBatch>& batches = nanite_pass.GetRenderBatches();
    bool all_batches_non_empty = true;
    for (const NaniteRenderBatch& batch : batches)
    {
        if (batch.cluster_count == 0)
        {
            all_batches_non_empty = false;
        }
    }
    Check(all_batches_non_empty, "no batch has cluster_count == 0 (that used to be the only thing produced)");

    // 相机转开 90°：网格完全在视锥外，逐簇裁剪必须一个不剩。
    auto camera_away = MakeCamera(Vector3(0.0f, 3.0f, 12.0f), Vector3(1000.0f, 3.0f, 12.0f));
    nanite_pass.Render(render_scene, camera_away, 1.0f / 60.0f);
    Check(!nanite_pass.hasVisibleClusters(), "turning the camera away culls every cluster");
    Check(nanite_pass.GetRenderBatches().empty(), "and therefore produces no batches");

    nanite_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(nanite_pass.hasVisibleClusters(), "pointing the camera back brings the clusters back");

    // -------------------------------------------------------------------------
    // 6. r.Nanite.Enable=0 stops the work and clears the stale batches.
    // -------------------------------------------------------------------------
    Check(SetCvar(console, "r.Nanite.Enable", "0"), "r.Nanite.Enable can be set");
    Check(!NaniteSettings::IsEnabled(), "the CVar layer reports Nanite as disabled");

    nanite_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(!nanite_pass.hasVisibleClusters(), "a disabled Nanite produces nothing");
    Check(nanite_pass.GetRenderBatches().empty(),
          "disabling Nanite clears the batches instead of leaving last frame's behind");

    Check(SetCvar(console, "r.Nanite.Enable", "1"), "r.Nanite.Enable restored");
    nanite_pass.Render(render_scene, camera, 1.0f / 60.0f);
    Check(nanite_pass.hasVisibleClusters(), "re-enabling Nanite produces clusters again");

    // -------------------------------------------------------------------------
    // 7. RenderPipelineBase::UpdateNanite dispatches -- and ignores what it should.
    // -------------------------------------------------------------------------
    TestPipeline pipeline;

    // No Nanite pass at all: a no-op, not a crash.
    pipeline.UpdateNanite(render_scene, camera, 1.0f / 60.0f);
    Check(true, "UpdateNanite without a Nanite pass is a no-op");

    // A pass that is not Nanite: the dynamic_cast fails and nothing happens.
    auto dummy = std::make_shared<DummyPass>();
    pipeline.SetNanitePass(dummy);
    pipeline.UpdateNanite(render_scene, camera, 1.0f / 60.0f);
    Check(true, "UpdateNanite ignores a non-Nanite pass");
    pipeline.SetNanitePass(nullptr);

    // Degenerate arguments must not be dereferenced.
    pipeline.UpdateNanite(nullptr, camera, 1.0f / 60.0f);
    pipeline.UpdateNanite(render_scene, nullptr, 1.0f / 60.0f);
    Check(true, "UpdateNanite rejects a null scene and a null camera");

    // A real Nanite pass: the pipeline hook is the production entry point
    // (RenderNaniteCmd -> RenderPipelineBase::UpdateNanite -> NaniteRenderPass).
    auto shared_pass = std::make_shared<NaniteRenderPass>();
    shared_pass->SetCommonInfo(common_info);
    shared_pass->Initialize(nullptr);
    pipeline.SetNanitePass(shared_pass);
    pipeline.UpdateNanite(render_scene, camera, 1.0f / 60.0f);

    Check(shared_pass->hasVisibleClusters(), "UpdateNanite dispatched to the Nanite pass");
    Check(!shared_pass->GetRenderBatches().empty(), "the dispatched pass produced batches");
    Check(shared_pass->getNaniteRenderer()->GetInstances().size() == 2,
          "the dispatched pass pulled the scene's instances in");

    // -------------------------------------------------------------------------
    // 8. A scene with no geometry at all is not an error.
    // -------------------------------------------------------------------------
    auto empty_scene = std::make_shared<RenderScene>();
    NaniteRenderer empty_renderer;
    empty_renderer.Initialize(nullptr, resource_ptr, NaniteSettings::BuildConfig());
    Check(!empty_renderer.UpdateFromScene(empty_scene, &render_resource), "an empty scene yields no instances");
    Check(empty_renderer.GetInstances().empty(), "and leaves the renderer untouched");

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
