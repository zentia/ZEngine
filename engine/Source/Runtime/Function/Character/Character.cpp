#include "Runtime/Function/Character/Character.h"

#include "Runtime/Application/Application.h"
#include "Runtime/Function/Framework/Component/Motor/MotorComponent.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Input/InputSystem.h"

Character::Character(std::shared_ptr<GameObject> character_object)
{
    SetObject(character_object);
}

GObjectID Character::GetObjectID() const
{
    if (m_CharacterObject)
    {
        return m_CharacterObject->GetID();
    }

    return k_invalid_gobject_id;
}

void Character::SetObject(std::shared_ptr<GameObject> gobject)
{
    m_CharacterObject = gobject;
    if (m_CharacterObject)
    {
        const Transform* transform_component = m_CharacterObject->tryGetComponentConst(Transform);
        if (transform_component)
        {
            const LocalTransform& transform = transform_component->GetLocalTransformConst();
            m_Position = transform.m_Position;
            m_Rotation = transform.m_Rotation;
        }
    }
    else
    {
        m_Position = Vector3::ZERO;
        m_Rotation = Quaternion::IDENTITY;
    }
}

void Character::Tick(float delta_time)
{
    if (m_CharacterObject == nullptr)
        return;

    unsigned int command = GET_SYSTEM(InputSystem)->getGameCommand();
    if (command < (unsigned int)GameCommand::invalid)
    {
        if ((((unsigned int)GameCommand::free_carema & command) > 0) != m_IsFreeCamera)
        {
            ToggleFreeCamera();
        }
    }

    Transform* transform_component = m_CharacterObject->tryGetComponent(Transform);

    if (m_RotationDirty)
    {
        transform_component->SetLocalRotation(m_RotationBuffer);
        m_RotationDirty = false;
    }

    const MotorComponent* motor_component = m_CharacterObject->tryGetComponentConst(MotorComponent);
    if (motor_component == nullptr)
    {
        return;
    }

    if (motor_component->getIsMoving())
    {
        m_RotationBuffer = m_Rotation;
        transform_component->SetLocalRotation(m_RotationBuffer);
        m_RotationDirty = true;
    }

    const Vector3& new_position = motor_component->getTargetPosition();

    m_Position = new_position;

    // float blend_ratio = std::max(1.f, motor_component->getSpeedRatio());

    // float frame_length = delta_time * blend_ratio;
    // m_Position =
    //     (m_Position * (s_CameraBlendTime - frame_length) + new_position * frame_length) / s_CameraBlendTime;
    // m_Position =
    //     (m_Position * (s_CameraBlendTime - frame_length) + new_position * frame_length) / s_CameraBlendTime;
}

void Character::ToggleFreeCamera()
{
    CameraComponent* camera_component = m_CharacterObject->tryGetComponent(CameraComponent);
    if (camera_component == nullptr)
        return;

    m_IsFreeCamera = !m_IsFreeCamera;

    if (m_IsFreeCamera)
    {
        m_OriginalCameraMode = camera_component->getCameraMode();
        camera_component->setCameraMode(CameraMode::free);
    }
    else
    {
        camera_component->setCameraMode(m_OriginalCameraMode);
    }
}