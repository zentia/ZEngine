#pragma once
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Function/Framework/Component/Component.h"

class Transform
{
public:
    DECLARE_SERIALIZE(Transform);

    Vector3 m_Position {Vector3::ZERO};
    Vector3 m_Scale {Vector3::UNIT_SCALE};
    Quaternion m_Rotation {Quaternion::IDENTITY};

    Transform() = default;
    Transform(const Vector3& position, const Quaternion& rotation, const Vector3& scale)
        : m_Position {position}, m_Scale {scale}, m_Rotation {rotation}
    {
    }

    Matrix4x4 getMatrix() const
    {
        Matrix4x4 temp;
        temp.MakeTransform(m_Position, m_Scale, m_Rotation);
        return temp;
    }
};

template<typename TransferFunction>
void Transform::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Position, "m_position");
    transfer.Transfer(m_Scale, "m_scale");
    transfer.Transfer(m_Rotation, "m_rotation");
}