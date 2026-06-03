#include "MeshData.h"

IMPLEMENT_REGISTER_CLASS(MeshData)
IMPLEMENT_OBJECT_SERIALIZE(MeshData)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(MeshData)

template<typename TransferFunction>
void MeshData::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(vertex_buffer, "vertex_buffer");
    transfer.Transfer(index_buffer, "index_buffer");
    transfer.Transfer(bind, "bind");
}
