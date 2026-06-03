#include "EditorSceneManager.h"

#include "Editor/EditorProjectPrefs/EditorProjectPrefs.h"
#include "Editor/WorldPartition/WorldPartitionEditorDebug.h"
#include "Editor/EditorUI/ContentBrowser/ContentBrowserHelpers.h"
#include "Editor/Platform/Interface/EditorUtility.h"
#include "Runtime/Project/ProjectInfo.h"
#include "core/Log/LogSystem.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Function/Framework/Component/Mesh/MeshRenderer.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSystem.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"
#include "Runtime/Resource/ResType/Data/MeshData.h"

#include <Application/Application.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>

namespace
{
    // Same pose as RenderSystem scene editor camera bootstrap (Unity-like 3/4 view).
    const Vector3 kDefaultSceneView3DOffset(0.0f, -10.0f, 5.0f);

    void ApplyDefaultSceneView3DPose(const std::shared_ptr<RenderCamera>& camera)
    {
        if (camera == nullptr)
        {
            return;
        }

        // Keep the XY focus from 2D panning; restore classic perspective direction.
        const Vector3 pivot(camera->position().x, camera->position().y, 0.0f);
        const Vector3 eye = pivot + kDefaultSceneView3DOffset;
        camera->LookAt(eye, pivot, Vector3::UNIT_Z);
    }

    bool ComputeMeshLocalBounds(const eastl::string& mesh_asset_path, AxisAlignedBox& out_bounds)
    {
        out_bounds = AxisAlignedBox();
        if (mesh_asset_path.empty())
        {
            return false;
        }

        const auto asset_manager = GET_SYSTEM(AssetManager);
        if (asset_manager == nullptr)
        {
            return false;
        }

        std::filesystem::path read_path = asset_manager->GetFullPath(mesh_asset_path);
        MeshData* mesh_data = asset_manager->ReadObject<MeshData>(read_path);
        if (mesh_data == nullptr)
        {
            return false;
        }

        for (const Vertex& vertex : mesh_data->vertex_buffer)
        {
            if (!std::isfinite(vertex.px) || !std::isfinite(vertex.py) || !std::isfinite(vertex.pz))
            {
                continue;
            }
            out_bounds.Merge(Vector3(vertex.px, vertex.py, vertex.pz));
        }
        return out_bounds.IsValid();
    }

    void MergeWorldBounds(const AxisAlignedBox& local_bounds, const Matrix4x4& world_matrix, AxisAlignedBox& combined_bounds, bool& has_bounds)
    {
        const Vector3 corners[8] = {
            Vector3(local_bounds.getMinCorner().x, local_bounds.getMinCorner().y, local_bounds.getMinCorner().z),
            Vector3(local_bounds.getMaxCorner().x, local_bounds.getMinCorner().y, local_bounds.getMinCorner().z),
            Vector3(local_bounds.getMinCorner().x, local_bounds.getMaxCorner().y, local_bounds.getMinCorner().z),
            Vector3(local_bounds.getMaxCorner().x, local_bounds.getMaxCorner().y, local_bounds.getMinCorner().z),
            Vector3(local_bounds.getMinCorner().x, local_bounds.getMinCorner().y, local_bounds.getMaxCorner().z),
            Vector3(local_bounds.getMaxCorner().x, local_bounds.getMinCorner().y, local_bounds.getMaxCorner().z),
            Vector3(local_bounds.getMinCorner().x, local_bounds.getMaxCorner().y, local_bounds.getMaxCorner().z),
            Vector3(local_bounds.getMaxCorner().x, local_bounds.getMaxCorner().y, local_bounds.getMaxCorner().z),
        };

        for (const Vector3& corner : corners)
        {
            const Vector3 world_corner = world_matrix * corner;
            if (!has_bounds)
            {
                combined_bounds = AxisAlignedBox();
                has_bounds = true;
            }
            combined_bounds.Merge(world_corner);
        }
    }

    bool TryGetSelectedMeshWorldBounds(const GameObject& game_object, Vector3& out_target, float& out_radius)
    {
        const MeshRenderer* mesh_renderer = game_object.tryGetComponentConst(MeshRenderer);
        if (mesh_renderer == nullptr)
        {
            return false;
        }

        const Transform* transform_component = game_object.tryGetComponentConst(Transform);
        const Matrix4x4 world_matrix =
            transform_component != nullptr ? transform_component->GetLocalToWorldMatrix() : Matrix4x4::IDENTITY;

        AxisAlignedBox combined_bounds;
        bool has_bounds = false;
        for (const SubMeshRes& sub_mesh : mesh_renderer->getSubMeshes())
        {
            AxisAlignedBox local_bounds;
            if (!ComputeMeshLocalBounds(sub_mesh.m_MeshAsset, local_bounds))
            {
                continue;
            }
            MergeWorldBounds(local_bounds, world_matrix, combined_bounds, has_bounds);
        }

        if (!has_bounds || !combined_bounds.IsValid())
        {
            return false;
        }

        out_target = combined_bounds.getCenter();
        const Vector3 extent = combined_bounds.getMaxCorner() - combined_bounds.getMinCorner();
        out_radius = Math::max(Math::max(extent.x, extent.y), extent.z) * 0.5f;
        out_radius = Math::max(out_radius, 0.5f);
        return true;
    }
}  // namespace

std::vector<std::type_index> EditorSceneManager::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(RenderSystem), GET_SYSTEM_TYPE(WindowSystem)};
}

bool EditorSceneManager::Initialize()
{
    return true;
}

void EditorSceneManager::Tick(float delta_time)
{
    if (m_PendingLastSceneRestore)
    {
        TryRestoreLastOpenedScene();
    }

    RefreshMainWindowTitle();

    if (auto world = GET_SYSTEM(WorldManager))
    {
        if (world->IsWorldPartitionEnabled())
        {
            Vector3 streaming_source = world->GetWorldPartitionStreamingSource();
            DrawWorldPartitionEditorOverlay(streaming_source);
        }
    }

    std::shared_ptr<GameObject> selected_gobject = GetSelectedGObject().lock();
    if (selected_gobject)
    {
        Transform* transform_component = selected_gobject->tryGetComponent(Transform);
        if (transform_component)
        {
            transform_component->setDirtyFlag(true);
        }
    }
}

