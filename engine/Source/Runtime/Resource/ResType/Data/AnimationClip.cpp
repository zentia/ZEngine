#include "AnimationClip.h"

IMPLEMENT_REGISTER_CLASS(AnimationAsset);
IMPLEMENT_OBJECT_SERAILIZE(AnimationAsset);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(AnimationAsset);


template<typename TransferFunction>
void AnimNodeMap::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(convert, "convert");
}

template<typename TransferFunction>
void AnimationChannel::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(position_keys, "position_keys");
    transfer.Transfer(rotation_keys, "rotation_keys");
    transfer.Transfer(scaling_keys, "scaling_keys");
}

template<typename TransferFunction>
void AnimationClip::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(total_frame, "total_frame");
    transfer.Transfer(node_count, "node_count");
    transfer.Transfer(node_channels, "node_channels");
}

template<typename TransferFunction>
void AnimationAsset::Transfer(TransferFunction& transfer_function)
{
    transfer_function.Transfer(node_map, "node_map");
    transfer_function.Transfer(clip_data, "clip_data");
    transfer_function.Transfer(skeleton_file_path, "skeleton_file_path");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimNodeMap, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimationChannel, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimationClip, , Transfer, void)