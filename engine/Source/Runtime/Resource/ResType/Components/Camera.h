#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

class CameraParameter : public Object
{
    REGISTER_CLASS(CameraParameter)
    DECLARE_OBJECT_SERIALIZE();

public:
    float m_Fov {50.f};

    virtual ~CameraParameter() {}
};

class FirstPersonCameraParameter : public CameraParameter
{
    REGISTER_CLASS(FirstPersonCameraParameter)
    DECLARE_OBJECT_SERIALIZE();

public:
    float m_VerticalOffset {0.6f};
};

class ThirdPersonCameraParameter : public CameraParameter
{
    REGISTER_CLASS(ThirdPersonCameraParameter)
    DECLARE_OBJECT_SERIALIZE();

public:
    float m_HorizontalOffset {3.f};
    float m_VerticalOffset {2.5f};
    Quaternion m_CursorPitch;
    Quaternion m_CursorYaw;
};

class FreeCameraParameter : public CameraParameter
{
    REGISTER_CLASS(FreeCameraParameter)
    DECLARE_OBJECT_SERIALIZE();

public:
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