#include "Runtime/Function/Framework/Component/PrefabRefComponent.h"

IMPLEMENT_REGISTER_CLASS(PrefabRefComponent)
IMPLEMENT_OBJECT_SERIALIZE(PrefabRefComponent)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(PrefabRefComponent)

template<typename TransferFunction>
void PrefabRefComponent::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_ReferencedPrefab, "m_referenced_prefab");
    transfer.Transfer(m_Expanded, "m_expanded");
}
