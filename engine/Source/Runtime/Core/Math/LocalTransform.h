#pragma once
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

// Serializable local TRS (position / rotation / scale). Used by mesh sub-assets,
// rigid-body shapes, skeleton binding poses, and the Transform component's
// serialized "transform" field. Not the scene-graph Component (see Transform.h).
class LocalTransform
{
public:
    DECLARE_SERIALIZE(LocalTransform);

    Vector3 m_Position {Vector3::ZERO};
    Vector3 m_Scale {Vector3::UNIT_SCALE};
    Quaternion m_Rotation {Quaternion::IDENTITY};

    LocalTransform() = default;
    LocalTransform(const Vector3& position, const Quaternion& rotation, const Vector3& scale)
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
void LocalTransform::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Position, "m_position");
    transfer.Transfer(m_Scale, "m_scale");
    transfer.Transfer(m_Rotation, "m_rotation");
}
