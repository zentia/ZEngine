#include "SkeletonData.h"



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