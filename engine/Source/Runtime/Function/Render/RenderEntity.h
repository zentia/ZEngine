#pragma once

#include "Runtime/Core/Math/AxisAligned.h"
#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Render/MaterialCustomParamSlots.h"
#include "Runtime/Function/ShaderLab/ShaderPropertyBlob.h"

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
    /// 运行时材质实例 id。0 = 用 `m_MaterialAssetId` 那份**共享** GpuPBRMaterial；
    /// 非 0 = 用 `m_MaterialInstances` 里这一份（== m_InstanceId，一个 part 一份）。
    ///
    /// 之所以不把实例塞进同一个 m_Materials：那边是按材质资产 id 索引的，两个
    /// allocator 的 id 空间会撞。单独一个 map，键就是 per-part 的 m_InstanceId，
    /// 而这个 id 由 m_InstanceIdAllocator 回收复用，所以 map 的规模上界是"同时带
    /// override 的 part 数"，不会随关卡加载次数无限涨。
    uint32_t m_MaterialInstanceId {0};
    bool m_Blend {false};
    bool m_DoubleSided {false};
    Vector4 m_BaseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
    float m_MetallicFactor {1.0f};
    float m_RoughnessFactor {1.0f};
    float m_NormalScale {1.0f};
    float m_OcclusionStrength {1.0f};
    Vector3 m_EmissiveFactor {0.0f, 0.0f, 0.0f};

    /// 运行时属性块槽位。见 GameObjectMaterialDesc::m_CustomParams。
    Vector4 m_CustomParams[k_MaterialCustomParamCount] {};

    /// ShaderLab 具名属性的**覆盖**字节。见 GameObjectMaterialDesc::m_PropertyBlob：
    /// 那份是资产值，这份是 per-renderer 的 override，上传时按 mask 盖在资产值上。
    ///
    /// 落点表不在这里 —— 它由共享材质的 GpuPBRMaterial::property_slots 携带（同一
    /// 个材质资产的所有 part 共用一份，没必要每个 part 拷一遍 std::vector）。
    ZEngine::ShaderLab::ShaderPropertyBlob m_PropertyBlob;
};
