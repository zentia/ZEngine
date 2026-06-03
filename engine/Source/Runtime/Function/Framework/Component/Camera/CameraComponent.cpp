#include "Runtime/Function/Framework/Component/Camera/CameraComponent.h"

#include "Application/Application.h"
#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/MathHeaders.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Character/Character.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Function/Framework/Level/Level.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Input/InputSystem.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderSwapContext.h"
#include "Runtime/Function/Render/RenderSystem.h"

namespace
{
    CameraParameter* createDefaultCameraParameter(CameraMode mode)
    {
        switch (mode)
        {
            case CameraMode::third_person:
                return MemoryManager::CreateObject<ThirdPersonCameraParameter>();
            case CameraMode::free:
                return MemoryManager::CreateObject<FreeCameraParameter>();
            case CameraMode::first_person:
            case CameraMode::invalid:
            default:
                return MemoryManager::CreateObject<FirstPersonCameraParameter>();
        }
    }
}  // namespace

IMPLEMENT_REGISTER_CLASS(CameraComponent)
IMPLEMENT_OBJECT_SERIALIZE(CameraComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(CameraComponent)

template<typename TransferFunction>
void CameraComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_CameraMode, "mode");
    transfer.Transfer(m_IsMainCamera, "main");
    transfer.Transfer(m_CameraRes, "settings");
}

void CameraComponent::Initialize(CameraMode mode, CameraParameter* parameter)
{
    m_CameraMode = mode;
    m_CameraRes.m_Parameter = PPtr<CameraParameter>(parameter);
}

CameraParameter* CameraComponent::GetCameraParameter() const
{
    return m_CameraRes.m_Parameter;
}

void CameraComponent::OnSerializedFieldsUpdated()
{
    if (m_CameraMode == CameraMode::invalid)
    {
        m_CameraMode = CameraMode::first_person;
    }

    CameraParameter* parameter = m_CameraRes.m_Parameter;
    bool should_reset_parameter = (parameter == nullptr);
    if (!should_reset_parameter)
    {
        switch (m_CameraMode)
        {
            case CameraMode::first_person:
                should_reset_parameter = dynamic_cast<FirstPersonCameraParameter*>(parameter) == nullptr;
                break;
            case CameraMode::third_person:
                should_reset_parameter = dynamic_cast<ThirdPersonCameraParameter*>(parameter) == nullptr;
                break;
            case CameraMode::free:
                should_reset_parameter = dynamic_cast<FreeCameraParameter*>(parameter) == nullptr;
                break;
            case CameraMode::invalid:
            default:
                should_reset_parameter = true;
                break;
        }
    }

    if (should_reset_parameter)
    {
        m_CameraRes.m_Parameter = PPtr<CameraParameter>(createDefaultCameraParameter(m_CameraMode));
        parameter = m_CameraRes.m_Parameter;
    }

    if (parameter != nullptr)
    {
        RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
        CameraSwapData camera_swap_data;
        camera_swap_data.m_FovX = parameter->m_Fov;
        swap_context.GetLogicSwapData().m_CameraSwapData = camera_swap_data;
    }

    if (m_IsMainCamera && m_ParentObject != nullptr)
    {
        Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (current_level != nullptr)
        {
            current_level->SetMainCamera(m_ParentObject->GetID());
        }
    }
}

void CameraComponent::PostLoadResource(GameObject* parent_object)

{
    m_ParentObject = parent_object;

    if (m_CameraMode == CameraMode::invalid)
    {
        m_CameraMode = CameraMode::first_person;
    }
    if (m_CameraRes.m_Parameter == nullptr)
    {
        m_CameraRes.m_Parameter = PPtr<CameraParameter>(createDefaultCameraParameter(m_CameraMode));
    }

    if (m_CameraRes.m_Parameter == nullptr)
    {
        LOG_ERROR(ZCamera, "camera parameter is null");
        return;
    }

    RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    CameraSwapData camera_swap_data;
    camera_swap_data.m_FovX = m_CameraRes.m_Parameter->m_Fov;
    swap_context.GetLogicSwapData().m_CameraSwapData = camera_swap_data;
}

