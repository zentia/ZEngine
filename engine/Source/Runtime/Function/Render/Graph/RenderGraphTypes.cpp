#include "Runtime/Function/Render/Graph/RenderGraphTypes.h"

namespace
{
    // Bytes per pixel per format. Only used to size the transient allocator and to
    // decide aliasing, so an approximation is fine -- it never reaches the RHI.
    size_t BytesPerPixel(RGFormat format)
    {
        switch (format)
        {
            case RGFormat::R8_UNORM:
            case RGFormat::R8_UINT:
                return 1;
            case RGFormat::R16_UINT:
            case RGFormat::R16G16_SFLOAT:
                return 4;
            case RGFormat::R32_UINT:
            case RGFormat::R32_SFLOAT:
            case RGFormat::R32G32_SFLOAT:
                return 8;
            case RGFormat::R8G8B8A8_UNORM:
            case RGFormat::R8G8B8A8_SRGB:
            case RGFormat::B8G8R8A8_UNORM:
            case RGFormat::B8G8R8A8_SRGB:
            case RGFormat::D24_UNORM_S8_UINT:
            case RGFormat::D32_SFLOAT:
                return 4;
            case RGFormat::R16G16B16A16_SFLOAT:
                return 8;
            case RGFormat::R32G32B32A32_SFLOAT:
                return 16;
            case RGFormat::Unknown:
            default:
                return 0;
        }
    }
}  // namespace

size_t RGTextureDesc::GetByteSize() const
{
    const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height) *
                          static_cast<size_t>(depth) * static_cast<size_t>(array_size);
    size_t bytes = texels * BytesPerPixel(format);

    // A full mip chain costs ~4/3 of the base level.
    if (mip_levels > 1)
    {
        bytes = bytes + bytes / 3;
    }
    return bytes;
}

const char* RGToString(RGResourceState state)
{
    switch (state)
    {
        case RGResourceState::Undefined:
            return "Undefined";
        case RGResourceState::RenderTarget:
            return "RenderTarget";
        case RGResourceState::DepthStencilWrite:
            return "DepthStencilWrite";
        case RGResourceState::DepthStencilRead:
            return "DepthStencilRead";
        case RGResourceState::ShaderResource:
            return "ShaderResource";
        case RGResourceState::UnorderedAccess:
            return "UnorderedAccess";
        case RGResourceState::CopySource:
            return "CopySource";
        case RGResourceState::CopyDest:
            return "CopyDest";
        case RGResourceState::Present:
            return "Present";
        default:
            return "?";
    }
}

const char* RGToString(RGPassType type)
{
    switch (type)
    {
        case RGPassType::Render:
            return "Render";
        case RGPassType::Compute:
            return "Compute";
        case RGPassType::Copy:
            return "Copy";
        default:
            return "?";
    }
}
