#pragma once

#include "Runtime/Function/Framework/Object/ObjectIdAllocator.h"
#include "Runtime/Function/Render/Light.h"
#include "Runtime/Function/Render/RenderCommon.h"
#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderGuidAllocator.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Function/Render/RenderType.h"

#include <array>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Level;
class RHI;
class RenderResource;
class RenderResourceBase;
class RenderCamera;

class RenderScene
{
public:
    // light
    AmbientLight m_AmbientLight;
    PDirectionalLight m_DirectionalLight;
    PointLightList m_PointLightList;

    // render entities
    std::vector<RenderEntity> m_RenderEntities;

    // Lumen 的间接光结果（由 LumenRenderPass 每帧写入，渲染器读它填 per-frame
    // uniform）。放在场景上而不是渲染器上，是因为 UpdatePerFrameBuffer 手里只有
    // 场景和相机，够不到渲染管线。
    SceneIndirectLighting m_IndirectLighting;

    // axis, for editor
    std::optional<RenderEntity> m_RenderAxis;

    // visible objects (updated per frame)
    //
    // These lists are written on the **render thread** (RenderUpdateSceneCmd) and
    // read on the **RHI thread**: RenderDispatchRHICommandsCmd hands the draw list
    // over and returns immediately, so the RHI thread can still be executing frame
    // N-k while the render thread is already refilling the lists for frame N.
    // A single set therefore lets a pass read a node mid-construction --
    // RenderMeshNode default-constructs ref_material to nullptr, which is exactly
    // how DirectionalLightShadowPass ended up dereferencing a null material -- or
    // a vector that is being reallocated under it.
    //
    // So keep a ring and publish only once the set is complete. Ring size must
    // exceed the RHI thread's worst-case lag, which RenderFramePipeline caps at
    // kMaxFramesInFlight (2); 3 leaves one slot of slack.
    static constexpr size_t kVisibleNodeSetCount = 3;

    struct VisibleNodeSet
    {
        std::vector<RenderMeshNode> directional_light;
        std::vector<RenderMeshNode> point_lights;
        std::vector<RenderMeshNode> main_camera;
        std::array<std::vector<RenderMeshNode>, 2> main_camera_per_viewport;
        std::array<std::vector<RenderMeshNode>, 2> opaque_per_viewport;
        std::array<std::vector<RenderMeshNode>, 2> forward_per_viewport;
        std::array<std::vector<RenderMeshNode>, 2> transparent_per_viewport;
        std::array<std::vector<RenderMeshNode>, 2> sky_per_viewport;
        RenderAxisNode axis_node {};
    };

    // clear
    void clear();

    // Rotate to the next set. Called once per frame, before any
    // UpdateVisibleObjects() call (the scene is updated once per camera).
    void BeginVisibleNodeUpdate();
    // Point the render passes at the set that was just filled. Called once per
    // frame, after every UpdateVisibleObjects() call.
    void PublishVisibleNodes();

    // update visible objects in each frame
    void UpdateVisibleObjects(std::shared_ptr<RenderResource> render_resource, std::shared_ptr<RenderCamera> camera);
    const std::vector<RenderMeshNode>& GetMainCameraVisibleMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraOpaqueMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraForwardMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraTransparentMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraSkyMeshNodes(ViewportType viewport) const;

    // 阶段 69：方向光 shadow map 画的就是这个列表（见
    // `DirectionalLightShadowPass::DrawModel`），所以它的长度就是"这一帧到底往
    // shadow map 里画了几个物体"。心跳探针用它来分辨"阴影开关开了"和"真的有东西
    // 被画进去" —— 只报开关的话，可见节点是空的时候照样是假绿。
    const std::vector<RenderMeshNode>& GetDirectionalLightVisibleMeshNodes() const;

    // set visible nodes ptr in render pass

    void SetVisibleNodesReference();

private:
    VisibleNodeSet& writeNodeSet() { return m_VisibleNodeSets[m_VisibleNodeWriteIndex]; }
    const VisibleNodeSet& publishedNodeSet() const { return m_VisibleNodeSets[m_VisibleNodeWriteIndex]; }

    std::array<VisibleNodeSet, kVisibleNodeSetCount> m_VisibleNodeSets;
    size_t m_VisibleNodeWriteIndex {0};

public:

    GuidAllocator<GameObjectPartId>& GetInstanceIdAllocator();
    GuidAllocator<MeshSourceDesc>& GetMeshAssetIdAllocator();
    GuidAllocator<MaterialSourceDesc>& GetMaterialAssetdAllocator();

    void AddInstanceIdToMap(uint32_t instance_id, GObjectID go_id);
    GObjectID GetGObjectIDByMeshID(uint32_t mesh_id) const;
    void UpsertGameObject(RHI* rhi, RenderResourceBase& render_resource, const GameObjectDesc& gobject);
    void DeleteEntityByGObjectID(GObjectID go_id);

    void ClearForLevelReloading();

    // Rebuild m_PointLightList from enabled LightComponent instances on the active level.
    void SyncPointLightsFromLevel(Level* level);

    void SetUploadRhi(RHI* rhi) { m_UploadRhi = rhi; }

    // Editor gizmo axis meshes (registered once from RenderSystem::CreateAxis).
    void RegisterAxisMeshSources(const std::array<size_t, 3>& mesh_asset_ids,
                                 const std::array<RenderMeshData, 3>& mesh_datas);
    const RenderMeshData* FindAxisMeshSourceData(size_t mesh_asset_id) const;

private:
    RenderEntity BuildRenderEntity(RHI* rhi,
                                   RenderResourceBase& render_resource,
                                   GObjectID go_id,
                                   size_t part_index,
                                   const GameObjectPartDesc& game_object_part);
    MaterialSourceDesc BuildMaterialSourceDesc(const GameObjectPartDesc& game_object_part) const;
    void UpsertRenderEntity(const RenderEntity& render_entity);
    void DeleteEntityByPartID(const GameObjectPartId& part_id);
    void RemoveStalePartEntities(GObjectID go_id, size_t valid_part_count);

    GuidAllocator<GameObjectPartId> m_InstanceIdAllocator;
    GuidAllocator<MeshSourceDesc> m_MeshAssetIdAllocator;
    GuidAllocator<MaterialSourceDesc> m_MaterialAssetIdAllocator;

    std::unordered_map<uint32_t, GObjectID> m_MeshObjectIdMap;

    RHI* m_UploadRhi {nullptr};
    bool m_AxisMeshSourcesRegistered {false};
    std::array<size_t, 3> m_AxisMeshAssetIds {};
    std::array<RenderMeshData, 3> m_AxisMeshDatas {};

    void UpdateVisibleObjectsDirectionalLight(VisibleNodeSet& nodes,
                                              std::shared_ptr<RenderResource> render_resource,

                                              std::shared_ptr<RenderCamera> camera);
    void UpdateVisibleObjectsPointLight(VisibleNodeSet& nodes, std::shared_ptr<RenderResource> render_resource);
    void UpdateVisibleObjectsMainCamera(VisibleNodeSet& nodes,
                                        std::shared_ptr<RenderResource> render_resource,
                                        std::shared_ptr<RenderCamera> camera);
    void UpdateVisibleObjectsAxis(VisibleNodeSet& nodes, std::shared_ptr<RenderResource> render_resource);
    void UpdateVisibleObjectsParticle(std::shared_ptr<RenderResource> render_resource);
};