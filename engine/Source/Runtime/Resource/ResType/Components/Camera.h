#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Camera.generated.h"

ZCLASS()
class CameraParameter : public Object
{
    ZGENERATED_BODY(CameraParameter)

public:
    ZPROPERTY(Name="m_fov")
    float m_Fov {50.f};

    virtual ~CameraParameter() {}
};

ZCLASS()
class FirstPersonCameraParameter : public CameraParameter
{
    ZGENERATED_BODY(FirstPersonCameraParameter)

public:
    ZPROPERTY(Name="m_vertical_offset")
    float m_VerticalOffset {0.6f};
};

ZCLASS()
class ThirdPersonCameraParameter : public CameraParameter
{
    ZGENERATED_BODY(ThirdPersonCameraParameter)

public:
    ZPROPERTY(Name="m_horizontal_offset")
    float m_HorizontalOffset {3.f};
    ZPROPERTY(Name="m_vertical_offset")
    float m_VerticalOffset {2.5f};
    ZPROPERTY(Name="m_cursor_pitch")
    Quaternion m_CursorPitch;
    ZPROPERTY(Name="m_cursor_yaw")
    Quaternion m_CursorYaw;
};

ZCLASS()
class FreeCameraParameter : public CameraParameter
{
    ZGENERATED_BODY(FreeCameraParameter)

public:
    ZPROPERTY(Name="m_speed")
    float m_Speed {1.f};
};

class CameraComponentRes
{
public:
    DECLARE_SERIALIZE(CameraComponentRes)

    PPtr<CameraParameter> m_Parameter;

    CameraComponentRes() = default;

    ~CameraComponentRes();
};

template<typename TransferFunction>
void CameraComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Parameter, "parameter");
}