#pragma once

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Matrix4.h"

#include <cstdint>
#include <vector>

class RenderEntity
{
public:
    uint32_t m_InstanceId {0};
    Matrix4x4 m_ModelMatrix {Matrix4x4::IDENTITY};

    // mesh
    size_t m_MeshAssetId {0};
    bool m_EnableVertexBlending {false};
    std::vector<Matrix4x4> m_JointMatrices;
    AxisAlignedBox m_BoundingBox;

    // material
    size_t m_MaterialAssetId {0};
    bool m_Blend {false};
    bool m_DoubleSided {false};
    Vector4 m_BaseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    float m_MetallicFactor {1.0f};
    float m_RoughnessFactor {1.0f};
    float m_NormalScale {1.0f};
    float m_OcclusionStrength {1.0f};
    Vector3 m_EmissiveFactor {0.0f, 0.0f, 0.0f};
};
