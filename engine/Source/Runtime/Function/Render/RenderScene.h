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

    // axis, for editor
    std::optional<RenderEntity> m_RenderAxis;

    // visible objects (updated per frame)
    std::vector<RenderMeshNode> m_DirectionalLightVisibleMeshNodes;
    std::vector<RenderMeshNode> m_PointLightsVisibleMeshNodes;
    std::array<std::vector<RenderMeshNode>, 2> m_MainCameraVisibleMeshNodesPerViewport;
    std::array<std::vector<RenderMeshNode>, 2> m_MainCameraOpaqueMeshNodesPerViewport;
    std::array<std::vector<RenderMeshNode>, 2> m_MainCameraForwardMeshNodesPerViewport;
    std::array<std::vector<RenderMeshNode>, 2> m_MainCameraTransparentMeshNodesPerViewport;
    std::vector<RenderMeshNode> m_MainCameraVisibleMeshNodes;
    RenderAxisNode m_AxisNode;

    // clear
    void clear();

    // update visible objects in each frame
    void UpdateVisibleObjects(std::shared_ptr<RenderResource> render_resource, std::shared_ptr<RenderCamera> camera);
    const std::vector<RenderMeshNode>& GetMainCameraVisibleMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraOpaqueMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraForwardMeshNodes(ViewportType viewport) const;
    const std::vector<RenderMeshNode>& GetMainCameraTransparentMeshNodes(ViewportType viewport) const;

    // set visible nodes ptr in render pass

    void SetVisibleNodesReference();

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

    void UpdateVisibleObjectsDirectionalLight(std::shared_ptr<RenderResource> render_resource,

                                              std::shared_ptr<RenderCamera> camera);
    void UpdateVisibleObjectsPointLight(std::shared_ptr<RenderResource> render_resource);
    void UpdateVisibleObjectsMainCamera(std::shared_ptr<RenderResource> render_resource,
                                        std::shared_ptr<RenderCamera> camera);
    void UpdateVisibleObjectsAxis(std::shared_ptr<RenderResource> render_resource);
    void UpdateVisibleObjectsParticle(std::shared_ptr<RenderResource> render_resource);
};