float intersectPlaneRay(Vector3 normal, float d, Vector3 origin, Vector3 dir)
{
    float deno = normal.dotProduct(dir);
    if (fabs(deno) < 0.0001)
    {
        deno = 0.0001;
    }

    return -(normal.dotProduct(origin) + d) / deno;
}

size_t EditorSceneManager::UpdateCursorOnAxis(Vector2 cursor_uv, Vector2 game_engine_window_size)
{
    Vector3 camera_forward = m_Camera->forward();
    Vector3 camera_up = m_Camera->up();
    Vector3 camera_right = m_Camera->right();
    Vector3 camera_position = m_Camera->position();

    if (m_SelectedGobjectId == k_invalid_gobject_id)
    {
        return m_SelectedAxis;
    }
    RenderEntity* selected_aixs = GetAxisMeshByType(m_AxisMode);
    m_SelectedAxis = 3;
    if (m_IsShowAxis == false)
    {
        return m_SelectedAxis;
    }
    else
    {
        Matrix4x4 model_matrix = selected_aixs->m_ModelMatrix;
        Vector3 model_scale;
        Quaternion model_rotation;
        Vector3 model_translation;
        model_matrix.Decomposition(model_translation, model_scale, model_rotation);
        Vector2 screen_center_uv = Vector2(cursor_uv.x, 1 - cursor_uv.y) - Vector2(0.5, 0.5);
        Vector3 world_ray_dir;
        if (m_Camera->IsOrthographic())
        {
            const float half_h = m_Camera->GetOrthoHalfHeight();
            const float spread_x = screen_center_uv.x * 2.0f * half_h * std::max(m_Camera->getAspect(), 0.01f);
            const float spread_y = screen_center_uv.y * 2.0f * half_h;
            world_ray_dir = camera_forward + camera_right * spread_x + camera_up * spread_y;
            if (world_ray_dir.length() > 1e-4f)
            {
                world_ray_dir.normalise();
            }
        }
        else
        {
            const float camera_fov = m_Camera->getFovYDeprecated();
            const float window_forward =
                game_engine_window_size.y / 2.0f / Math::tan(Math::DegreesToRadians(camera_fov) / 2.0f);
            world_ray_dir = camera_forward * window_forward +
                            camera_right * (float)game_engine_window_size.x * screen_center_uv.x +
                            camera_up * (float)game_engine_window_size.y * screen_center_uv.y;
        }

        Vector4 local_ray_origin = model_matrix.inverse() * Vector4(camera_position, 1.0f);
        Vector3 local_ray_origin_xyz = Vector3(local_ray_origin.x, local_ray_origin.y, local_ray_origin.z);
        Quaternion inversed_rotation = model_rotation.inverse();
        inversed_rotation.normalise();
        Vector3 local_ray_dir = inversed_rotation * world_ray_dir;

        Vector3 plane_normals[3] = {Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)};

        float plane_view_depth[3] = {intersectPlaneRay(plane_normals[0], 0, local_ray_origin_xyz, local_ray_dir),
                                     intersectPlaneRay(plane_normals[1], 0, local_ray_origin_xyz, local_ray_dir),
                                     intersectPlaneRay(plane_normals[2], 0, local_ray_origin_xyz, local_ray_dir)};

        Vector3 intersect_pt[3] = {
            local_ray_origin_xyz + plane_view_depth[0] * local_ray_dir,  // yoz
            local_ray_origin_xyz + plane_view_depth[1] * local_ray_dir,  // xoz
            local_ray_origin_xyz + plane_view_depth[2] * local_ray_dir   // xoy
        };

        if ((int)m_AxisMode == 0 || (int)m_AxisMode == 2)  // transition axis & scale axis
        {
            const float DIST_THRESHOLD = 0.6f;
            const float EDGE_OF_AXIS_MIN = 0.1f;
            const float EDGE_OF_AXIS_MAX = 2.0f;
            const float AXIS_LENGTH = 2.0f;

            float max_dist = 0.0f;
            // whether the ray (camera to mouse point) on any plane
            for (int i = 0; i < 3; ++i)
            {
                float local_ray_dir_proj = Math::abs(local_ray_dir.dotProduct(plane_normals[i]));
                float cos_alpha = local_ray_dir_proj / 1.0f;  // local_ray_dir_proj / local_ray_dir.length
                if (cos_alpha <= 0.15)                        // cos(80deg)~cps(100deg)
                {
                    int index00 = (i + 1) % 3;
                    int index01 = 3 - i - index00;
                    int index10 = (i + 2) % 3;
                    int index11 = 3 - i - index10;
                    float axis_dist = (Math::abs(intersect_pt[index00][i]) + Math::abs(intersect_pt[index10][i])) / 2;
                    if (axis_dist > DIST_THRESHOLD)  // too far from axis
                    {
                        continue;
                    }
                    // which axis is closer
                    if ((intersect_pt[index00][index01] > EDGE_OF_AXIS_MIN) &&
                        (intersect_pt[index00][index01] < AXIS_LENGTH) && (intersect_pt[index00][index01] > max_dist) &&
                        (Math::abs(intersect_pt[index00][i]) < EDGE_OF_AXIS_MAX))
                    {
                        max_dist = intersect_pt[index00][index01];
                        m_SelectedAxis = index01;
                    }
                    if ((intersect_pt[index10][index11] > EDGE_OF_AXIS_MIN) &&
                        (intersect_pt[index10][index11] < AXIS_LENGTH) && (intersect_pt[index10][index11] > max_dist) &&
                        (Math::abs(intersect_pt[index10][i]) < EDGE_OF_AXIS_MAX))
                    {
                        max_dist = intersect_pt[index10][index11];
                        m_SelectedAxis = index11;
                    }
                }
            }
            // check axis
            if (m_SelectedAxis == 3)
            {
                float min_dist = 1e10f;
                for (int i = 0; i < 3; ++i)
                {
                    int index0 = (i + 1) % 3;
                    int index1 = (i + 2) % 3;
                    float dist = Math::Sqr(intersect_pt[index0][index1]) + Math::Sqr(intersect_pt[index1][index0]);
                    if ((intersect_pt[index0][i] > EDGE_OF_AXIS_MIN) && (intersect_pt[index0][i] < EDGE_OF_AXIS_MAX) &&
                        (dist < DIST_THRESHOLD) && (dist < min_dist))
                    {
                        min_dist = dist;
                        m_SelectedAxis = i;
                    }
                }
            }
        }
        else if ((int)m_AxisMode == 1)  // rotation axis
        {
            const float DIST_THRESHOLD = 0.2f;

            float min_dist = 1e10f;
            for (int i = 0; i < 3; ++i)
            {
                const float dist = std::fabs(1 - std::hypot(intersect_pt[i].x, intersect_pt[i].y, intersect_pt[i].z));
                if ((dist < DIST_THRESHOLD) && (dist < min_dist))
                {
                    min_dist = dist;
                    m_SelectedAxis = i;
                }
            }
        }
        else
        {
            return m_SelectedAxis;
        }
    }

    GET_SYSTEM(RenderSystem)->SetSelectedAxis(m_SelectedAxis);

    return m_SelectedAxis;
}

