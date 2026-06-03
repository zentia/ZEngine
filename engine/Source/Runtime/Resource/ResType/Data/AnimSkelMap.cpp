#include "AnimSkelMap.h"

IMPLEMENT_REGISTER_CLASS(AnimSkelMap);
IMPLEMENT_OBJECT_SERAILIZE(AnimSkelMap)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AnimSkelMap)

template<typename TransferFunction>
void AnimSkelMap::Transfer(TransferFunction& transfer)
{
}