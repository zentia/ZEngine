#include "AnimationClip.h"

IMPLEMENT_REGISTER_CLASS(AnimationAsset);
IMPLEMENT_OBJECT_SERAILIZE(AnimationAsset);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AnimationAsset);

template<typename TransferFunction>
void AnimationAsset::Transfer(TransferFunction& transfer_function)
{
}