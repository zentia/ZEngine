#include "Runtime/Resource/ResType/Components/Camera.h"

#include "Runtime/Core/Base/Macro.h"

IMPLEMENT_REGISTER_CLASS(CameraParameter)
IMPLEMENT_OBJECT_SERIALIZE(CameraParameter)
template<typename TransferFunction>
void CameraParameter::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_Fov, "m_fov");
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(CameraParameter)

IMPLEMENT_REGISTER_CLASS(FirstPersonCameraParameter)
IMPLEMENT_OBJECT_SERIALIZE(FirstPersonCameraParameter)
template<typename TransferFunction>
void FirstPersonCameraParameter::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_VerticalOffset, "m_vertical_offset");
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(FirstPersonCameraParameter)

IMPLEMENT_REGISTER_CLASS(ThirdPersonCameraParameter)
IMPLEMENT_OBJECT_SERIALIZE(ThirdPersonCameraParameter)
template<typename TransferFunction>
void ThirdPersonCameraParameter::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_HorizontalOffset, "m_horizontal_offset");
    transfer.Transfer(m_VerticalOffset, "m_vertical_offset");
    transfer.Transfer(m_CursorPitch, "m_cursor_pitch");
    transfer.Transfer(m_CursorYaw, "m_cursor_yaw");
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(ThirdPersonCameraParameter)

IMPLEMENT_REGISTER_CLASS(FreeCameraParameter)
IMPLEMENT_OBJECT_SERIALIZE(FreeCameraParameter)
template<typename TransferFunction>
void FreeCameraParameter::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_Speed, "m_speed");
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(FreeCameraParameter)

CameraComponentRes::~CameraComponentRes() {}