RenderEntity* EditorSceneManager::GetAxisMeshByType(EditorAxisMode axis_mode)
{
    RenderEntity* axis_mesh = nullptr;
    switch (axis_mode)
    {
        case EditorAxisMode::TranslateMode:
            axis_mesh = &m_TranslationAxis;
            break;
        case EditorAxisMode::RotateMode:
            axis_mesh = &m_RotationAxis;
            break;
        case EditorAxisMode::ScaleMode:
            axis_mesh = &m_ScaleAixs;
            break;
        default:
            break;
    }
    return axis_mesh;
}

void EditorSceneManager::DrawSelectedEntityAxis()
{
    std::shared_ptr<GameObject> selected_object = GetSelectedGObject().lock();

    if (g_isEditorMode && selected_object != nullptr)
    {
        const Transform* transform_component = selected_object->tryGetComponentConst(Transform);

        Vector3 scale;
        Quaternion rotation;
        Vector3 translation;
        transform_component->GetLocalToWorldMatrix().Decomposition(translation, scale, rotation);
        Matrix4x4 translation_matrix = Matrix4x4::GetTrans(translation);
        Matrix4x4 scale_matrix = Matrix4x4::BuildScaleMatrix(1.0f, 1.0f, 1.0f);
        Matrix4x4 axis_model_matrix = translation_matrix * scale_matrix;
        RenderEntity* selected_aixs = GetAxisMeshByType(m_AxisMode);
        if (m_AxisMode == EditorAxisMode::TranslateMode || m_AxisMode == EditorAxisMode::RotateMode)
        {
            selected_aixs->m_ModelMatrix = axis_model_matrix;
        }
        else if (m_AxisMode == EditorAxisMode::ScaleMode)
        {
            selected_aixs->m_ModelMatrix = axis_model_matrix * Matrix4x4(rotation);
        }

        GET_SYSTEM(RenderSystem)->SetVisibleAxis(*selected_aixs);
    }
    else
    {
        GET_SYSTEM(RenderSystem)->SetVisibleAxis(std::nullopt);
    }
}

std::weak_ptr<GameObject> EditorSceneManager::GetSelectedGObject() const
{
    std::weak_ptr<GameObject> selected_object;
    if (m_SelectedGobjectId != k_invalid_gobject_id)
    {
        Level* level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (level != nullptr)
        {
            selected_object = level->GetGObjectByID(m_SelectedGobjectId);
        }
    }
    return selected_object;
}

bool EditorSceneManager::IsGObjectSelected(GObjectID object_id) const
{
    if (object_id == k_invalid_gobject_id)
    {
        return false;
    }

    return std::find(m_SelectedGobjectIds.begin(), m_SelectedGobjectIds.end(), object_id) !=
           m_SelectedGobjectIds.end();
}

GObjectID EditorSceneManager::PickGObjectAtViewportUv(Vector2 picked_uv) const
{
    const size_t picked_mesh_id = GetGuidOfPickedMesh(picked_uv);
    if (picked_mesh_id == 0)
    {
        return k_invalid_gobject_id;
    }

    return GET_SYSTEM(RenderSystem)->GetGObjectIDByMeshID(picked_mesh_id);
}

void EditorSceneManager::OnGObjectSelected(GObjectID selected_gobject_id, GObjectSelectionOp op)
{
    m_SelectedAssetPath.clear();
    m_SelectedAssetType.clear();

    if (selected_gobject_id == k_invalid_gobject_id)
    {
        m_SelectedGobjectIds.clear();
        m_SelectedGobjectId = k_invalid_gobject_id;
        m_RangeSelectionAnchorId = k_invalid_gobject_id;
        m_SelectedObjectMatrix = Matrix4x4::IDENTITY;
        DrawSelectedEntityAxis();
        LOG_INFO(ZEditor, "no game object selected");
        return;
    }

    if (op == GObjectSelectionOp::Toggle)
    {
        const auto existing =
            std::find(m_SelectedGobjectIds.begin(), m_SelectedGobjectIds.end(), selected_gobject_id);
        if (existing != m_SelectedGobjectIds.end())
        {
            m_SelectedGobjectIds.erase(existing);
        }
        else
        {
            m_SelectedGobjectIds.push_back(selected_gobject_id);
        }

        if (m_RangeSelectionAnchorId == k_invalid_gobject_id)
        {
            m_RangeSelectionAnchorId = selected_gobject_id;
        }
    }
    else
    {
        m_SelectedGobjectIds.clear();
        m_SelectedGobjectIds.push_back(selected_gobject_id);
        m_RangeSelectionAnchorId = selected_gobject_id;
    }

    if (m_SelectedGobjectIds.empty())
    {
        m_SelectedGobjectId = k_invalid_gobject_id;
        m_RangeSelectionAnchorId = k_invalid_gobject_id;
        m_SelectedObjectMatrix = Matrix4x4::IDENTITY;
        DrawSelectedEntityAxis();
        LOG_INFO(ZEditor, "no game object selected");
        return;
    }

    m_SelectedGobjectId = selected_gobject_id;
    m_SelectedObjectMatrix = Matrix4x4::IDENTITY;

    std::shared_ptr<GameObject> selected_gobject = GetSelectedGObject().lock();
    if (selected_gobject)
    {
        const Transform* transform_component = selected_gobject->tryGetComponentConst(Transform);
        if (transform_component != nullptr)
        {
            m_SelectedObjectMatrix = transform_component->GetLocalToWorldMatrix();
        }
    }

    DrawSelectedEntityAxis();
    LOG_INFO(ZEditor,
             "select game object {} ({} selected)",
             m_SelectedGobjectId,
             m_SelectedGobjectIds.size());
}

