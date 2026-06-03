#include "SkeletonData.h"

IMPLEMENT_REGISTER_CLASS(SkeletonData);
IMPLEMENT_OBJECT_SERAILIZE(SkeletonData)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SkeletonData)

template<typename TransferFunction>
void SkeletonData::Transfer(TransferFunction& transfer)
{
}