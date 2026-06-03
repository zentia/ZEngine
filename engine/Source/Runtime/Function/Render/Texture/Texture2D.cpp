// =============================================================================
// Texture2D.cpp
// -----------------------------------------------------------------------------
// Reflection registration + Transfer template instantiation for Texture2D.
// The class is intentionally state-only -- there is no GPU lifecycle here
// (see Texture2D.h header comment, route-B section).
// =============================================================================

#include "Runtime/Function/Render/Texture/Texture2D.h"

IMPLEMENT_REGISTER_CLASS(Texture2D)
IMPLEMENT_OBJECT_SERAILIZE(Texture2D)

// Transfer schema. Field names are kept short and stable -- if you rename
// one, every existing .zasset on disk becomes unreadable. The order
// (width / height / format / pixels) is documentation only; SerializedFile
// keys by the string label so reordering is safe, but for readability
// we keep the descriptor fields first and the bulk blob last.
template<typename TransferFunction>
void Texture2D::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_Width, "width");
    transfer.Transfer(m_Height, "height");
    transfer.Transfer(m_Format, "format");
    transfer.Transfer(m_Pixels, "pixels");
    // Appended after "pixels" for schema evolution: an old single-mip .zasset
    // has no "mip_offsets" node, so SafeBinaryRead returns kNotFound and leaves
    // this empty -- GetMipCount()/GetMipSpan() then treat m_Pixels as one mip.
    transfer.Transfer(m_MipOffsets, "mip_offsets");
}

// Force-emit Transfer<...> for every concrete TransferFunction the engine
// uses (JSONRead / JSONWrite / StreamedBinaryRead/Write / SafeBinaryRead /
// GenerateTypeTreeTransfer). Without this, IMPLEMENT_OBJECT_SERAILIZE's
// VirtualRedirectTransfer overrides would link-fail because the template
// definition only lives in this TU. Mirrors MaterialRes.cpp.
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Texture2D)