void EditorSceneManager::OnGObjectRangeSelected(GObjectID end_object_id,
                                                const std::vector<GObjectID>& visible_order)
{
    if (end_object_id == k_invalid_gobject_id)
    {
        OnGObjectSelected(k_invalid_gobject_id, GObjectSelectionOp::Replace);
        return;
    }

    if (m_RangeSelectionAnchorId == k_invalid_gobject_id || visible_order.empty())
    {
        OnGObjectSelected(end_object_id, GObjectSelectionOp::Replace);
        return;
    }

    const auto anchor_it =
        std::find(visible_order.begin(), visible_order.end(), m_RangeSelectionAnchorId);
    const auto end_it = std::find(visible_order.begin(), visible_order.end(), end_object_id);
    if (anchor_it == visible_order.end() || end_it == visible_order.end())
    {
        OnGObjectSelected(end_object_id, GObjectSelectionOp::Replace);
        return;
    }

    m_SelectedAssetPath.clear();
    m_SelectedAssetType.clear();

    const size_t anchor_index = static_cast<size_t>(anchor_it - visible_order.begin());
    const size_t end_index = static_cast<size_t>(end_it - visible_order.begin());
    const size_t begin_index = std::min(anchor_index, end_index);
    const size_t past_end_index = std::max(anchor_index, end_index) + 1;

    m_SelectedGobjectIds.assign(visible_order.begin() + begin_index, visible_order.begin() + past_end_index);
    m_SelectedGobjectId = end_object_id;
    m_SelectedObjectMatrix = Matrix4x4::IDENTITY;

    std::shared_ptr<GameObject> selected_gobject = GetSelectedGObject().lock();
    if (selected_gobject)
    {
        const Transform* transform_component = selected_gobject->tryGetComponentConst(Transform);
        if (transform_component != nullptr)
        {
            m_SelectedObjectMatrix = transform_component->GetLocalToWorldMatrix();
        }
    }

    DrawSelectedEntityAxis();
    LOG_INFO(ZEditor,
             "range select anchor={} end={} ({} selected)",
             m_RangeSelectionAnchorId,
             end_object_id,
             m_SelectedGobjectIds.size());
}

void EditorSceneManager::OnAssetSelected(const std::filesystem::path& asset_path, const std::string& asset_type)
{
    m_SelectedAssetPath = asset_path;
    m_SelectedAssetType = asset_type;
    m_SelectedGobjectIds.clear();
    m_SelectedGobjectId = k_invalid_gobject_id;
    m_SelectedObjectMatrix = Matrix4x4::IDENTITY;
    GET_SYSTEM(RenderSystem)->SetVisibleAxis(std::nullopt);
}

void EditorSceneManager::OnDeleteSelectedGObject()
{
    Level* current_active_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    if (current_active_level == nullptr || m_SelectedGobjectIds.empty())
    {
        return;
    }

    std::vector<GObjectID> ids_to_delete = m_SelectedGobjectIds;
    RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();

    for (const GObjectID object_id : ids_to_delete)
    {
        std::shared_ptr<GameObject> selected_object = current_active_level->GetGObjectByID(object_id).lock();
        if (selected_object == nullptr)
        {
            continue;
        }

        current_active_level->DeleteGObjectByID(object_id);
        swap_context.GetLogicSwapData().AddDeleteGameObject(GameObjectDesc {selected_object->GetID(), {}});
    }

    GET_SYSTEM(WorldManager)->MarkCurrentLevelDirty();
    OnGObjectSelected(k_invalid_gobject_id, GObjectSelectionOp::Replace);
}

void EditorSceneManager::SetSceneView2D(bool enabled)
{
    if (m_SceneView2D == enabled || m_Camera == nullptr)
    {
        return;
    }

    m_SceneView2D = enabled;

    if (enabled)
    {
        m_SceneView3DSavedEye = m_Camera->position();
        m_SceneView3DSavedForward = m_Camera->forward();
        m_SceneView3DSavedUp = m_Camera->up();
        m_HasSceneView3DSavedPose = true;

        const Vector3 pos = m_Camera->position();
        const float distance = std::max(std::abs(pos.z), 5.0f);
        const float fovy_rad = Math::DegreesToRadians(std::max(m_Camera->getFOV().y, 1.0f));
        const float ortho_half = std::max(distance * std::tan(fovy_rad * 0.5f), 0.5f);

        m_Camera->SetOrthoHalfHeight(ortho_half);
        m_Camera->SetOrthographic(true);

        const Vector3 pivot(pos.x, pos.y, 0.0f);
        m_Camera->LookAt(Vector3(pivot.x, pivot.y, distance), pivot, Vector3(0.0f, 1.0f, 0.0f));

        if (m_AxisMode == EditorAxisMode::RotateMode)
        {
            m_AxisMode = EditorAxisMode::TranslateMode;
            DrawSelectedEntityAxis();
        }
    }
    else
    {
        m_Camera->SetOrthographic(false);
        if (m_HasSceneView3DSavedPose)
        {
            const Vector3 target = m_SceneView3DSavedEye + m_SceneView3DSavedForward;
            m_Camera->LookAt(m_SceneView3DSavedEye, target, m_SceneView3DSavedUp);
        }
        else
        {
            ApplyDefaultSceneView3DPose(m_Camera);
        }
    }
}

