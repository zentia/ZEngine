#include "Behaviour.h"

IMPLEMENT_OBJECT_SERAILIZE(Behaviour);
IMPLEMENT_REGISTER_CLASS(Behaviour);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Behaviour);

template<typename TransferFunction>
void Behaviour::Transfer(TransferFunction& transfer)
{
}