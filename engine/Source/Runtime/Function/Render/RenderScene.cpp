#include "Runtime/Function/Render/RenderScene.h"

#include "Runtime/Function/Framework/Component/Light/LightComponent.h"
#include "Runtime/Function/Framework/Component/Transform/TransformComponent.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Render/RenderHelper.h"
#include "Runtime/Function/Render/RenderPass.h"
#include "Runtime/Function/Render/RenderResource.h"
#include "Runtime/Function/Render/RenderResourceBase.h"
#include "Runtime/Profiler/Profiler.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <algorithm>
#include <cctype>

namespace
{
    std::string ToUpperCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        return value;
    }

    bool EqualsIgnoreCase(const std::string& lhs, const std::string& rhs)
    {
        return ToUpperCopy(lhs) == ToUpperCopy(rhs);
    }

    bool IsBlendModeEnabled(const std::string& blend)
    {
        const std::string normalized_blend = ToUpperCopy(blend);
        return !(normalized_blend.empty() || normalized_blend == "OFF" || normalized_blend == "OPAQUE");
    }

    bool ShouldTrackVisibilityDebug(const eastl::string& mesh_asset)
    {
        return mesh_asset.find("cube.mesh.json") != eastl::string::npos;
    }

    const char* ViewportTypeName(ViewportType viewport_type)
    {
        return viewport_type == ViewportType::game ? "Game" : "Scene";
    }

    std::unordered_set<uint32_t> g_debug_tracked_instances;
    std::unordered_map<uint32_t, bool> g_debug_last_main_camera_visibility;

#if defined(Z_HAS_VULKAN)
    const VulkanShaderPassData* FindShaderPassByLightMode(const VulkanPBRMaterial& material, const char* light_mode)
    {
        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (EqualsIgnoreCase(shader_pass.light_mode, light_mode != nullptr ? light_mode : ""))
            {
                return &shader_pass;
            }
        }
        return nullptr;
    }

    bool ShouldRenderTransparent(const RenderEntity& entity, const VulkanPBRMaterial& material)
    {
        if (entity.m_Blend || IsBlendModeEnabled(material.blend))
        {
            return true;
        }

        if (FindShaderPassByLightMode(material, "Transparent") != nullptr)
        {
            return true;
        }

        for (const VulkanShaderPassData& shader_pass : material.shader_passes)
        {
            if (IsBlendModeEnabled(shader_pass.blend))
            {
                return true;
            }
        }

        return false;
    }

    bool ShouldRenderForward(const VulkanPBRMaterial& material)
    {
        if (FindShaderPassByLightMode(material, "ForwardBase") != nullptr ||
            FindShaderPassByLightMode(material, "Forward") != nullptr)
        {
            return true;
        }

        if (!material.light_mode.empty() &&
            (EqualsIgnoreCase(material.light_mode, "ForwardBase") || EqualsIgnoreCase(material.light_mode, "Forward")))
        {
            return true;
        }

        return false;
    }
#endif

    float CalculateCameraDistanceSquared(const Matrix4x4& model_matrix, const Vector3& camera_position)
    {
        return model_matrix.GetTrans().squaredDistance(camera_position);
    }
}  // namespace

void RenderScene::clear()
{
    g_debug_tracked_instances.clear();
    g_debug_last_main_camera_visibility.clear();
}

void RenderScene::SyncPointLightsFromLevel(Level* level)
{
    m_PointLightList.m_Lights.clear();
    if (level == nullptr)
    {
        return;
    }

    for (const auto& [go_id, game_object] : level->getAllGObjects())
    {
        (void)go_id;
        if (game_object == nullptr)
        {
            continue;
        }
        LightComponent* light_component = game_object->tryGetComponent(LightComponent);
        if (light_component == nullptr || !light_component->isEnabled())
        {
            continue;
        }
        TransformComponent* transform = game_object->tryGetComponent(TransformComponent);
        if (transform == nullptr)
        {
            continue;
        }

        PointLight point_light {};
        point_light.m_Position = transform->GetPosition();
        point_light.m_Flux       = light_component->GetFlux();
        m_PointLightList.m_Lights.push_back(point_light);
    }
}

