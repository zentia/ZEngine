#include "Runtime/Resource/ResType/Components/RigidBody.h"

#include "Runtime/Core/Base/Macro.h"

IMPLEMENT_REGISTER_CLASS(RigidBodyShape)
IMPLEMENT_OBJECT_SERIALIZE(RigidBodyShape)

template<typename TransferFunction>
void RigidBodyShape::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_GlobalTransform, "global_transform");
    transfer.Transfer(m_Type, "type");
    transfer.Transfer(m_LocalTransform, "local_transform");
    transfer.Transfer(m_Geometry, "geometry");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(RigidBodyShape, , Transfer, void)