void EditorSceneManager::FocusSelectedGObject()
{
    if (m_Camera == nullptr)
    {
        return;
    }

    std::shared_ptr<GameObject> selected_object = GetSelectedGObject().lock();
    if (selected_object == nullptr)
    {
        return;
    }

    const Transform* transform_component = selected_object->tryGetComponentConst(Transform);
    if (transform_component == nullptr)
    {
        return;
    }

    Vector3 target_position = transform_component->GetLocalPosition();
    float focus_radius = 0.0f;
    if (TryGetSelectedMeshWorldBounds(*selected_object, target_position, focus_radius))
    {
        // Mesh-centered framing (Unity Frame Selected style).
    }

    Vector3 camera_forward = m_Camera->forward();
    if (camera_forward.length() < 0.001f)
    {
        camera_forward = Vector3(0.0f, 1.0f, 0.0f);
    }
    camera_forward.normalise();

    Vector3 camera_up = m_Camera->up();
    if (camera_up.length() < 0.001f)
    {
        camera_up = Vector3(0.0f, 0.0f, 1.0f);
    }
    camera_up.normalise();

    if (m_SceneView2D)
    {
        const float frame_half = std::max(focus_radius * 1.25f, 2.0f);
        m_Camera->SetOrthoHalfHeight(frame_half);
        const float distance = std::max(std::abs(m_Camera->position().z), 5.0f);
        m_Camera->LookAt(Vector3(target_position.x, target_position.y, distance),
                         Vector3(target_position.x, target_position.y, 0.0f),
                         Vector3(0.0f, 1.0f, 0.0f));
        return;
    }

    float focus_distance = (m_Camera->position() - target_position).length();
    if (focus_radius > 0.0f)
    {
        focus_distance = Math::max(focus_radius * 2.5f, 3.0f);
    }
    else
    {
        focus_distance = Math::max(focus_distance, 3.0f);
    }

    const Vector3 camera_position = target_position - camera_forward * focus_distance;
    m_Camera->LookAt(camera_position, target_position, camera_up);
}

