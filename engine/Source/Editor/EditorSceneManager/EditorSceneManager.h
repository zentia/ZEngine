#pragma once

#include "Editor/Axis/Axis.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Render/RenderObject.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class Editor;
class RenderCamera;
class RenderEntity;

enum class EditorAxisMode : int
{
    TranslateMode = 0,
    RotateMode = 1,
    ScaleMode = 2,
    Default = 3
};

enum class GObjectSelectionOp
{
    Replace,
    Toggle,
};

class EditorSceneManager : public IEngineSystem
{
public:
    // UE-style constant screen axis length (GizmoRenderingUtil / TransformGizmo indirect drag).
    static constexpr float kEditorGizmoAxisPixelLength = 100.0f;
    // EditorScaleAxis mesh extent along each local axis (see Axis.cpp, tip near 1.6).
    static constexpr float kEditorScaleGizmoMeshExtent = 1.6f;

    // Scale-mode handle length on one axis: follows object local scale * constant-view-size factor.
    static float ComputeScaleModeAxisLength(const RenderCamera& camera,
                                            const Vector3& pivot_world,
                                            float viewport_width,
                                            float viewport_height,
                                            float local_scale_component);

    static float ComputePixelToWorldScale(const RenderCamera& camera,
                                          const Vector3& world_location,
                                          float viewport_width,
                                          float viewport_height);
    static float ComputeGizmoAxisLengthWorld(const RenderCamera& camera,
                                             const Vector3& world_location,
                                             float viewport_width,
                                             float viewport_height);
    static Vector2 ComputeScreenAxisDirection(const RenderCamera& camera,
                                              const Vector3& pivot_world,
                                              const Vector3& world_axis_unit,
                                              float viewport_pos_x,
                                              float viewport_pos_y,
                                              float viewport_width,
                                              float viewport_height);

    std::string GetName() const override { return "EditorSceneManager"; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::PostInit; }
    std::vector<std::type_index> GetDependencies() const override;

    bool Initialize() override;
    void Shutdown() override {}
    void Tick(float delta_time);

    // Screen-space axis hit for translate/scale; ray cast for rotate. Returns 0..2 or 3 (none).
    size_t UpdateCursorOnAxis(Vector2 mouse_px, Vector2 viewport_pos, Vector2 viewport_size);
    size_t PickAxisAtViewportPixels(Vector2 mouse_px, Vector2 viewport_pos, Vector2 viewport_size) const;
    void DrawSelectedEntityAxis();
    std::weak_ptr<GameObject> GetSelectedGObject() const;
    const std::filesystem::path& getSelectedAssetPath() const { return m_SelectedAssetPath; }
    const std::string& getSelectedAssetType() const { return m_SelectedAssetType; }
    RenderEntity* GetAxisMeshByType(EditorAxisMode axis_mode);
    void OnGObjectSelected(GObjectID selected_gobject_id,
                           GObjectSelectionOp op = GObjectSelectionOp::Replace);
    // Shift+click range in Hierarchy visible tree order (`visible_order` from roots DFS).
    void OnGObjectRangeSelected(GObjectID end_object_id, const std::vector<GObjectID>& visible_order);
    bool IsGObjectSelected(GObjectID object_id) const;
    size_t GetSelectedObjectCount() const { return m_SelectedGobjectIds.size(); }
    const std::vector<GObjectID>& GetSelectedObjectIDs() const { return m_SelectedGobjectIds; }
    // Viewport UV (0..1). Returns k_invalid_gobject_id on miss.
    GObjectID PickGObjectAtViewportUv(Vector2 picked_uv) const;
    void OnAssetSelected(const std::filesystem::path& asset_path, const std::string& asset_type);
    void OnDeleteSelectedGObject();

    void FocusSelectedGObject();

    // Unity-style scene file ops (.scene under Assets/).
    void SaveActiveSceneAsDialog();
    bool OpenSceneFromContentBrowserPath(const eastl::string& content_browser_file_path);

    static std::string GetActiveSceneDisplayName();
    void RefreshMainWindowTitle();
    bool TryLeaveCurrentScene();
    void MoveEntity(float new_mouse_pos_x,
                    float new_mouse_pos_y,
                    float last_mouse_pos_x,
                    float last_mouse_pos_y,
                    Vector2 engine_window_pos,
                    Vector2 engine_window_size,
                    size_t cursor_on_axis,
                    Matrix4x4 model_matrix);

    void setEditorCamera(std::shared_ptr<RenderCamera> camera) { m_Camera = camera; }
    void UploadAxisResource();
    size_t GetGuidOfPickedMesh(const Vector2& picked_uv) const;

public:
    std::shared_ptr<RenderCamera> getEditorCamera() { return m_Camera; };

    GObjectID getSelectedObjectID() { return m_SelectedGobjectId; };
    Matrix4x4 getSelectedObjectMatrix() { return m_SelectedObjectMatrix; }
    EditorAxisMode getEditorAxisMode() { return m_AxisMode; }

    void setSelectedObjectID(GObjectID selected_gobject_id) { m_SelectedGobjectId = selected_gobject_id; };
    void setSelectedObjectMatrix(Matrix4x4 new_object_matrix) { m_SelectedObjectMatrix = new_object_matrix; }
    void setEditorAxisMode(EditorAxisMode new_axis_mode);

    bool IsSceneView2D() const { return m_SceneView2D; }
    void SetSceneView2D(bool enabled);

private:
    EditorTranslationAxis m_TranslationAxis;
    EditorRotationAxis m_RotationAxis;
    EditorScaleAxis m_ScaleAixs;

    GObjectID m_SelectedGobjectId {k_invalid_gobject_id};
    std::vector<GObjectID> m_SelectedGobjectIds;
    GObjectID m_RangeSelectionAnchorId {k_invalid_gobject_id};
    std::filesystem::path m_SelectedAssetPath;
    std::string m_SelectedAssetType;
    Matrix4x4 m_SelectedObjectMatrix {Matrix4x4::IDENTITY};

    EditorAxisMode m_AxisMode {EditorAxisMode::TranslateMode};
    bool m_SceneView2D {false};
    bool m_HasSceneView3DSavedPose {false};
    Vector3 m_SceneView3DSavedEye {0.0f, -10.0f, 5.0f};
    Vector3 m_SceneView3DSavedForward {0.0f, 1.0f, 0.0f};
    Vector3 m_SceneView3DSavedUp {0.0f, 0.0f, 1.0f};
    std::shared_ptr<RenderCamera> m_Camera;

    size_t m_SelectedAxis {3};

    bool m_IsShowAxis = true;

    bool m_PendingLastSceneRestore {true};
    std::string m_LastMainWindowTitle;

    void TryRestoreLastOpenedScene();
    void RecordLastOpenedScene(const eastl::string& level_url);
    bool OpenSceneInternal(const eastl::string& level_url, bool skip_unsaved_prompt);
};