void RenderScene::UpdateVisibleObjects(std::shared_ptr<RenderResource> render_resource,
                                       std::shared_ptr<RenderCamera> camera)
{
    Z_PROFILE_SCOPE("RenderScene::updateVisibleObjects");
    UpdateVisibleObjectsDirectionalLight(render_resource, camera);
    UpdateVisibleObjectsPointLight(render_resource);
    UpdateVisibleObjectsMainCamera(render_resource, camera);
    UpdateVisibleObjectsAxis(render_resource);
    UpdateVisibleObjectsParticle(render_resource);
}

const std::vector<RenderMeshNode>& RenderScene::GetMainCameraVisibleMeshNodes(ViewportType viewport) const
{
    return m_MainCameraVisibleMeshNodesPerViewport[static_cast<size_t>(viewport)];
}

const std::vector<RenderMeshNode>& RenderScene::GetMainCameraOpaqueMeshNodes(ViewportType viewport) const
{
    return m_MainCameraOpaqueMeshNodesPerViewport[static_cast<size_t>(viewport)];
}

const std::vector<RenderMeshNode>& RenderScene::GetMainCameraForwardMeshNodes(ViewportType viewport) const
{
    return m_MainCameraForwardMeshNodesPerViewport[static_cast<size_t>(viewport)];
}

const std::vector<RenderMeshNode>& RenderScene::GetMainCameraTransparentMeshNodes(ViewportType viewport) const
{
    return m_MainCameraTransparentMeshNodesPerViewport[static_cast<size_t>(viewport)];
}

void RenderScene::SetVisibleNodesReference()

{
    RenderPass::m_VisiableNodes.p_directional_light_visible_mesh_nodes = &m_DirectionalLightVisibleMeshNodes;
    RenderPass::m_VisiableNodes.p_point_lights_visible_mesh_nodes = &m_PointLightsVisibleMeshNodes;
    RenderPass::m_VisiableNodes.p_main_camera_visible_mesh_nodes = &m_MainCameraVisibleMeshNodes;
    RenderPass::m_VisiableNodes.p_axis_node = &m_AxisNode;
}

GuidAllocator<GameObjectPartId>& RenderScene::GetInstanceIdAllocator()
{
    return m_InstanceIdAllocator;
}

GuidAllocator<MeshSourceDesc>& RenderScene::GetMeshAssetIdAllocator()
{
    return m_MeshAssetIdAllocator;
}

GuidAllocator<MaterialSourceDesc>& RenderScene::GetMaterialAssetdAllocator()
{
    return m_MaterialAssetIdAllocator;
}

void RenderScene::AddInstanceIdToMap(uint32_t instance_id, GObjectID go_id)
{
    m_MeshObjectIdMap[instance_id] = go_id;
}

GObjectID RenderScene::GetGObjectIDByMeshID(uint32_t mesh_id) const
{
    auto find_it = m_MeshObjectIdMap.find(mesh_id);
    if (find_it != m_MeshObjectIdMap.end())
    {
        return find_it->second;
    }
    return GObjectID();
}

MaterialSourceDesc RenderScene::BuildMaterialSourceDesc(const GameObjectPartDesc& game_object_part) const
{
    if (game_object_part.m_MaterialDesc.m_WithTexture)
    {
        MaterialSourceDesc desc;
        desc.m_MaterialAsset = game_object_part.m_MaterialDesc.m_MaterialAsset;
        desc.m_Shader = game_object_part.m_MaterialDesc.m_Shader.empty() ? "StandardLit"
                                                                         : game_object_part.m_MaterialDesc.m_Shader;
        desc.m_BaseColorFile = game_object_part.m_MaterialDesc.m_BaseColorTextureFile;
        desc.m_MetallicRoughnessFile = game_object_part.m_MaterialDesc.m_MetallicRoughnessTextureFile;
        desc.m_NormalFile = game_object_part.m_MaterialDesc.m_NormalTextureFile;
        desc.m_OcclusionFile = game_object_part.m_MaterialDesc.m_OcclusionTextureFile;
        desc.m_EmissiveFile = game_object_part.m_MaterialDesc.m_EmissiveTextureFile;
        desc.m_EnabledShaderKeywords = game_object_part.m_MaterialDesc.m_EnabledShaderKeywords;
        return desc;
    }

    return {"",
            "StandardLit",
            GET_SYSTEM(AssetManager)->GetFullPath("asset/texture/default/albedo.jpg").generic_string().c_str(),
            GET_SYSTEM(AssetManager)->GetFullPath("asset/texture/default/mr.jpg").generic_string().c_str(),
            GET_SYSTEM(AssetManager)->GetFullPath("asset/texture/default/normal.jpg").generic_string().c_str(),
            "",
            ""};
}