void EditorSceneManager::MoveEntity(float new_mouse_pos_x,

                                    float new_mouse_pos_y,
                                    float last_mouse_pos_x,
                                    float last_mouse_pos_y,
                                    Vector2 engine_window_pos,
                                    Vector2 engine_window_size,
                                    size_t cursor_on_axis,
                                    Matrix4x4 model_matrix)
{
    std::shared_ptr<GameObject> selected_object = GetSelectedGObject().lock();
    if (selected_object == nullptr)
        return;

    float angularVelocity =
        18.0f / Math::max(engine_window_size.x, engine_window_size.y);  // 18 degrees while moving full screen
    Vector2 delta_mouse_move_uv = {(new_mouse_pos_x - last_mouse_pos_x), (new_mouse_pos_y - last_mouse_pos_y)};

    Vector3 model_scale;
    Quaternion model_rotation;
    Vector3 model_translation;
    model_matrix.Decomposition(model_translation, model_scale, model_rotation);

    Matrix4x4 axis_model_matrix = Matrix4x4::IDENTITY;
    axis_model_matrix.setTrans(model_translation);

    Matrix4x4 view_matrix = m_Camera->getLookAtMatrix();
    Matrix4x4 proj_matrix = m_Camera->GetProjectionMatrix();

    Vector4 model_world_position_4(model_translation, 1.f);

    Vector4 model_origin_clip_position = proj_matrix * view_matrix * model_world_position_4;
    model_origin_clip_position /= model_origin_clip_position.w;
    Vector2 model_origin_clip_uv =
        Vector2((model_origin_clip_position.x + 1) / 2.0f, (model_origin_clip_position.y + 1) / 2.0f);

    Vector4 axis_x_local_position_4(1, 0, 0, 1);
    if (m_AxisMode == EditorAxisMode::ScaleMode)
    {
        axis_x_local_position_4 = Matrix4x4(model_rotation) * axis_x_local_position_4;
    }
    Vector4 axis_x_world_position_4 = axis_model_matrix * axis_x_local_position_4;
    axis_x_world_position_4.w = 1.0f;
    Vector4 axis_x_clip_position = proj_matrix * view_matrix * axis_x_world_position_4;
    axis_x_clip_position /= axis_x_clip_position.w;
    Vector2 axis_x_clip_uv((axis_x_clip_position.x + 1) / 2.0f, (axis_x_clip_position.y + 1) / 2.0f);
    Vector2 axis_x_direction_uv = axis_x_clip_uv - model_origin_clip_uv;
    axis_x_direction_uv.normalise();

    Vector4 axis_y_local_position_4(0, 1, 0, 1);
    if (m_AxisMode == EditorAxisMode::ScaleMode)
    {
        axis_y_local_position_4 = Matrix4x4(model_rotation) * axis_y_local_position_4;
    }
    Vector4 axis_y_world_position_4 = axis_model_matrix * axis_y_local_position_4;
    axis_y_world_position_4.w = 1.0f;
    Vector4 axis_y_clip_position = proj_matrix * view_matrix * axis_y_world_position_4;
    axis_y_clip_position /= axis_y_clip_position.w;
    Vector2 axis_y_clip_uv((axis_y_clip_position.x + 1) / 2.0f, (axis_y_clip_position.y + 1) / 2.0f);
    Vector2 axis_y_direction_uv = axis_y_clip_uv - model_origin_clip_uv;
    axis_y_direction_uv.normalise();

    Vector4 axis_z_local_position_4(0, 0, 1, 1);
    if (m_AxisMode == EditorAxisMode::ScaleMode)
    {
        axis_z_local_position_4 = Matrix4x4(model_rotation) * axis_z_local_position_4;
    }
    Vector4 axis_z_world_position_4 = axis_model_matrix * axis_z_local_position_4;
    axis_z_world_position_4.w = 1.0f;
    Vector4 axis_z_clip_position = proj_matrix * view_matrix * axis_z_world_position_4;
    axis_z_clip_position /= axis_z_clip_position.w;
    Vector2 axis_z_clip_uv((axis_z_clip_position.x + 1) / 2.0f, (axis_z_clip_position.y + 1) / 2.0f);
    Vector2 axis_z_direction_uv = axis_z_clip_uv - model_origin_clip_uv;
    axis_z_direction_uv.normalise();

    Transform* transform_component = selected_object->tryGetComponent(Transform);

    Matrix4x4 new_model_matrix(Matrix4x4::IDENTITY);
    if (m_AxisMode == EditorAxisMode::TranslateMode)  // translate
    {
        Vector3 move_vector = {0, 0, 0};
        if (cursor_on_axis == 0)
        {
            move_vector.x = delta_mouse_move_uv.dotProduct(axis_x_direction_uv) * angularVelocity;
        }
        else if (cursor_on_axis == 1)
        {
            move_vector.y = delta_mouse_move_uv.dotProduct(axis_y_direction_uv) * angularVelocity;
        }
        else if (cursor_on_axis == 2)
        {
            move_vector.z = delta_mouse_move_uv.dotProduct(axis_z_direction_uv) * angularVelocity;
        }
        else
        {
            return;
        }

        Matrix4x4 translate_mat;
        translate_mat.MakeTransform(move_vector, Vector3::UNIT_SCALE, Quaternion::IDENTITY);
        new_model_matrix = axis_model_matrix * translate_mat;

        new_model_matrix = new_model_matrix * Matrix4x4(model_rotation);
        new_model_matrix = new_model_matrix * Matrix4x4::BuildScaleMatrix(model_scale.x, model_scale.y, model_scale.z);

        Vector3 new_scale;
        Quaternion new_rotation;
        Vector3 new_translation;
        new_model_matrix.Decomposition(new_translation, new_scale, new_rotation);

        Matrix4x4 translation_matrix = Matrix4x4::GetTrans(new_translation);
        Matrix4x4 scale_matrix = Matrix4x4::BuildScaleMatrix(1.f, 1.f, 1.f);
        Matrix4x4 axis_model_matrix = translation_matrix * scale_matrix;

        m_TranslationAxis.m_ModelMatrix = axis_model_matrix;
        m_RotationAxis.m_ModelMatrix = axis_model_matrix;
        m_ScaleAixs.m_ModelMatrix = axis_model_matrix;

        GET_SYSTEM(RenderSystem)->SetVisibleAxis(m_TranslationAxis);

        transform_component->SetLocalPosition(new_translation);
        transform_component->SetLocalRotation(new_rotation);
        transform_component->SetLocalScale(new_scale);
    }
    else if (m_AxisMode == EditorAxisMode::RotateMode)  // rotate
    {
        float last_mouse_u = (last_mouse_pos_x - engine_window_pos.x) / engine_window_size.x;
        float last_mouse_v = (last_mouse_pos_y - engine_window_pos.y) / engine_window_size.y;
        Vector2 last_move_vector(last_mouse_u - model_origin_clip_uv.x, last_mouse_v - model_origin_clip_uv.y);
        float new_mouse_u = (new_mouse_pos_x - engine_window_pos.x) / engine_window_size.x;
        float new_mouse_v = (new_mouse_pos_y - engine_window_pos.y) / engine_window_size.y;
        Vector2 new_move_vector(new_mouse_u - model_origin_clip_uv.x, new_mouse_v - model_origin_clip_uv.y);
        Vector3 delta_mouse_uv_3(delta_mouse_move_uv.x, delta_mouse_move_uv.y, 0);
        float move_radian;
        Vector3 axis_of_rotation = {0, 0, 0};
        if (cursor_on_axis == 0)
        {
            move_radian = (delta_mouse_move_uv * angularVelocity).length();
            if (m_Camera->forward().dotProduct(Vector3::UNIT_X) < 0)
            {
                move_radian = -move_radian;
            }
            axis_of_rotation.x = 1;
        }
        else if (cursor_on_axis == 1)
        {
            move_radian = (delta_mouse_move_uv * angularVelocity).length();
            if (m_Camera->forward().dotProduct(Vector3::UNIT_Y) < 0)
            {
                move_radian = -move_radian;
            }
            axis_of_rotation.y = 1;
        }
        else if (cursor_on_axis == 2)
        {
            move_radian = (delta_mouse_move_uv * angularVelocity).length();
            if (m_Camera->forward().dotProduct(Vector3::UNIT_Z) < 0)
            {
                move_radian = -move_radian;
            }
            axis_of_rotation.z = 1;
        }
        else
        {
            return;
        }
        float move_direction = last_move_vector.x * new_move_vector.y - new_move_vector.x * last_move_vector.y;
        if (move_direction < 0)
        {
            move_radian = -move_radian;
        }

        Quaternion move_rot;
        move_rot.FromAngleAxis(Radian(move_radian), axis_of_rotation);
        new_model_matrix = axis_model_matrix * move_rot;
        new_model_matrix = new_model_matrix * Matrix4x4(model_rotation);
        new_model_matrix = new_model_matrix * Matrix4x4::BuildScaleMatrix(model_scale.x, model_scale.y, model_scale.z);
        Vector3 new_scale;
        Quaternion new_rotation;
        Vector3 new_translation;

        new_model_matrix.Decomposition(new_translation, new_scale, new_rotation);

        transform_component->SetLocalPosition(new_translation);
        transform_component->SetLocalRotation(new_rotation);
        transform_component->SetLocalScale(new_scale);
        m_ScaleAixs.m_ModelMatrix = new_model_matrix;
    }
    else if (m_AxisMode == EditorAxisMode::ScaleMode)  // scale
    {
        Vector3 delta_scale_vector = {0, 0, 0};
        Vector3 new_model_scale = {0, 0, 0};
        if (cursor_on_axis == 0)
        {
            delta_scale_vector.x = 0.01f;
            if (delta_mouse_move_uv.dotProduct(axis_x_direction_uv) < 0)
            {
                delta_scale_vector = -delta_scale_vector;
            }
        }
        else if (cursor_on_axis == 1)
        {
            delta_scale_vector.y = 0.01f;
            if (delta_mouse_move_uv.dotProduct(axis_y_direction_uv) < 0)
            {
                delta_scale_vector = -delta_scale_vector;
            }
        }
        else if (cursor_on_axis == 2)
        {
            delta_scale_vector.z = 0.01f;
            if (delta_mouse_move_uv.dotProduct(axis_z_direction_uv) < 0)
            {
                delta_scale_vector = -delta_scale_vector;
            }
        }
        else
        {
            return;
        }
        new_model_scale = model_scale + delta_scale_vector;
        axis_model_matrix = axis_model_matrix * Matrix4x4(model_rotation);
        Matrix4x4 scale_mat;
        scale_mat.MakeTransform(Vector3::ZERO, new_model_scale, Quaternion::IDENTITY);
        new_model_matrix = axis_model_matrix * scale_mat;
        Vector3 new_scale;
        Quaternion new_rotation;
        Vector3 new_translation;
        new_model_matrix.Decomposition(new_translation, new_scale, new_rotation);

        transform_component->SetLocalPosition(new_translation);
        transform_component->SetLocalRotation(new_rotation);
        transform_component->SetLocalScale(new_scale);
    }
    setSelectedObjectMatrix(new_model_matrix);
    GET_SYSTEM(WorldManager)->MarkCurrentLevelDirty();
}