void CameraComponent::Tick(float delta_time)
{
    if (!m_ParentObject)
        return;

    Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    std::shared_ptr<Character> current_character = current_level->getCurrentActiveCharacter().lock();
    if (current_character == nullptr)
        return;

    if (current_character->GetObjectID() != m_ParentObject->GetID())
        return;

    switch (m_CameraMode)
    {
        case CameraMode::first_person:
            TickFirstPersonCamera(delta_time);
            break;
        case CameraMode::third_person:
            TickThirdPersonCamera(delta_time);
            break;
        case CameraMode::free:
            TickFreeCamera(delta_time);
            break;
        default:
            break;
    }
}
bool CameraComponent::ApplyRenderTexture()
{
    return true;
}

void CameraComponent::ApplyToGameRenderCamera(RenderCamera& render_camera) const
{
    if (m_CameraRes.m_Parameter != nullptr)
    {
        render_camera.setFOVx(m_CameraRes.m_Parameter->m_Fov);
    }

    Vector3 position = m_Position;
    Vector3 forward = m_Forward;
    Vector3 up = m_Up;

    bool use_runtime_camera_pose = false;
    if (g_isPlaying && m_ParentObject != nullptr)
    {
        Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
        if (current_level != nullptr)
        {
            std::shared_ptr<Character> current_character = current_level->getCurrentActiveCharacter().lock();
            use_runtime_camera_pose = current_character != nullptr && current_character->GetObjectID() == m_ParentObject->GetID();
        }
    }

    if (!use_runtime_camera_pose && m_ParentObject != nullptr)
    {
        const Transform* transform_component = m_ParentObject->tryGetComponentConst(Transform);
        if (transform_component != nullptr)
        {
            const Quaternion rotation = transform_component->GetRotation();
            position = transform_component->GetPosition();
            forward = rotation * Vector3::NEGATIVE_UNIT_Y;
            up = rotation * Vector3::UNIT_Z;
        }
    }

    if (forward.isZeroLength())
    {
        forward = Vector3::NEGATIVE_UNIT_Y;
    }
    else
    {
        forward.normalise();
    }

    if (up.isZeroLength())
    {
        up = Vector3::UNIT_Z;
    }
    else
    {
        up.normalise();
    }

    const Matrix4x4 desired_mat = Math::MakeLookAtMatrix(position, position + forward, up);
    render_camera.LookAt(position, position + forward, up);
    render_camera.SetMainViewMatrix(desired_mat, RenderCameraType::Game);
}

void CameraComponent::TickFirstPersonCamera(float delta_time)
{
    Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    std::shared_ptr<Character> current_character = current_level->getCurrentActiveCharacter().lock();
    if (current_character == nullptr)
        return;

    Quaternion q_yaw, q_pitch;

    q_yaw.FromAngleAxis(GET_SYSTEM(InputSystem)->m_CursorDeltaYaw, Vector3::UNIT_Z);
    q_pitch.FromAngleAxis(GET_SYSTEM(InputSystem)->m_CursorDeltaPitch, m_Left);

    const float offset =
        static_cast<FirstPersonCameraParameter*>((CameraParameter*)m_CameraRes.m_Parameter)->m_VerticalOffset;
    m_Position = current_character->GetPosition() + offset * Vector3::UNIT_Z;

    m_Forward = q_yaw * q_pitch * m_Forward;
    m_Left = q_yaw * q_pitch * m_Left;
    m_Up = m_Forward.crossProduct(m_Left);

    Matrix4x4 desired_mat = Math::MakeLookAtMatrix(m_Position, m_Position + m_Forward, m_Up);

    RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    CameraSwapData camera_swap_data;
    camera_swap_data.m_CameraType = RenderCameraType::Game;
    camera_swap_data.m_ViewMatrix = desired_mat;
    swap_context.GetLogicSwapData().m_CameraSwapData = camera_swap_data;

    Vector3 object_facing = m_Forward - m_Forward.dotProduct(Vector3::UNIT_Z) * Vector3::UNIT_Z;
    Vector3 object_left = Vector3::UNIT_Z.crossProduct(object_facing);
    Quaternion object_rotation;
    object_rotation.FromAxes(object_left, -object_facing, Vector3::UNIT_Z);
    current_character->SetRotation(object_rotation);
}

