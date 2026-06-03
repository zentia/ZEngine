#pragma once
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

struct CameraPose
{
    DECLARE_SERIALIZE(CameraPose)

public:
    Vector3 m_Position;
    Vector3 m_Target;
    Vector3 m_Up;
};

template<typename TransferFunction>
void CameraPose::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Position, "position");
    transfer.Transfer(m_Target, "target");
    transfer.Transfer(m_Up, "up");
}

struct CameraConfig
{
    DECLARE_SERIALIZE(CameraConfig)

public:
    CameraPose m_Pose;
    Vector2 m_Aspect;
    float m_ZFar;
    float m_ZNear;
};

template<typename TransferFunction>
void CameraConfig::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Pose, "pose");
    transfer.Transfer(m_Aspect, "aspect");
    transfer.Transfer(m_ZFar, "z_far");
    transfer.Transfer(m_ZNear, "z_near");
}