#include "LightComponent.h"

IMPLEMENT_REGISTER_CLASS(LightComponent)
IMPLEMENT_OBJECT_SERIALIZE(LightComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(LightComponent)

template<typename TransferFunction>
void LightComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_Flux, "flux");
    transfer.Transfer(m_Radius, "radius");
}