void CameraComponent::TickThirdPersonCamera(float delta_time)
{
    Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    std::shared_ptr<Character> current_character = current_level->getCurrentActiveCharacter().lock();
    if (current_character == nullptr)
        return;

    ThirdPersonCameraParameter* param =
        static_cast<ThirdPersonCameraParameter*>((CameraParameter*)m_CameraRes.m_Parameter);

    Quaternion q_yaw, q_pitch;

    q_yaw.FromAngleAxis(GET_SYSTEM(InputSystem)->m_CursorDeltaYaw, Vector3::UNIT_Z);
    q_pitch.FromAngleAxis(GET_SYSTEM(InputSystem)->m_CursorDeltaPitch, Vector3::UNIT_X);

    param->m_CursorPitch = q_pitch * param->m_CursorPitch;

    const float vertical_offset = param->m_VerticalOffset;
    const float horizontal_offset = param->m_HorizontalOffset;
    Vector3 offset = Vector3(0, horizontal_offset, vertical_offset);

    Vector3 center_pos = current_character->GetPosition() + Vector3::UNIT_Z * vertical_offset;
    m_Position = current_character->getRotation() * param->m_CursorPitch * offset + current_character->GetPosition();

    m_Forward = center_pos - m_Position;
    m_Up = current_character->getRotation() * param->m_CursorPitch * Vector3::UNIT_Z;
    m_Left = m_Up.crossProduct(m_Forward);

    current_character->SetRotation(q_yaw * current_character->getRotation());

    Matrix4x4 desired_mat = Math::MakeLookAtMatrix(m_Position, m_Position + m_Forward, m_Up);

    RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    CameraSwapData camera_swap_data;
    camera_swap_data.m_CameraType = RenderCameraType::Game;
    camera_swap_data.m_ViewMatrix = desired_mat;
    swap_context.GetLogicSwapData().m_CameraSwapData = camera_swap_data;
}

void CameraComponent::TickFreeCamera(float delta_time)
{
    unsigned int command = GET_SYSTEM(InputSystem)->getGameCommand();
    if (command >= (unsigned int)GameCommand::invalid)
        return;

    Level* current_level = GET_SYSTEM(WorldManager)->getCurrentActiveLevel();
    std::shared_ptr<Character> current_character = current_level->getCurrentActiveCharacter().lock();
    if (current_character == nullptr)
        return;

    Quaternion q_yaw, q_pitch;

    q_yaw.FromAngleAxis(GET_SYSTEM(InputSystem)->m_CursorDeltaYaw, Vector3::UNIT_Z);
    q_pitch.FromAngleAxis(GET_SYSTEM(InputSystem)->m_CursorDeltaPitch, m_Left);

    m_Forward = q_yaw * q_pitch * m_Forward;
    m_Left = q_yaw * q_pitch * m_Left;
    m_Up = m_Forward.crossProduct(m_Left);

    bool has_move_command = ((unsigned int)GameCommand::forward | (unsigned int)GameCommand::backward |
                             (unsigned int)GameCommand::left | (unsigned int)GameCommand::right) &
                            command;
    if (has_move_command)
    {
        Vector3 move_direction = Vector3::ZERO;

        if ((unsigned int)GameCommand::forward & command)
        {
            move_direction += m_Forward;
        }

        if ((unsigned int)GameCommand::backward & command)
        {
            move_direction -= m_Forward;
        }

        if ((unsigned int)GameCommand::left & command)
        {
            move_direction += m_Left;
        }

        if ((unsigned int)GameCommand::right & command)
        {
            move_direction -= m_Left;
        }

        m_Position += move_direction * 2.0f * delta_time;
    }

    Matrix4x4 desired_mat = Math::MakeLookAtMatrix(m_Position, m_Position + m_Forward, m_Up);

    RenderSwapContext& swap_context = GET_SYSTEM(RenderSystem)->GetSwapContext();
    CameraSwapData camera_swap_data;
    camera_swap_data.m_CameraType = RenderCameraType::Game;
    camera_swap_data.m_ViewMatrix = desired_mat;
    swap_context.GetLogicSwapData().m_CameraSwapData = camera_swap_data;
}