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
    // Legacy names are probed only by name-addressed readers (SerializeUtility.h);
    // a positional read (prefab instantiate clones every MeshRenderer sub-mesh
    // through it) gets the writer's sequence -- the extra probes here used to
    // misalign the stream and crash on the next string.
    if constexpr (kTransferProbesLegacyFields<TransferFunction>)
    {
        Vector3 legacy_position {Vector3::ZERO};
        transfer.Transfer(legacy_position, "m_position");
        transfer.Transfer(m_Position, "m_positionD");

        // Legacy .object.json / authored JSON spell these without the "m_"
        // prefix and keep position single-precision. Reading is additive so a
        // file written by either convention loads; writing still emits the
        // canonical names.
        Vector3 authored_position {Vector3::ZERO};
        transfer.Transfer(authored_position, "position");

        if (m_Position == Vector3d::ZERO)
        {
            if (legacy_position != Vector3::ZERO)
            {
                m_Position = Vector3d(legacy_position);
            }
            else if (authored_position != Vector3::ZERO)
            {
                m_Position = Vector3d(authored_position);
            }
        }

        Quaternion authored_rotation {Quaternion::IDENTITY};
        transfer.Transfer(authored_rotation, "rotation");
        if (m_Rotation == Quaternion::IDENTITY && authored_rotation != Quaternion::IDENTITY)
        {
            m_Rotation = authored_rotation;
        }
    }
    else
    {
        transfer.Transfer(m_Position, "m_positionD");
        Vector3 legacy_position = m_Position.ToVector3();
        transfer.Transfer(legacy_position, "m_position");
    }
    transfer.Transfer(m_Scale, "m_scale");
    if constexpr (kTransferProbesLegacyFields<TransferFunction>)
    {
        Vector3 authored_scale {Vector3::UNIT_SCALE};
        transfer.Transfer(authored_scale, "scale");
        if (m_Scale == Vector3::UNIT_SCALE && authored_scale != Vector3::UNIT_SCALE)
        {
            m_Scale = authored_scale;
        }
    }
    transfer.Transfer(m_Rotation, "m_rotation");
}