RenderEntity RenderScene::BuildRenderEntity(std::shared_ptr<RHI> rhi,
                                            RenderResourceBase& render_resource,
                                            GObjectID go_id,
                                            size_t part_index,
                                            const GameObjectPartDesc& game_object_part)
{
    const GameObjectPartId part_id {go_id, part_index};

    RenderEntity render_entity;
    render_entity.m_InstanceId = static_cast<uint32_t>(m_InstanceIdAllocator.allocGuid(part_id));
    render_entity.m_ModelMatrix = game_object_part.m_TransformDesc.m_TransformMatrix;
    AddInstanceIdToMap(render_entity.m_InstanceId, go_id);

    const MeshSourceDesc mesh_source {game_object_part.m_MeshDesc.m_MeshAsset};
    const bool mesh_cache_registered = m_MeshAssetIdAllocator.hasElement(mesh_source);
    const AxisAlignedBox cached_bounding_box =
        mesh_cache_registered ? render_resource.GetCachedBoudingBox(mesh_source) : AxisAlignedBox();
    const bool mesh_cache_hit = mesh_cache_registered && cached_bounding_box.IsValid();

    RenderMeshData mesh_data;
    if (!mesh_cache_hit)
    {
        mesh_data = render_resource.LoadMeshData(mesh_source, render_entity.m_BoundingBox);
    }
    else
    {
        render_entity.m_BoundingBox = cached_bounding_box;
    }

    bool mesh_has_geometry = mesh_cache_hit;
    if (!mesh_cache_hit)
    {
        mesh_has_geometry =
            mesh_data.m_StaticMeshData.m_VertexBuffer != nullptr && mesh_data.m_StaticMeshData.m_VertexBuffer->m_Size > 0 &&
            mesh_data.m_StaticMeshData.m_IndexBuffer != nullptr && mesh_data.m_StaticMeshData.m_IndexBuffer->m_Size > 0;
    }
    render_entity.m_EnableVertexBlending = game_object_part.m_SkeletonAnimationResult.m_Transforms.size() > 1;
    render_entity.m_JointMatrices.resize(game_object_part.m_SkeletonAnimationResult.m_Transforms.size());
    for (size_t transform_index = 0; transform_index < game_object_part.m_SkeletonAnimationResult.m_Transforms.size(); ++transform_index)
    {
        render_entity.m_JointMatrices[transform_index] =
            game_object_part.m_SkeletonAnimationResult.m_Transforms[transform_index].m_Matrix;
    }

    render_entity.m_Blend = game_object_part.m_MaterialDesc.m_IsBlend;
    render_entity.m_DoubleSided = game_object_part.m_MaterialDesc.m_IsDoubleSided;
    // Built-in texture fallback (no project Material): always render both faces.
    if (!game_object_part.m_MaterialDesc.m_WithTexture)
    {
        render_entity.m_DoubleSided = true;
    }
    render_entity.m_BaseColorFactor = game_object_part.m_MaterialDesc.m_BaseColorFactor;
    render_entity.m_MetallicFactor = game_object_part.m_MaterialDesc.m_MetallicFactor;
    render_entity.m_RoughnessFactor = game_object_part.m_MaterialDesc.m_RoughnessFactor;
    render_entity.m_NormalScale = game_object_part.m_MaterialDesc.m_NormalScale;
    render_entity.m_OcclusionStrength = game_object_part.m_MaterialDesc.m_OcclusionStrength;
    render_entity.m_EmissiveFactor = game_object_part.m_MaterialDesc.m_EmissiveFactor;

    const MaterialSourceDesc material_source = BuildMaterialSourceDesc(game_object_part);
    const bool is_material_loaded = m_MaterialAssetIdAllocator.hasElement(material_source);

    RenderMaterialData material_data;
    if (!is_material_loaded)
    {
        material_data = render_resource.LoadMaterialData(material_source);
    }

    render_entity.m_MaterialAssetId = m_MaterialAssetIdAllocator.allocGuid(material_source);

    if (!mesh_has_geometry)
    {
        LOG_WARNING(ZRender,
                    "buildRenderEntity skipped mesh upload: asset='{}' go_id={} part={} cache_hit={}",
                    mesh_source.m_MeshAsset.c_str(),
                    go_id,
                    part_index,
                    mesh_cache_hit);
        return render_entity;
    }

    render_entity.m_MeshAssetId = m_MeshAssetIdAllocator.allocGuid(mesh_source);

    if (!mesh_cache_hit)
    {
        render_resource.UploadGameObjectRenderResource(rhi, render_entity, mesh_data);
    }

    if (!is_material_loaded)
    {
        render_resource.UploadGameObjectRenderResource(rhi, render_entity, material_data);
    }

    if (ShouldTrackVisibilityDebug(mesh_source.m_MeshAsset))
    {
        const Vector3 model_position = render_entity.m_ModelMatrix.GetTrans();
        const Vector3 bbox_min = render_entity.m_BoundingBox.getMinCorner();
        const Vector3 bbox_max = render_entity.m_BoundingBox.getMaxCorner();
        g_debug_tracked_instances.insert(render_entity.m_InstanceId);
        g_debug_last_main_camera_visibility.erase(render_entity.m_InstanceId);
        LOG_INFO(ZRender,
                 "buildRenderEntity tracked mesh='{}' go_id={} part={} instance={} mesh_cached={} material_cached={} material_asset='{}' model_pos=({}, {}, {}) bbox_min=({}, {}, {}) bbox_max=({}, {}, {})",
                 mesh_source.m_MeshAsset.c_str(),
                 go_id,
                 part_index,
                 render_entity.m_InstanceId,
                 mesh_cache_hit,
                 is_material_loaded,
                 material_source.m_MaterialAsset.c_str(),
                 model_position.x,
                 model_position.y,
                 model_position.z,
                 bbox_min.x,
                 bbox_min.y,
                 bbox_min.z,
                 bbox_max.x,
                 bbox_max.y,
                 bbox_max.z);
    }

    return render_entity;
}

