#include "SkeletonData.h"

IMPLEMENT_REGISTER_CLASS(SkeletonData);
IMPLEMENT_OBJECT_SERAILIZE(SkeletonData)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(SkeletonData)


template<typename TransferFunction>
void SkeletonData::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(bones_map, "bones_map");
    transfer.Transfer(is_flat, "is_flat");
    transfer.Transfer(root_index, "root_index");
    transfer.Transfer(in_topological_order, "in_topological_order");
}

template<typename TransferFunction>
void RawBone::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(index, "index");
    transfer.Transfer(binding_pose, "binding_pose");
    transfer.Transfer(tpose_matrix, "tpose_matrix");
    transfer.Transfer(parent_index, "parent_index");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(RawBone, , Transfer, void)