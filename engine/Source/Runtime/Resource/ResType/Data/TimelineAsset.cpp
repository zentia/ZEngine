#include "TimelineAsset.h"

IMPLEMENT_REGISTER_CLASS(TimelineAsset)
IMPLEMENT_OBJECT_SERAILIZE(TimelineAsset)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(TimelineAsset)

template<typename TransferFunction>
void TimelineAsset::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);
    transfer.Transfer(m_Duration, "m_duration");
    transfer.Transfer(m_FrameRate, "m_frame_rate");
    transfer.Transfer(m_Tracks, "m_tracks");
}