void RenderScene::UpsertRenderEntity(const RenderEntity& render_entity)
{
    for (RenderEntity& entity : m_RenderEntities)
    {
        if (entity.m_InstanceId == render_entity.m_InstanceId)
        {
            entity = render_entity;
            return;
        }
    }

    m_RenderEntities.push_back(render_entity);
}

void RenderScene::DeleteEntityByPartID(const GameObjectPartId& part_id)
{
    size_t guid = s_InvalidGuid;
    if (!m_InstanceIdAllocator.getElementGuid(part_id, guid))
    {
        return;
    }

    m_InstanceIdAllocator.freeElement(part_id);
    m_MeshObjectIdMap.erase(static_cast<uint32_t>(guid));
    g_debug_tracked_instances.erase(static_cast<uint32_t>(guid));
    g_debug_last_main_camera_visibility.erase(static_cast<uint32_t>(guid));
    m_RenderEntities.erase(std::remove_if(m_RenderEntities.begin(),
                                          m_RenderEntities.end(),
                                          [guid](const RenderEntity& entity) {
                                              return entity.m_InstanceId == guid;
                                          }),
                           m_RenderEntities.end());
}

void RenderScene::RemoveStalePartEntities(GObjectID go_id, size_t valid_part_count)
{
    const std::vector<size_t> allocated_guids = m_InstanceIdAllocator.getAllocatedGuids();
    for (size_t guid : allocated_guids)
    {
        GameObjectPartId allocated_part_id;
        if (!m_InstanceIdAllocator.getGuidRelatedElement(guid, allocated_part_id))
        {
            continue;
        }

        if (allocated_part_id.m_GoId == go_id && allocated_part_id.m_PartId >= valid_part_count)
        {
            DeleteEntityByPartID(allocated_part_id);
        }
    }
}

