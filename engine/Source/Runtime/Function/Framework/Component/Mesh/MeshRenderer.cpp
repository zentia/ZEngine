#include "MeshRenderer.h"

#include "Runtime/Function/Framework/Component/Transform/Transform.h"

IMPLEMENT_REGISTER_CLASS(MeshRenderer)
IMPLEMENT_OBJECT_SERIALIZE(MeshRenderer)

template<typename TransferFunction>
void MeshRenderer::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
}
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(MeshRenderer)

GameObjectDesc MeshRenderer::BuildGameObjectDesc(const Transform* transform_component) const
{
    return BuildGameObjectDescFromParts(BuildRenderParts(transform_component));
}