void EditorSceneManager::UploadAxisResource()
{
    auto& instance_id_allocator = GET_SYSTEM(RenderSystem)->GetGOInstanceIdAllocator();
    auto& mesh_asset_id_allocator = GET_SYSTEM(RenderSystem)->GetMeshAssetIdAllocator();

    // assign some value that won't be used by other game objects
    {
        GameObjectPartId axis_instance_id = {0xFFAA, 0xFFAA};
        MeshSourceDesc mesh_source_desc = {"%%translation_axis%%"};

        m_TranslationAxis.m_InstanceId = instance_id_allocator.allocGuid(axis_instance_id);
        m_TranslationAxis.m_MeshAssetId = mesh_asset_id_allocator.allocGuid(mesh_source_desc);
    }

    {
        GameObjectPartId axis_instance_id = {0xFFBB, 0xFFBB};
        MeshSourceDesc mesh_source_desc = {"%%rotate_axis%%"};

        m_RotationAxis.m_InstanceId = instance_id_allocator.allocGuid(axis_instance_id);
        m_RotationAxis.m_MeshAssetId = mesh_asset_id_allocator.allocGuid(mesh_source_desc);
    }

    {
        GameObjectPartId axis_instance_id = {0xFFCC, 0xFFCC};
        MeshSourceDesc mesh_source_desc = {"%%scale_axis%%"};

        m_ScaleAixs.m_InstanceId = instance_id_allocator.allocGuid(axis_instance_id);
        m_ScaleAixs.m_MeshAssetId = mesh_asset_id_allocator.allocGuid(mesh_source_desc);
    }

    GET_SYSTEM(RenderSystem)
        ->CreateAxis({m_TranslationAxis, m_RotationAxis, m_ScaleAixs},
                     {m_TranslationAxis.m_MeshData, m_RotationAxis.m_MeshData, m_ScaleAixs.m_MeshData});
}

size_t EditorSceneManager::GetGuidOfPickedMesh(const Vector2& picked_uv) const
{
    // RenderSystem::GetGuidOfPickedMesh flushes in-flight frames and marshals
    // PickPass GPU readback onto the RHI thread when parallel rendering is on.
    return GET_SYSTEM(RenderSystem)->GetGuidOfPickedMesh(picked_uv);
}

namespace
{
    void AfterSceneOpened(EditorSceneManager& scene_manager);
}

std::string EditorSceneManager::GetActiveSceneDisplayName()
{
    const WorldManager* world = GET_SYSTEM(WorldManager);
    if (world == nullptr)
    {
        return "Untitled";
    }

    const Level* level = world->getCurrentActiveLevel();
    if (level == nullptr)
    {
        return "Untitled";
    }

    const eastl::string& url = level->getLevelResUrl();
    if (url.empty())
    {
        return "Untitled";
    }

    return std::filesystem::path(url.c_str()).stem().string();
}

void EditorSceneManager::RefreshMainWindowTitle()
{
    std::string title = GetActiveSceneDisplayName();
    if (GET_SYSTEM(WorldManager)->IsCurrentLevelDirty())
    {
        title += '*';
    }

    if (const ProjectInfo* project = GET_SYSTEM(ProjectInfo))
    {
        if (!project->name.empty())
        {
            title += " - ";
            title += project->name.c_str();
        }
    }
    title += " - ZEditor";

    if (title == m_LastMainWindowTitle)
    {
        return;
    }

    m_LastMainWindowTitle = title;
    GET_SYSTEM(WindowSystem)->SetTitle(title.c_str());
}

bool EditorSceneManager::TryLeaveCurrentScene()
{
    WorldManager* world = GET_SYSTEM(WorldManager);
    if (world == nullptr || !world->IsCurrentLevelDirty())
    {
        return true;
    }

    const SceneSavePromptResult result = EditorUtility::PromptUnsavedScene(GetActiveSceneDisplayName());
    if (result == SceneSavePromptResult::Cancel)
    {
        return false;
    }

    if (result == SceneSavePromptResult::Save)
    {
        Level* level = world->getCurrentActiveLevel();
        if (level == nullptr)
        {
            return false;
        }

        if (level->getLevelResUrl().empty())
        {
            SaveActiveSceneAsDialog();
        }
        else
        {
            world->SaveCurrentLevel();
        }

        if (world->IsCurrentLevelDirty())
        {
            return false;
        }
    }
    else if (Level* level = world->getCurrentActiveLevel())
    {
        level->ClearDirty();
    }

    return true;
}

bool EditorSceneManager::OpenSceneInternal(const eastl::string& level_url, bool skip_unsaved_prompt)
{
    if (level_url.empty())
    {
        return false;
    }

    if (!skip_unsaved_prompt && !TryLeaveCurrentScene())
    {
        return false;
    }

    WorldManager* world = GET_SYSTEM(WorldManager);
    if (world == nullptr)
    {
        return false;
    }

    if (!world->OpenScene(level_url))
    {
        return false;
    }

    AfterSceneOpened(*this);
    RecordLastOpenedScene(level_url);
    RefreshMainWindowTitle();
    return true;
}