void RenderScene::UpsertGameObject(std::shared_ptr<RHI> rhi, RenderResourceBase& render_resource, const GameObjectDesc& gobject)
{
    const std::vector<GameObjectPartDesc>& object_parts = gobject.getObjectParts();
    for (size_t part_index = 0; part_index < object_parts.size(); ++part_index)
    {
        const RenderEntity render_entity =
            BuildRenderEntity(rhi, render_resource, gobject.getId(), part_index, object_parts[part_index]);
        if (!render_entity.m_BoundingBox.IsValid())
        {
            continue;
        }
        UpsertRenderEntity(render_entity);
    }

    RemoveStalePartEntities(gobject.getId(), object_parts.size());
}

void RenderScene::DeleteEntityByGObjectID(GObjectID go_id)
{
    const std::vector<size_t> allocated_guids = m_InstanceIdAllocator.getAllocatedGuids();
    for (size_t guid : allocated_guids)
    {
        GameObjectPartId allocated_part_id;
        if (!m_InstanceIdAllocator.getGuidRelatedElement(guid, allocated_part_id))
        {
            continue;
        }

        if (allocated_part_id.m_GoId == go_id)
        {
            DeleteEntityByPartID(allocated_part_id);
        }
    }
}

void RenderScene::ClearForLevelReloading()
{
    m_InstanceIdAllocator.clear();
    m_MeshObjectIdMap.clear();
    m_RenderEntities.clear();
    g_debug_tracked_instances.clear();
    g_debug_last_main_camera_visibility.clear();
}

void RenderScene::UpdateVisibleObjectsDirectionalLight(std::shared_ptr<RenderResource> render_resource,
                                                       std::shared_ptr<RenderCamera> camera)
{
    Matrix4x4 directional_light_proj_view = CalculateDirectionalLightCamera(*this, *camera);
    ViewportType viewport_type =
        (camera->m_CurrentCameraType == RenderCameraType::Game) ? ViewportType::game : ViewportType::scene;

    render_resource->m_MeshPerframeStorageBufferObjects[static_cast<size_t>(viewport_type)].directional_light_proj_view =
        directional_light_proj_view;
    render_resource->m_MeshPerframeStorageBufferObject.directional_light_proj_view = directional_light_proj_view;
    render_resource->m_MeshDirectionalLightShadowPerframeStorageBufferObject.light_proj_view =
        directional_light_proj_view;

    m_DirectionalLightVisibleMeshNodes.clear();

    ClusterFrustum frustum =
        CreateClusterFrustumFromMatrix(directional_light_proj_view, -1.0, 1.0, -1.0, 1.0, 0.0, 1.0);

    for (const RenderEntity& entity : m_RenderEntities)
    {
        BoundingBox mesh_asset_bounding_box {entity.m_BoundingBox.getMinCorner(),
                                             entity.m_BoundingBox.getMaxCorner()};

        if (TiledFrustumIntersectBox(frustum, BoundingBoxTransform(mesh_asset_bounding_box, entity.m_ModelMatrix)))
        {
            m_DirectionalLightVisibleMeshNodes.emplace_back();
            RenderMeshNode& temp_node = m_DirectionalLightVisibleMeshNodes.back();

            temp_node.model_matrix = &entity.m_ModelMatrix;

            assert(entity.m_JointMatrices.size() <= s_MeshVertexBlendingMaxJointCount);
            if (!entity.m_JointMatrices.empty())
            {
                temp_node.joint_count = static_cast<uint32_t>(entity.m_JointMatrices.size());
                temp_node.joint_matrices = entity.m_JointMatrices.data();
            }
            temp_node.node_id = entity.m_InstanceId;

            RenderMeshGPUResource& mesh_asset = render_resource->GetEntityMesh(entity);
            temp_node.ref_mesh = &mesh_asset;
            temp_node.enable_vertex_blending = entity.m_EnableVertexBlending;

            RenderMaterialGPUResource& material_asset = render_resource->GetEntityMaterial(entity);
            temp_node.ref_material = &material_asset;
        }
    }
}

