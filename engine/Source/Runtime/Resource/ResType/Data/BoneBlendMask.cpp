#include "BoneBlendMask.h"

IMPLEMENT_REGISTER_CLASS(BoneBlendMask);
IMPLEMENT_OBJECT_SERAILIZE(BoneBlendMask);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(BoneBlendMask)

template<typename TransferFunction>
void BoneBlendMask::Transfer(TransferFunction& transfer)
{
}