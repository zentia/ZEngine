#include "LevelDebugger.h"

#include "Application/Application.h"
#include "Function/Framework/Component/Animation/AnimationComponent.h"
#include "Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Core/Math/LocalTransform.h"
#include "Runtime/Function/Render/DebugDraw/DebugDrawManager.h"
#include "Runtime/Function/Render/RenderDebugConfig.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Components/Animation.h"
void LevelDebugger::Tick(Level* level) const
{
    if (g_isEditorMode)
    {
        return;
    }

    if (GET_SYSTEM(RenderDebugConfig)->animation.show_bone_name)
    {
        ShowAllBonesName(level);
    }
    if (GET_SYSTEM(RenderDebugConfig)->animation.show_skeleton)
    {
        ShowAllBones(level);
    }
    if (GET_SYSTEM(RenderDebugConfig)->game_object.show_bounding_box)
    {
        ShowAllBoundingBox(level);
    }
    if (GET_SYSTEM(RenderDebugConfig)->camera.show_runtime_info)
    {
        ShowCameraInfo(level);
    }
}

void LevelDebugger::ShowAllBones(Level* level) const
{
    const LevelObjectsMap& go_map = level->getAllGObjects();
    for (const auto& gobject_pair : go_map)
    {
        DrawBones(gobject_pair.second);
    }
}

void LevelDebugger::ShowBones(Level* level, GObjectID go_id) const
{
    std::shared_ptr<GameObject> gobject = level->GetGObjectByID(go_id).lock();
    DrawBones(gobject);
}

void LevelDebugger::ShowAllBonesName(Level* level) const
{
    const LevelObjectsMap& go_map = level->getAllGObjects();
    for (const auto& gobject_pair : go_map)
    {
        DrawBonesName(gobject_pair.second);
    }
}

void LevelDebugger::ShowBonesName(Level* level, GObjectID go_id) const
{
    std::shared_ptr<GameObject> gobject = level->GetGObjectByID(go_id).lock();
    DrawBonesName(gobject);
}

void LevelDebugger::ShowAllBoundingBox(Level* level) const
{
    const LevelObjectsMap& go_map = level->getAllGObjects();
    for (const auto& gobject_pair : go_map)
    {
        DrawBoundingBox(gobject_pair.second);
    }
}

void LevelDebugger::ShowBoundingBox(Level* level, GObjectID go_id) const
{
    std::shared_ptr<GameObject> gobject = level->GetGObjectByID(go_id).lock();
    DrawBoundingBox(gobject);
}

void LevelDebugger::ShowCameraInfo(Level* level) const
{
    std::shared_ptr<GameObject> gobject = level->getCurrentActiveCharacter().lock()->getObject().lock();
    DrawCameraInfo(gobject);
}
void LevelDebugger::DrawBones(std::shared_ptr<GameObject> object) const
{
    const Transform* transform_component =
        object->tryGetComponentConst<Transform>("Transform");
    const AnimationComponent* animation_component =
        object->tryGetComponentConst<AnimationComponent>("AnimationComponent");

    if (transform_component == nullptr || animation_component == nullptr)
        return;

    DebugDrawGroup* debug_draw_group = GET_SYSTEM(DebugDrawManager)->TryGetOrCreateDebugDrawGroup("bone");

    Matrix4x4 object_matrix = transform_component->GetLocalToWorldMatrix();

    const Skeleton& skeleton = animation_component->GetSkeleton();
    const Bone* bones = skeleton.GetBones();
    int32_t bones_count = skeleton.GetBonesCount();
    for (int32_t bone_index = 0; bone_index < bones_count; bone_index++)
    {
        if (bones[bone_index].GetParent() == nullptr || bone_index == 1)
            continue;

        Matrix4x4 bone_matrix = LocalTransform(bones[bone_index]._getDerivedPosition(),
                                          bones[bone_index]._getDerivedOrientation(),
                                          bones[bone_index]._getDerivedScale())
                                    .getMatrix();
        Vector4 bone_position(0.0f, 0.0f, 0.0f, 1.0f);
        bone_position = object_matrix * bone_matrix * bone_position;
        bone_position /= bone_position[3];

        Node* parent_bone = bones[bone_index].GetParent();
        Matrix4x4 parent_bone_matrix = LocalTransform(parent_bone->_getDerivedPosition(),
                                                 parent_bone->_getDerivedOrientation(),
                                                 parent_bone->_getDerivedScale())
                                           .getMatrix();
        Vector4 parent_bone_position(0.0f, 0.0f, 0.0f, 1.0f);
        parent_bone_position = object_matrix * parent_bone_matrix * parent_bone_position;
        parent_bone_position /= parent_bone_position[3];

        debug_draw_group->AddLine(Vector3(bone_position.x, bone_position.y, bone_position.z),
                                  Vector3(parent_bone_position.x, parent_bone_position.y, parent_bone_position.z),
                                  Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                                  Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                                  0.0f,
                                  true);
        debug_draw_group->AddSphere(Vector3(bone_position.x, bone_position.y, bone_position.z),
                                    0.015f,
                                    Vector4(0.0f, 0.0f, 1.0f, 1.0f),
                                    0.0f,
                                    true);
    }
}