void RenderScene::UpdateVisibleObjectsPointLight(std::shared_ptr<RenderResource> render_resource)
{
    m_PointLightsVisibleMeshNodes.clear();

    std::vector<BoundingSphere> point_lights_bounding_spheres;
    uint32_t point_light_num = static_cast<uint32_t>(m_PointLightList.m_Lights.size());
    point_lights_bounding_spheres.resize(point_light_num);
    for (size_t i = 0; i < point_light_num; i++)
    {
        point_lights_bounding_spheres[i].m_Center = m_PointLightList.m_Lights[i].m_Position;
        point_lights_bounding_spheres[i].m_Radius = m_PointLightList.m_Lights[i].calculateRadius();
    }

    for (const RenderEntity& entity : m_RenderEntities)
    {
        BoundingBox mesh_asset_bounding_box {entity.m_BoundingBox.getMinCorner(),
                                             entity.m_BoundingBox.getMaxCorner()};

        bool intersect_with_point_lights = true;
        for (size_t i = 0; i < point_light_num; i++)
        {
            if (!BoxIntersectsWithSphere(BoundingBoxTransform(mesh_asset_bounding_box, entity.m_ModelMatrix),
                                         point_lights_bounding_spheres[i]))
            {
                intersect_with_point_lights = false;
                break;
            }
        }

        if (intersect_with_point_lights)
        {
            m_PointLightsVisibleMeshNodes.emplace_back();
            RenderMeshNode& temp_node = m_PointLightsVisibleMeshNodes.back();

            temp_node.model_matrix = &entity.m_ModelMatrix;

            assert(entity.m_JointMatrices.size() <= s_MeshVertexBlendingMaxJointCount);
            if (!entity.m_JointMatrices.empty())
            {
                temp_node.joint_count = static_cast<uint32_t>(entity.m_JointMatrices.size());
                temp_node.joint_matrices = entity.m_JointMatrices.data();
            }
            temp_node.node_id = entity.m_InstanceId;

            RenderMeshGPUResource& mesh_asset = render_resource->GetEntityMesh(entity);
            temp_node.ref_mesh = &mesh_asset;
            temp_node.enable_vertex_blending = entity.m_EnableVertexBlending;

            RenderMaterialGPUResource& material_asset = render_resource->GetEntityMaterial(entity);
            temp_node.ref_material = &material_asset;
        }
    }
}