namespace
{
    eastl::string PathToLevelUrl(const std::filesystem::path& path)
    {
        const std::filesystem::path normalized = ContentBrowserHelpers::NormalizeContentBrowserPath(path);
        if (const auto project_info = GET_SYSTEM(ProjectInfo))
        {
            std::error_code ec;
            const std::filesystem::path content_root =
                std::filesystem::absolute(project_info->GetProjectContent(), ec).lexically_normal();
            if (!ec && !content_root.empty())
            {
                const std::filesystem::path rel = std::filesystem::relative(normalized, content_root, ec);
                if (!ec && !rel.empty())
                {
                    const std::string rel_str = rel.generic_string();
                    if (rel_str.rfind("..", 0) != 0)
                    {
                        std::string url = rel_str;
                        std::replace(url.begin(), url.end(), '\\', '/');
                        return url.c_str();
                    }
                }
            }
        }
        return normalized.generic_string().c_str();
    }

    std::filesystem::path LevelUrlToAbsolutePath(const eastl::string& level_url)
    {
        if (level_url.empty())
        {
            return {};
        }

        const ProjectInfo* project = GET_SYSTEM(ProjectInfo);
        if (project == nullptr)
        {
            return {};
        }

        std::error_code ec;
        const std::filesystem::path content_root =
            std::filesystem::absolute(project->GetProjectContent(), ec).lexically_normal();
        if (ec || content_root.empty())
        {
            return {};
        }

        std::filesystem::path rel(level_url.c_str());
        rel.replace_extension(".scene");
        return (content_root / rel).lexically_normal();
    }

    std::filesystem::path DefaultSceneSaveDirectory()
    {
        const std::filesystem::path assets_root = ContentBrowserHelpers::GetEditorSourceAssetFolder();
        if (assets_root.empty())
        {
            return assets_root;
        }

        std::error_code ec;
        std::filesystem::create_directories(assets_root, ec);
        return assets_root;
    }

    std::string DefaultSceneSaveFileName()
    {
        if (const WorldManager* world = GET_SYSTEM(WorldManager))
        {
            if (const Level* active = world->getCurrentActiveLevel())
            {
                const std::filesystem::path current(active->getLevelResUrl().c_str());
                if (!current.empty())
                {
                    return current.stem().string() + ".scene";
                }
            }
        }
        return "NewScene.scene";
    }

    void AfterSceneOpened(EditorSceneManager& scene_manager)
    {
        GET_SYSTEM(RenderSystem)->ClearForLevelReloading();
        scene_manager.OnGObjectSelected(k_invalid_gobject_id);
    }
}  // namespace

void EditorSceneManager::TryRestoreLastOpenedScene()
{
    WorldManager* world = GET_SYSTEM(WorldManager);
    if (world == nullptr || !world->IsWorldLoaded())
    {
        return;
    }

    m_PendingLastSceneRestore = false;

    if (world->IsWorldPartitionEnabled())
    {
        return;
    }

    const std::string last_url =
        EditorProjectPrefs::GetString(EditorProjectPrefKeys::LastOpenedScene);
    if (last_url.empty())
    {
        return;
    }

    const eastl::string level_url = last_url.c_str();
    const std::filesystem::path scene_path = LevelUrlToAbsolutePath(level_url);
    if (scene_path.empty())
    {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(scene_path, ec))
    {
        LOG_WARNING(ZEditor, "Last scene missing on disk, skipping restore: {}", scene_path.generic_string());
        return;
    }

    if (const Level* active = world->getCurrentActiveLevel())
    {
        if (active->getLevelResUrl() == level_url)
        {
            return;
        }
    }

    if (!OpenSceneInternal(level_url, true))
    {
        LOG_WARNING(ZEditor, "Failed to restore last scene: {}", level_url.c_str());
        return;
    }

    LOG_INFO(ZEditor, "Restored last scene: {}", level_url.c_str());
}

void EditorSceneManager::RecordLastOpenedScene(const eastl::string& level_url)
{
    if (level_url.empty())
    {
        return;
    }

    EditorProjectPrefs::SetString(EditorProjectPrefKeys::LastOpenedScene, level_url.c_str());
}

void EditorSceneManager::SaveActiveSceneAsDialog()
{
    std::string selected_path;
    const std::filesystem::path default_directory = DefaultSceneSaveDirectory();
    const std::string default_file_name = DefaultSceneSaveFileName();
    if (!EditorUtility::SaveFileDialog("Save Scene As",
                                       default_directory.string(),
                                       default_file_name,
                                       selected_path,
                                       "scene",
                                       "*.scene"))
    {
        return;
    }

    std::filesystem::path save_path(selected_path);
    if (save_path.extension().empty())
    {
        save_path += ".scene";
    }
    else
    {
        std::string ext = save_path.extension().generic_string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".scene")
        {
            save_path.replace_extension(".scene");
        }
    }

    std::error_code ec;
    if (!save_path.parent_path().empty())
    {
        std::filesystem::create_directories(save_path.parent_path(), ec);
    }

    WorldManager* world = GET_SYSTEM(WorldManager);
    if (world == nullptr)
    {
        LOG_ERROR(ZEditor, "Save Scene As: WorldManager unavailable");
        return;
    }

    const eastl::string level_url = PathToLevelUrl(save_path);
    if (!world->SaveCurrentLevelAs(level_url))
    {
        LOG_ERROR(ZEditor, "Save Scene As failed: {}", save_path.generic_string());
        return;
    }

    RecordLastOpenedScene(level_url);
    RefreshMainWindowTitle();
    LOG_INFO(ZEditor, "Save Scene As: {}", save_path.generic_string());
}

bool EditorSceneManager::OpenSceneFromContentBrowserPath(const eastl::string& content_browser_file_path)
{
    if (content_browser_file_path.empty())
    {
        return false;
    }

    std::string extension = std::filesystem::path(content_browser_file_path.c_str()).extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (extension != ".scene")
    {
        return false;
    }

    const eastl::string level_url = PathToLevelUrl(content_browser_file_path.c_str());
    if (!OpenSceneInternal(level_url, false))
    {
        LOG_ERROR(ZEditor, "OpenScene failed: {}", content_browser_file_path.c_str());
        return false;
    }

    LOG_INFO(ZEditor, "OpenScene: {}", content_browser_file_path.c_str());
    return true;
}