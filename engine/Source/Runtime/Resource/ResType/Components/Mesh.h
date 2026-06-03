#pragma once
#include "Runtime/Core/Math/LocalTransform.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <vector>

class SubMeshRes
{
public:
    DECLARE_SERIALIZE(SubMeshRes)

    // 引用导入后的 Mesh 资产，而不是直接引用 .obj 源文件
    eastl::string m_MeshAsset;
    LocalTransform m_Transform;

    // Legacy assets stored material per sub mesh; keep this field only for migration compatibility.
    eastl::string m_LegacyMaterialAsset;
};

template<typename TransferFunction>
void SubMeshRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_MeshAsset, "mesh");
    transfer.Transfer(m_Transform, "transform");
    transfer.Transfer(m_LegacyMaterialAsset, "material");
}