void RenderScene::UpdateVisibleObjectsMainCamera(std::shared_ptr<RenderResource> render_resource,
                                                 std::shared_ptr<RenderCamera> camera)
{
    ViewportType viewport_type =
        (camera->m_CurrentCameraType == RenderCameraType::Game) ? ViewportType::game : ViewportType::scene;
    const size_t viewport_index = static_cast<size_t>(viewport_type);

    auto& visible_mesh_nodes = m_MainCameraVisibleMeshNodesPerViewport[viewport_index];
    auto& opaque_mesh_nodes = m_MainCameraOpaqueMeshNodesPerViewport[viewport_index];
    auto& forward_mesh_nodes = m_MainCameraForwardMeshNodesPerViewport[viewport_index];
    auto& transparent_mesh_nodes = m_MainCameraTransparentMeshNodesPerViewport[viewport_index];
    visible_mesh_nodes.clear();
    opaque_mesh_nodes.clear();
    forward_mesh_nodes.clear();
    transparent_mesh_nodes.clear();

    Matrix4x4 view_matrix = camera->GetViewMatrix();
    Matrix4x4 proj_matrix = camera->GetPersProjMatrix();
    Matrix4x4 proj_view_matrix = proj_matrix * view_matrix;
    Vector3 camera_position = camera->position();

    ClusterFrustum f = CreateClusterFrustumFromMatrix(proj_view_matrix, -1.0, 1.0, -1.0, 1.0, 0.0, 1.0);

    for (const RenderEntity& entity : m_RenderEntities)
    {
        BoundingBox mesh_asset_bounding_box {entity.m_BoundingBox.getMinCorner(),
                                             entity.m_BoundingBox.getMaxCorner()};
        const BoundingBox world_bounding_box = BoundingBoxTransform(mesh_asset_bounding_box, entity.m_ModelMatrix);
        const bool is_visible = TiledFrustumIntersectBox(f, world_bounding_box);

        if (g_debug_tracked_instances.count(entity.m_InstanceId) > 0)
        {
            const auto visibility_it = g_debug_last_main_camera_visibility.find(entity.m_InstanceId);
            if (visibility_it == g_debug_last_main_camera_visibility.end() || visibility_it->second != is_visible)
            {
                const Vector3 model_position = entity.m_ModelMatrix.GetTrans();
                const Vector3 bbox_min = entity.m_BoundingBox.getMinCorner();
                const Vector3 bbox_max = entity.m_BoundingBox.getMaxCorner();
                LOG_INFO(ZRender,
                         "main camera visibility viewport={} instance={} visible={} model_pos=({}, {}, {}) local_bbox_min=({}, {}, {}) local_bbox_max=({}, {}, {})",
                         ViewportTypeName(viewport_type),
                         entity.m_InstanceId,
                         is_visible,
                         model_position.x,
                         model_position.y,
                         model_position.z,
                         bbox_min.x,
                         bbox_min.y,
                         bbox_min.z,
                         bbox_max.x,
                         bbox_max.y,
                         bbox_max.z);
                g_debug_last_main_camera_visibility[entity.m_InstanceId] = is_visible;
            }
        }

        if (!is_visible)
        {
            continue;
        }

        RenderMeshNode temp_node;
        temp_node.model_matrix = &entity.m_ModelMatrix;

        assert(entity.m_JointMatrices.size() <= s_MeshVertexBlendingMaxJointCount);
        if (!entity.m_JointMatrices.empty())
        {
            temp_node.joint_count = static_cast<uint32_t>(entity.m_JointMatrices.size());
            temp_node.joint_matrices = entity.m_JointMatrices.data();
        }
        temp_node.node_id = entity.m_InstanceId;

        RenderMeshGPUResource& mesh_asset = render_resource->GetEntityMesh(entity);
        temp_node.ref_mesh = &mesh_asset;
        temp_node.enable_vertex_blending = entity.m_EnableVertexBlending;

        RenderMaterialGPUResource& material_asset = render_resource->GetEntityMaterial(entity);
        temp_node.ref_material = &material_asset;

        visible_mesh_nodes.emplace_back(temp_node);

#if defined(Z_HAS_VULKAN)
        VulkanPBRMaterial* vulkan_material = AsVulkanMaterialource(temp_node.ref_material);
        if (vulkan_material != nullptr && ShouldRenderTransparent(entity, *vulkan_material))
        {
            transparent_mesh_nodes.emplace_back(temp_node);
        }
        else if (vulkan_material != nullptr && ShouldRenderForward(*vulkan_material) &&
                 FindShaderPassByLightMode(*vulkan_material, "GBuffer") == nullptr)
        {
            forward_mesh_nodes.emplace_back(temp_node);
        }
        else
        {
            opaque_mesh_nodes.emplace_back(temp_node);
        }
#else
        if (entity.m_Blend)
        {
            transparent_mesh_nodes.emplace_back(temp_node);
        }
        else
        {
            opaque_mesh_nodes.emplace_back(temp_node);
        }
#endif
    }

    std::sort(transparent_mesh_nodes.begin(),
              transparent_mesh_nodes.end(),
              [&camera_position](const RenderMeshNode& lhs, const RenderMeshNode& rhs) {
                  const float lhs_distance = CalculateCameraDistanceSquared(*lhs.model_matrix, camera_position);
                  const float rhs_distance = CalculateCameraDistanceSquared(*rhs.model_matrix, camera_position);
                  return lhs_distance > rhs_distance;
              });

    m_MainCameraVisibleMeshNodes = visible_mesh_nodes;
}

void RenderScene::UpdateVisibleObjectsAxis(std::shared_ptr<RenderResource> render_resource)
{
    if (m_RenderAxis.has_value())
    {
        RenderEntity& axis = *m_RenderAxis;

        m_AxisNode.model_matrix = axis.m_ModelMatrix;
        m_AxisNode.node_id = axis.m_InstanceId;

        RenderMeshGPUResource& mesh_asset = render_resource->GetEntityMesh(axis);
        m_AxisNode.ref_mesh = &mesh_asset;
        m_AxisNode.enable_vertex_blending = axis.m_EnableVertexBlending;
    }
}

void RenderScene::UpdateVisibleObjectsParticle(std::shared_ptr<RenderResource> render_resource)
{
    // TODO
}