void LevelDebugger::DrawBonesName(std::shared_ptr<GameObject> object) const
{
    const Transform* transform_component =
        object->tryGetComponentConst<Transform>("Transform");
    const AnimationComponent* animation_component =
        object->tryGetComponentConst<AnimationComponent>("AnimationComponent");

    if (transform_component == nullptr || animation_component == nullptr)
        return;

    DebugDrawGroup* debug_draw_group = GET_SYSTEM(DebugDrawManager)->TryGetOrCreateDebugDrawGroup("bone name");

    Matrix4x4 object_matrix = transform_component->GetLocalToWorldMatrix();

    const Skeleton& skeleton = animation_component->GetSkeleton();
    const Bone* bones = skeleton.GetBones();
    int32_t bones_count = skeleton.GetBonesCount();
    for (int32_t bone_index = 0; bone_index < bones_count; bone_index++)
    {
        if (bones[bone_index].GetParent() == nullptr || bone_index == 1)
            continue;

        Matrix4x4 bone_matrix = LocalTransform(bones[bone_index]._getDerivedPosition(),
                                          bones[bone_index]._getDerivedOrientation(),
                                          bones[bone_index]._getDerivedScale())
                                    .getMatrix();
        Vector4 bone_position(0.0f, 0.0f, 0.0f, 1.0f);
        bone_position = object_matrix * bone_matrix * bone_position;
        bone_position /= bone_position[3];

        debug_draw_group->AddText(bones[bone_index].GetName(),
                                  Vector4(1.0f, 0.0f, 0.0f, 1.0f),
                                  Vector3(bone_position.x, bone_position.y, bone_position.z),
                                  8,
                                  false);
    }
}

void LevelDebugger::DrawBoundingBox(std::shared_ptr<GameObject> object) const
{
    const RigidBodyComponent* rigidbody_component =
        object->tryGetComponentConst<RigidBodyComponent>("RigidBodyComponent");
    if (rigidbody_component == nullptr)
        return;

    std::vector<AxisAlignedBox> bounding_boxes;
    rigidbody_component->GetShapeBoundingBoxes(bounding_boxes);
    for (size_t bounding_box_index = 0; bounding_box_index < bounding_boxes.size(); bounding_box_index++)
    {
        AxisAlignedBox bounding_box = bounding_boxes[bounding_box_index];
        DebugDrawGroup* debug_draw_group = GET_SYSTEM(DebugDrawManager)->TryGetOrCreateDebugDrawGroup("bounding box");
        Vector3 center = Vector3(bounding_box.getCenter().x, bounding_box.getCenter().y, bounding_box.getCenter().z);
        Vector3 halfExtent =
            Vector3(bounding_box.getHalfExtent().x, bounding_box.getHalfExtent().y, bounding_box.getHalfExtent().z);

        debug_draw_group->AddBox(center, halfExtent, Vector4(1.0f, 0.0f, 0.0f, 0.0f), Vector4(0.0f, 1.0f, 0.0f, 1.0f));
    }
}

void LevelDebugger::DrawCameraInfo(std::shared_ptr<GameObject> object) const
{
    const CameraComponent* camera_component = object->tryGetComponentConst<CameraComponent>("CameraComponent");
    if (camera_component == nullptr)
        return;

    DebugDrawGroup* debug_draw_group = GET_SYSTEM(DebugDrawManager)->TryGetOrCreateDebugDrawGroup("show camera info");

    std::ostringstream buffer;
    buffer << "camera mode: ";
    switch (camera_component->getCameraMode())
    {
        case CameraMode::first_person:
            buffer << "first person";
            break;
        case CameraMode::third_person:
            buffer << "third person";
            break;
        case CameraMode::free:
            buffer << "free";
            break;
        case CameraMode::invalid:
            buffer << "invalid";
            break;
    }
    buffer << std::endl;

    Vector3 position = camera_component->GetPosition();
    Vector3 forward = camera_component->getForward();
    Vector3 direction = forward - position;
    buffer << "camera position: (" << position.x << "," << position.y << "," << position.z << ")" << std::endl;
    buffer << "camera direction : (" << direction.x << "," << direction.y << "," << direction.z << ")";
    debug_draw_group->AddText(
        buffer.str().c_str(), Vector4(1.0f, 0.0f, 0.0f, 1.0f), Vector3(-1.0f, -0.2f, 0.0f), 10, true);
}