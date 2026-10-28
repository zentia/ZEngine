#include "AnimationClip.h"



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
    // APPEND-ONLY (P0.1). Must stay last: positional readers follow the
    // writer's sequence, so inserting a field above `node_channels` would
    // misalign every clip written before this existed. Those clips simply have
    // no "duration" node and read back as 0.
    transfer.Transfer(duration, "duration");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimNodeMap, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimationChannel, , Transfer, void)
INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimationClip, , Transfer, void)