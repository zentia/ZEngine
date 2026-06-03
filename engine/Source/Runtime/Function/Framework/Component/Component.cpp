#include "Component.h"

#include "Runtime/Core/Serialize/TransferFunctions/SerializeTransfer.h"

IMPLEMENT_OBJECT_SERIALIZE(Component);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Component)

template<typename TransferFunction>
void Component::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Enabled, "enabled");
}