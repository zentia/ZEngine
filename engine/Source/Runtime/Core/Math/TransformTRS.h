#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector3d.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

/// Serializable translation / rotation / scale (mesh sub-assets, physics shapes, transform hierarchy).
class TransformTRS
{
public:
    DECLARE_SERIALIZE(TransformTRS);

    Vector3d m_Position {Vector3d::ZERO};
    Vector3 m_Scale {Vector3::UNIT_SCALE};
    Quaternion m_Rotation {Quaternion::IDENTITY};

    TransformTRS() = default;
    TransformTRS(const Vector3d& position, const Quaternion& rotation, const Vector3& scale)
        : m_Position {position}
        , m_Scale {scale}
        , m_Rotation {rotation}
    {
    }

    TransformTRS(const Vector3& position, const Quaternion& rotation, const Vector3& scale)
        : TransformTRS(Vector3d(position), rotation, scale)
    {
    }

    Matrix4x4 getMatrix() const
    {
        Matrix4x4 temp;
        temp.MakeTransform(m_Position.ToVector3(), m_Scale, m_Rotation);
        return temp;
    }
};

template<typename TransferFunction>
void TransformTRS::Transfer(TransferFunction& transfer)
{
    if constexpr (TransferFunction::IsReading())
    {
        Vector3 legacy_position {Vector3::ZERO};
        transfer.Transfer(legacy_position, "m_position");
        transfer.Transfer(m_Position, "m_positionD");
        if (m_Position == Vector3d::ZERO && legacy_position != Vector3::ZERO)
        {
            m_Position = Vector3d(legacy_position);
        }
    }
    else
    {
        transfer.Transfer(m_Position, "m_positionD");
        Vector3 legacy_position = m_Position.ToVector3();
        transfer.Transfer(legacy_position, "m_position");
    }
    transfer.Transfer(m_Scale, "m_scale");
    transfer.Transfer(m_Rotation, "m_rotation");
}
