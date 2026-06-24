#include "Runtime/Function/Render/Interface/DX12/DX12RHI.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Core/Thread/ThreadManager.h"
#include "Runtime/Function/Render/Interface/DX12/DX12HostSync.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/UI/Render/UIGpuResources.h"
#include "Runtime/Project/ProjectInfo.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <cstring>
#include <d3dcompiler.h>
#include <functional>
#include <map>
#include <utility>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    bool CheckDX12(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            LOG_ERROR(ZRender, "{} HRESULT=0x{:08X}", message, static_cast<unsigned int>(result));
            return false;
        }
        return true;
    }

    bool IsActualDx12DeviceRemoval(HRESULT reason)
    {
        return reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET ||
               reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR || reason == DXGI_ERROR_DEVICE_HUNG;
    }

    bool IsDynamicDescriptorType(RHIDescriptorType type)
    {
        return type == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
               type == RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    }

    struct TextureUploadStaging
    {
        UINT subresource_index {0};
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT num_rows {0};
        UINT64 row_size_in_bytes {0};
        const uint8_t* pixels {nullptr};
        size_t source_row_pitch {0};
        std::vector<uint8_t> owned_pixels;
    };

    void TransitionImageOnList(DX12Image* image,
                               uint32_t subresource,
                               D3D12_RESOURCE_STATES new_state,
                               ID3D12GraphicsCommandList* command_list)
    {
        if (image == nullptr || image->getResource() == nullptr || command_list == nullptr)
        {
            return;
        }

        const uint32_t subresource_count = image->getSubresourceCount();
        if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
        {
            for (uint32_t i = 0; i < subresource_count; ++i)
            {
                TransitionImageOnList(image, i, new_state, command_list);
            }
            return;
        }

        D3D12_RESOURCE_STATES old_state = image->getState(subresource);
        if (old_state == new_state)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = image->getResource();
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = old_state;
        barrier.Transition.StateAfter = new_state;
        command_list->ResourceBarrier(1, &barrier);
        image->setState(subresource, new_state);
    }

    uint16_t Float32ToFloat16(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));

        const uint32_t sign = (bits >> 16) & 0x8000u;
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        const uint32_t mantissa = bits & 0x7FFFFFu;

        if (exponent == 0xFFu)
        {
            return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
        }
        if (exponent == 0)
        {
            return static_cast<uint16_t>(sign);
        }

        int32_t new_exp = static_cast<int32_t>(exponent) - 127 + 15;
        if (new_exp >= 31)
        {
            return static_cast<uint16_t>(sign | 0x7C00u);
        }
        if (new_exp <= 0)
        {
            return static_cast<uint16_t>(sign);
        }

        const uint32_t new_mantissa = mantissa >> 13;
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp) << 10) | new_mantissa);
    }

    bool CreateTextureUploadStaging(ID3D12Device* device,
                                    ID3D12Resource* texture_resource,
                                    const void* pixels,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t array_layer,
                                    uint32_t mip_level,
                                    uint32_t mip_levels,
                                    uint32_t array_layers,
                                    uint32_t source_bytes_per_pixel,
                                    uint32_t upload_bytes_per_pixel,
                                    TextureUploadStaging& out_staging)
    {
        if (device == nullptr || texture_resource == nullptr || pixels == nullptr)
        {
            return false;
        }

        const uint8_t* upload_pixels = static_cast<const uint8_t*>(pixels);
        if (source_bytes_per_pixel == 16 && upload_bytes_per_pixel == 8)
        {
            out_staging.owned_pixels.resize(static_cast<size_t>(width) * height * 8);
            const float* src = static_cast<const float*>(pixels);
            for (uint32_t i = 0; i < width * height; ++i)
            {
                for (uint32_t channel = 0; channel < 4; ++channel)
                {
                    const uint16_t half = Float32ToFloat16(src[i * 4 + channel]);
                    std::memcpy(&out_staging.owned_pixels[(static_cast<size_t>(i) * 4 + channel) * 2],
                                &half,
                                sizeof(half));
                }
            }
            upload_pixels = out_staging.owned_pixels.data();
        }

        out_staging.pixels = upload_pixels;
        out_staging.source_row_pitch = static_cast<size_t>(width) * upload_bytes_per_pixel;
        out_staging.subresource_index = mip_level + array_layer * mip_levels;

        D3D12_RESOURCE_DESC texture_desc = texture_resource->GetDesc();
        UINT64 upload_buffer_size = 0;
        device->GetCopyableFootprints(&texture_desc,
                                      out_staging.subresource_index,
                                      1,
                                      0,
                                      &out_staging.footprint,
                                      &out_staging.num_rows,
                                      &out_staging.row_size_in_bytes,
                                      &upload_buffer_size);

        D3D12_HEAP_PROPERTIES heap_properties = {};
        heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC upload_desc = {};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_buffer_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.Format = DXGI_FORMAT_UNKNOWN;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (!CheckDX12(device->CreateCommittedResource(&heap_properties,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &upload_desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(&out_staging.buffer)),
                       "DX12 create texture upload buffer failed"))
        {
            return false;
        }

        uint8_t* mapped_data = nullptr;
        D3D12_RANGE read_range {0, 0};
        if (!CheckDX12(out_staging.buffer->Map(0, &read_range, reinterpret_cast<void**>(&mapped_data)),
                       "DX12 map texture upload buffer failed"))
        {
            return false;
        }

        for (UINT row = 0; row < out_staging.num_rows; ++row)
        {
            std::memcpy(mapped_data + out_staging.footprint.Offset +
                            static_cast<size_t>(row) * out_staging.footprint.Footprint.RowPitch,
                        upload_pixels + static_cast<size_t>(row) * out_staging.source_row_pitch,
                        std::min<size_t>(out_staging.source_row_pitch, static_cast<size_t>(out_staging.row_size_in_bytes)));
        }
        out_staging.buffer->Unmap(0, nullptr);
        return true;
    }

    void RecordTextureCopy(ID3D12GraphicsCommandList* command_list,
                           ID3D12Resource* texture_resource,
                           const TextureUploadStaging& staging)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture_resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = staging.subresource_index;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = staging.buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = staging.footprint;

        command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Forward declarations -- definitions live further down next to
    // GetFormatBytesPerPixel; the mip uploader below needs them earlier.
    bool IsBlockCompressedRHIFormat(RHIFormat format);
    uint32_t BcBlockBytesRHI(RHIFormat format);

    // Build one upload-buffer staging for a single mip subresource, reading the
    // mip's data from `mip_src` (tightly packed: `source_row_pitch` bytes per
    // source row, `num_source_rows` rows). Works for both linear (rows = height,
    // pitch = width*bpp) and block-compressed (rows = blocksY, pitch =
    // blocksX*blockBytes) layouts because GetCopyableFootprints already reports
    // the correct destination row count / size for the subresource's format.
    bool CreateMipUploadStaging(ID3D12Device* device,
                                ID3D12Resource* texture_resource,
                                const uint8_t* mip_src,
                                UINT subresource_index,
                                size_t source_row_pitch,
                                UINT num_source_rows,
                                TextureUploadStaging& out_staging)
    {
        if (device == nullptr || texture_resource == nullptr || mip_src == nullptr)
        {
            return false;
        }

        out_staging.subresource_index = subresource_index;
        out_staging.source_row_pitch = source_row_pitch;

        D3D12_RESOURCE_DESC texture_desc = texture_resource->GetDesc();
        UINT64 upload_buffer_size = 0;
        device->GetCopyableFootprints(&texture_desc,
                                      subresource_index,
                                      1,
                                      0,
                                      &out_staging.footprint,
                                      &out_staging.num_rows,
                                      &out_staging.row_size_in_bytes,
                                      &upload_buffer_size);

        D3D12_HEAP_PROPERTIES heap_properties = {};
        heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC upload_desc = {};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_buffer_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.Format = DXGI_FORMAT_UNKNOWN;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (!CheckDX12(device->CreateCommittedResource(&heap_properties,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &upload_desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr,
                                                       IID_PPV_ARGS(&out_staging.buffer)),
                       "DX12 create mip upload buffer failed"))
        {
            return false;
        }

        uint8_t* mapped_data = nullptr;
        D3D12_RANGE read_range {0, 0};
        if (!CheckDX12(out_staging.buffer->Map(0, &read_range, reinterpret_cast<void**>(&mapped_data)),
                       "DX12 map mip upload buffer failed"))
        {
            return false;
        }

        const UINT rows = std::min<UINT>(num_source_rows, out_staging.num_rows);
        for (UINT row = 0; row < rows; ++row)
        {
            std::memcpy(mapped_data + out_staging.footprint.Offset +
                            static_cast<size_t>(row) * out_staging.footprint.Footprint.RowPitch,
                        mip_src + static_cast<size_t>(row) * source_row_pitch,
                        std::min<size_t>(source_row_pitch, static_cast<size_t>(out_staging.row_size_in_bytes)));
        }
        out_staging.buffer->Unmap(0, nullptr);
        return true;
    }

    // Upload a tightly-packed mip chain (mip0 first, every successive mip
    // concatenated -- the exact layout TextureCompressor / Texture2D::m_Pixels
    // produce) into a freshly-created DX12 texture. Handles linear (RGBA8) and
    // block-compressed (BC1/BC3/BC7) layouts. array_layer 0 only.
    bool UploadPackedMipChain(ID3D12Device* device,
                              DX12Image* dx12_image,
                              const uint8_t* blob,
                              uint32_t width,
                              uint32_t height,
                              uint32_t mip_levels,
                              RHIFormat storage_format,
                              uint32_t bytes_per_pixel,
                              std::function<bool(std::function<void(ID3D12GraphicsCommandList*)>)> execute_upload)
    {
        const bool block = IsBlockCompressedRHIFormat(storage_format);
        const uint32_t block_bytes = block ? BcBlockBytesRHI(storage_format) : 0;
        const uint32_t total_mips = dx12_image->getMipLevels();

        std::vector<TextureUploadStaging> stagings;
        stagings.reserve(mip_levels);

        size_t blob_offset = 0;
        for (uint32_t mip = 0; mip < mip_levels; ++mip)
        {
            const uint32_t mw = std::max<uint32_t>(1u, width >> mip);
            const uint32_t mh = std::max<uint32_t>(1u, height >> mip);

            size_t source_row_pitch = 0;
            UINT num_source_rows = 0;
            size_t mip_size = 0;
            if (block)
            {
                const uint32_t blocks_x = (mw + 3) / 4;
                const uint32_t blocks_y = (mh + 3) / 4;
                source_row_pitch = static_cast<size_t>(blocks_x) * block_bytes;
                num_source_rows = blocks_y;
                mip_size = source_row_pitch * blocks_y;
            }
            else
            {
                source_row_pitch = static_cast<size_t>(mw) * bytes_per_pixel;
                num_source_rows = mh;
                mip_size = source_row_pitch * mh;
            }

            // subresource = mip + array_layer * total_mips, array_layer = 0.
            const UINT subresource_index = mip;
            TextureUploadStaging staging {};
            if (!CreateMipUploadStaging(device,
                                        dx12_image->getResource(),
                                        blob + blob_offset,
                                        subresource_index,
                                        source_row_pitch,
                                        num_source_rows,
                                        staging))
            {
                return false;
            }
            stagings.push_back(std::move(staging));
            blob_offset += mip_size;
        }
        (void)total_mips;

        return execute_upload([&](ID3D12GraphicsCommandList* command_list) {
            for (const TextureUploadStaging& staging : stagings)
            {
                TransitionImageOnList(dx12_image,
                                      staging.subresource_index,
                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                      command_list);
                RecordTextureCopy(command_list, dx12_image->getResource(), staging);
                TransitionImageOnList(dx12_image,
                                      staging.subresource_index,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      command_list);
            }
        });
    }

    DXGI_FORMAT ToDX12Format(RHIFormat format)
    {
        switch (format)
        {
            case RHI_FORMAT_R8_UNORM:
                return DXGI_FORMAT_R8_UNORM;
            case RHI_FORMAT_R8G8_UNORM:
                return DXGI_FORMAT_R8G8_UNORM;
            case RHI_FORMAT_R8G8B8A8_UNORM:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case RHI_FORMAT_R8G8B8A8_SRGB:
                return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            case RHI_FORMAT_B8G8R8A8_UNORM:
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            case RHI_FORMAT_B8G8R8A8_SRGB:
                return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            case RHI_FORMAT_R16G16B16A16_SFLOAT:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            // Block-compressed (texture cook). BC1 has no separate RGB/RGBA DXGI
            // form -- the 1-bit-alpha variant is the same DXGI_FORMAT_BC1_*; the
            // RGB-only encode simply leaves alpha at 1.
            case RHI_FORMAT_BC1_RGB_UNORM_BLOCK:
            case RHI_FORMAT_BC1_RGBA_UNORM_BLOCK:
                return DXGI_FORMAT_BC1_UNORM;
            case RHI_FORMAT_BC1_RGB_SRGB_BLOCK:
            case RHI_FORMAT_BC1_RGBA_SRGB_BLOCK:
                return DXGI_FORMAT_BC1_UNORM_SRGB;
            case RHI_FORMAT_BC3_UNORM_BLOCK:
                return DXGI_FORMAT_BC3_UNORM;
            case RHI_FORMAT_BC3_SRGB_BLOCK:
                return DXGI_FORMAT_BC3_UNORM_SRGB;
            case RHI_FORMAT_BC7_UNORM_BLOCK:
                return DXGI_FORMAT_BC7_UNORM;
            case RHI_FORMAT_BC7_SRGB_BLOCK:
                return DXGI_FORMAT_BC7_UNORM_SRGB;
            case RHI_FORMAT_R32_SFLOAT:
                return DXGI_FORMAT_R32_FLOAT;
            case RHI_FORMAT_R32G32_SFLOAT:
                return DXGI_FORMAT_R32G32_FLOAT;
            case RHI_FORMAT_R32G32B32_SFLOAT:
                return DXGI_FORMAT_R32G32B32_FLOAT;
            case RHI_FORMAT_R32G32B32A32_SFLOAT:
                return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case RHI_FORMAT_R32_UINT:
                return DXGI_FORMAT_R32_UINT;
            case RHI_FORMAT_D16_UNORM:
                return DXGI_FORMAT_D16_UNORM;
            case RHI_FORMAT_D32_SFLOAT:
                return DXGI_FORMAT_D32_FLOAT;
            case RHI_FORMAT_D24_UNORM_S8_UINT:
                return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case RHI_FORMAT_D32_SFLOAT_S8_UINT:
                return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            default:
                return DXGI_FORMAT_UNKNOWN;
        }
    }

    DXGI_FORMAT ToDX12SwapchainFormat(RHIFormat format)
    {
        DXGI_FORMAT dxgi_format = ToDX12Format(format);
        return dxgi_format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : dxgi_format;
    }

    // Derive PSO RTV/DSV formats from the bound render pass (shadow maps, RP2, etc.).
    // Falls back to the swapchain color + global depth formats when renderPass is null.
    void ApplyPsoRenderTargetFormats(const RHIGraphicsPipelineCreateInfo& create_info,
                                     RHIFormat swapchain_format,
                                     RHIFormat global_depth_format,
                                     D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso_desc)
    {
        auto* dx12_render_pass = static_cast<DX12RenderPass*>(create_info.renderPass);
        if (dx12_render_pass == nullptr)
        {
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = ToDX12SwapchainFormat(swapchain_format);
            if (pso_desc.DepthStencilState.DepthEnable)
            {
                pso_desc.DSVFormat = ToDX12Format(global_depth_format);
            }
            return;
        }

        const RHISubpassDescription* subpass = dx12_render_pass->getSubpass(create_info.subpass);
        const auto& attachments = dx12_render_pass->getAttachments();
        if (subpass == nullptr || attachments.empty())
        {
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = ToDX12SwapchainFormat(swapchain_format);
            if (pso_desc.DepthStencilState.DepthEnable)
            {
                pso_desc.DSVFormat = ToDX12Format(global_depth_format);
            }
            return;
        }

        pso_desc.NumRenderTargets = std::min<uint32_t>(subpass->colorAttachmentCount, 8u);
        for (uint32_t color_index = 0; color_index < pso_desc.NumRenderTargets; ++color_index)
        {
            pso_desc.RTVFormats[color_index] = DXGI_FORMAT_UNKNOWN;
            if (subpass->pColorAttachments != nullptr)
            {
                const uint32_t attachment_index = subpass->pColorAttachments[color_index].attachment;
                if (attachment_index < attachments.size())
                {
                    pso_desc.RTVFormats[color_index] = ToDX12Format(attachments[attachment_index].format);
                }
            }
        }

        if (subpass->pDepthStencilAttachment != nullptr)
        {
            const uint32_t depth_attachment_index = subpass->pDepthStencilAttachment->attachment;
            if (depth_attachment_index < attachments.size())
            {
                pso_desc.DSVFormat = ToDX12Format(attachments[depth_attachment_index].format);
            }
            else if (pso_desc.DepthStencilState.DepthEnable)
            {
                pso_desc.DSVFormat = ToDX12Format(global_depth_format);
            }
        }
        else
        {
            pso_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        }
    }

    DXGI_FORMAT ToDX12UavFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            default:
                return format;
        }
    }

    D3D12_HEAP_TYPE ToDX12HeapType(RHIMemoryPropertyFlags properties)
    {
        if (properties & RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        {
            return D3D12_HEAP_TYPE_UPLOAD;
        }
        return D3D12_HEAP_TYPE_DEFAULT;
    }

    D3D12_HEAP_TYPE ToDX12BufferHeapType(RHIBufferUsageFlags usage, RHIMemoryPropertyFlags properties)
    {
        if ((properties & RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (usage & RHI_BUFFER_USAGE_TRANSFER_DST_BIT))
        {
            return D3D12_HEAP_TYPE_READBACK;
        }
        if (properties & RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        {
            return D3D12_HEAP_TYPE_UPLOAD;
        }
        return D3D12_HEAP_TYPE_DEFAULT;
    }

    D3D12_RESOURCE_STATES ToDX12BufferInitialState(D3D12_HEAP_TYPE heap_type)
    {
        if (heap_type == D3D12_HEAP_TYPE_UPLOAD)
        {
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        if (heap_type == D3D12_HEAP_TYPE_READBACK)
        {
            return D3D12_RESOURCE_STATE_COPY_DEST;
        }
        return D3D12_RESOURCE_STATE_COMMON;
    }

    void RecordDefaultHeapBufferCopy(ID3D12GraphicsCommandList* command_list,
                                     ID3D12Resource* src_resource,
                                     ID3D12Resource* dst_resource,
                                     uint64_t src_offset,
                                     uint64_t dst_offset,
                                     uint64_t size,
                                     D3D12_RESOURCE_STATES dst_state_after_copy)
    {
        if (command_list == nullptr || src_resource == nullptr || dst_resource == nullptr || size == 0)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER to_copy_dest = {};
        to_copy_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy_dest.Transition.pResource = dst_resource;
        to_copy_dest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_copy_dest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        to_copy_dest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &to_copy_dest);

        command_list->CopyBufferRegion(dst_resource, dst_offset, src_resource, src_offset, size);

        D3D12_RESOURCE_BARRIER to_final = {};
        to_final.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_final.Transition.pResource = dst_resource;
        to_final.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_final.Transition.StateAfter = dst_state_after_copy;
        to_final.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &to_final);
    }

    bool IsDX12DepthFormat(RHIFormat format)
    {
        return format == RHI_FORMAT_D16_UNORM || format == RHI_FORMAT_D32_SFLOAT ||
               format == RHI_FORMAT_D24_UNORM_S8_UINT || format == RHI_FORMAT_D32_SFLOAT_S8_UINT;
    }

    D3D12_RESOURCE_STATES ToDX12ResourceState(RHIImageLayout layout)
    {
        switch (layout)
        {
            case RHI_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RHI_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            case RHI_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
                return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RHI_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            case RHI_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
            case RHI_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            case RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case RHI_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                return D3D12_RESOURCE_STATE_COPY_DEST;
            case RHI_IMAGE_LAYOUT_GENERAL:
                return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RHI_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                return D3D12_RESOURCE_STATE_PRESENT;
            case RHI_IMAGE_LAYOUT_UNDEFINED:
            case RHI_IMAGE_LAYOUT_PREINITIALIZED:
            default:
                return D3D12_RESOURCE_STATE_COMMON;
        }
    }

    D3D12_RESOURCE_FLAGS ToDX12ResourceFlags(RHIImageUsageFlags usage)
    {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        if (usage & RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (usage & RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }
        if (usage & RHI_IMAGE_USAGE_STORAGE_BIT)
        {
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        return flags;
    }

    D3D12_TEXTURE_ADDRESS_MODE ToDX12AddressMode(RHISamplerAddressMode mode)
    {
        switch (mode)
        {
            case RHI_SAMPLER_ADDRESS_MODE_REPEAT:
                return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            case RHI_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
                return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            case RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
                return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            case RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
            default:
                return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        }
    }

    // True for the block-compressed formats the texture cook produces on DX12
    // (BC1/BC3/BC7). ASTC is intentionally NOT here -- it is a mobile-only
    // format that DX12 cannot sample, so cooked ASTC variants are never fed to
    // this backend (they are file-inspected, never rendered, per the plan).
    bool IsBlockCompressedRHIFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHI_FORMAT_BC1_RGB_UNORM_BLOCK:
            case RHI_FORMAT_BC1_RGB_SRGB_BLOCK:
            case RHI_FORMAT_BC1_RGBA_UNORM_BLOCK:
            case RHI_FORMAT_BC1_RGBA_SRGB_BLOCK:
            case RHI_FORMAT_BC3_UNORM_BLOCK:
            case RHI_FORMAT_BC3_SRGB_BLOCK:
            case RHI_FORMAT_BC7_UNORM_BLOCK:
            case RHI_FORMAT_BC7_SRGB_BLOCK:
                return true;
            default:
                return false;
        }
    }

    // Bytes per 4x4 block for a BC format. BC1 = 8, BC3/BC7 = 16.
    uint32_t BcBlockBytesRHI(RHIFormat format)
    {
        switch (format)
        {
            case RHI_FORMAT_BC1_RGB_UNORM_BLOCK:
            case RHI_FORMAT_BC1_RGB_SRGB_BLOCK:
            case RHI_FORMAT_BC1_RGBA_UNORM_BLOCK:
            case RHI_FORMAT_BC1_RGBA_SRGB_BLOCK:
                return 8;
            default:
                return 16;
        }
    }

    uint32_t GetFormatBytesPerPixel(RHIFormat format)
    {
        switch (format)
        {
            case RHI_FORMAT_R8_UNORM:
            case RHI_FORMAT_R8_SRGB:
                return 1;
            case RHI_FORMAT_R8G8_UNORM:
            case RHI_FORMAT_R8G8_SRGB:
                return 2;
            case RHI_FORMAT_R8G8B8A8_UNORM:
            case RHI_FORMAT_R8G8B8A8_SRGB:
            case RHI_FORMAT_B8G8R8A8_UNORM:
            case RHI_FORMAT_B8G8R8A8_SRGB:
            case RHI_FORMAT_R32_SFLOAT:
            case RHI_FORMAT_R32_UINT:
                return 4;
            case RHI_FORMAT_R16G16B16A16_SFLOAT:
            case RHI_FORMAT_R32G32_SFLOAT:
                return 8;
            case RHI_FORMAT_R32G32B32A32_SFLOAT:
                return 16;
            default:
                return 4;
        }
    }

    void PickDx12StorageFormat(RHIFormat requested_format,
                               RHIFormat& out_storage_format,
                               uint32_t& out_source_bytes_per_pixel,
                               uint32_t& out_upload_bytes_per_pixel)
    {
        out_storage_format = requested_format;
        out_source_bytes_per_pixel = GetFormatBytesPerPixel(requested_format);
        out_upload_bytes_per_pixel = out_source_bytes_per_pixel;

        if (requested_format == RHI_FORMAT_R32G32B32A32_SFLOAT)
        {
            out_storage_format = RHI_FORMAT_R16G16B16A16_SFLOAT;
            out_upload_bytes_per_pixel = 8;
            LOG_INFO(ZRender,
                     "DX12: storing HDR as R16G16B16A16 (source remains R32G32B32A32, converted on upload)");
        }
    }

    uint32_t GetDXGIFormatBytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_R8_UNORM:
                return 1;
            case DXGI_FORMAT_R8G8_UNORM:
                return 2;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R32_UINT:
                return 4;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R32G32_FLOAT:
                return 8;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return 16;
            default:
                return 4;
        }
    }

    DXGI_FORMAT ToDX12TypelessAwareRTVFormat(RHIFormat format)
    {
        return ToDX12SwapchainFormat(format);
    }

    D3D12_DESCRIPTOR_RANGE_TYPE ToDX12DescriptorRangeType(RHIDescriptorType type)
    {
        switch (type)
        {
            case RHI_DESCRIPTOR_TYPE_SAMPLER:
                return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            case RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            case RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            case RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            case RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case RHI_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            default:
                return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        }
    }

    D3D12_SHADER_VISIBILITY ToDX12ShaderVisibility(RHIShaderStageFlags stage_flags)
    {
        if (stage_flags == RHI_SHADER_STAGE_VERTEX_BIT)
        {
            return D3D12_SHADER_VISIBILITY_VERTEX;
        }
        if (stage_flags == RHI_SHADER_STAGE_FRAGMENT_BIT)
        {
            return D3D12_SHADER_VISIBILITY_PIXEL;
        }
        return D3D12_SHADER_VISIBILITY_ALL;
    }

    uint32_t AlignTo(uint32_t value, uint32_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    DXGI_FORMAT ToDX12IndexFormat(RHIIndexType index_type)
    {
        switch (index_type)
        {
            case RHI_INDEX_TYPE_UINT32:
                return DXGI_FORMAT_R32_UINT;
            case RHI_INDEX_TYPE_UINT16:
            default:
                return DXGI_FORMAT_R16_UINT;
        }
    }

    D3D12_INPUT_CLASSIFICATION ToDX12InputClassification(RHIVertexInputRate input_rate)
    {
        return input_rate == RHI_VERTEX_INPUT_RATE_INSTANCE ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    }

    D3D12_PRIMITIVE_TOPOLOGY ToDX12PrimitiveTopology(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHI_PRIMITIVE_TOPOLOGY_POINT_LIST:
                return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RHI_PRIMITIVE_TOPOLOGY_LINE_LIST:
                return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case RHI_PRIMITIVE_TOPOLOGY_LINE_STRIP:
                return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
                return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            default:
                return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }

    D3D12_PRIMITIVE_TOPOLOGY_TYPE ToDX12PrimitiveTopologyType(RHIPrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHI_PRIMITIVE_TOPOLOGY_POINT_LIST:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case RHI_PRIMITIVE_TOPOLOGY_LINE_LIST:
            case RHI_PRIMITIVE_TOPOLOGY_LINE_STRIP:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            case RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
            default:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
    }

    D3D12_FILL_MODE ToDX12FillMode(RHIPolygonMode mode)
    {
        return mode == RHI_POLYGON_MODE_LINE ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    }

    D3D12_CULL_MODE ToDX12CullMode(RHICullModeFlags flags)
    {
        if (flags & RHI_CULL_MODE_FRONT_BIT)
        {
            return D3D12_CULL_MODE_FRONT;
        }
        if (flags & RHI_CULL_MODE_BACK_BIT)
        {
            return D3D12_CULL_MODE_BACK;
        }
        return D3D12_CULL_MODE_NONE;
    }

    D3D12_COMPARISON_FUNC ToDX12ComparisonFunc(RHICompareOp compare_op)
    {
        switch (compare_op)
        {
            case RHI_COMPARE_OP_NEVER:
                return D3D12_COMPARISON_FUNC_NEVER;
            case RHI_COMPARE_OP_LESS:
                return D3D12_COMPARISON_FUNC_LESS;
            case RHI_COMPARE_OP_EQUAL:
                return D3D12_COMPARISON_FUNC_EQUAL;
            case RHI_COMPARE_OP_LESS_OR_EQUAL:
                return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case RHI_COMPARE_OP_GREATER:
                return D3D12_COMPARISON_FUNC_GREATER;
            case RHI_COMPARE_OP_NOT_EQUAL:
                return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case RHI_COMPARE_OP_GREATER_OR_EQUAL:
                return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case RHI_COMPARE_OP_ALWAYS:
            default:
                return D3D12_COMPARISON_FUNC_ALWAYS;
        }
    }

    D3D12_BLEND ToDX12BlendFactor(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHI_BLEND_FACTOR_ZERO:
                return D3D12_BLEND_ZERO;
            case RHI_BLEND_FACTOR_ONE:
                return D3D12_BLEND_ONE;
            case RHI_BLEND_FACTOR_SRC_COLOR:
                return D3D12_BLEND_SRC_COLOR;
            case RHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
                return D3D12_BLEND_INV_SRC_COLOR;
            case RHI_BLEND_FACTOR_DST_COLOR:
                return D3D12_BLEND_DEST_COLOR;
            case RHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
                return D3D12_BLEND_INV_DEST_COLOR;
            case RHI_BLEND_FACTOR_SRC_ALPHA:
                return D3D12_BLEND_SRC_ALPHA;
            case RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
                return D3D12_BLEND_INV_SRC_ALPHA;
            case RHI_BLEND_FACTOR_DST_ALPHA:
                return D3D12_BLEND_DEST_ALPHA;
            case RHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
                return D3D12_BLEND_INV_DEST_ALPHA;
            case RHI_BLEND_FACTOR_SRC_ALPHA_SATURATE:
                return D3D12_BLEND_SRC_ALPHA_SAT;
            default:
                return D3D12_BLEND_ONE;
        }
    }

    D3D12_BLEND_OP ToDX12BlendOp(RHIBlendOp op)
    {
        switch (op)
        {
            case RHI_BLEND_OP_SUBTRACT:
                return D3D12_BLEND_OP_SUBTRACT;
            case RHI_BLEND_OP_REVERSE_SUBTRACT:
                return D3D12_BLEND_OP_REV_SUBTRACT;
            case RHI_BLEND_OP_MIN:
                return D3D12_BLEND_OP_MIN;
            case RHI_BLEND_OP_MAX:
                return D3D12_BLEND_OP_MAX;
            case RHI_BLEND_OP_ADD:
            default:
                return D3D12_BLEND_OP_ADD;
        }
    }

    UINT8 ToDX12ColorWriteMask(RHIColorComponentFlags mask)
    {
        UINT8 result = 0;
        if (mask & RHI_COLOR_COMPONENT_R_BIT)
        {
            result |= D3D12_COLOR_WRITE_ENABLE_RED;
        }
        if (mask & RHI_COLOR_COMPONENT_G_BIT)
        {
            result |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        }
        if (mask & RHI_COLOR_COMPONENT_B_BIT)
        {
            result |= D3D12_COLOR_WRITE_ENABLE_BLUE;
        }
        if (mask & RHI_COLOR_COMPONENT_A_BIT)
        {
            result |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        }
        return result == 0 ? D3D12_COLOR_WRITE_ENABLE_ALL : result;
    }

    const char* ToDX12SemanticName(uint32_t location)
    {
        switch (location)
        {
            case 0:
                return "POSITION";
            case 1:
                return "NORMAL";
            case 2:
                return "TANGENT";
            case 3:
                return "TEXCOORD";
            case 4:
                return "COLOR";
            default:
                return "TEXCOORD";
        }
    }

    UINT ToDX12SemanticIndex(uint32_t location)
    {
        return location > 4 ? location - 3 : 0;
    }

    D3D12_FILTER ToDX12Filter(const RHISamplerCreateInfo* info)
    {
        const bool min_linear = info->minFilter == RHI_FILTER_LINEAR;
        const bool mag_linear = info->magFilter == RHI_FILTER_LINEAR;
        const bool mip_linear = info->mipmapMode == RHI_SAMPLER_MIPMAP_MODE_LINEAR;

        if (info->compareEnable)
        {
            if (min_linear && mag_linear && mip_linear)
            {
                return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
            }
            return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        }

        if (info->anisotropyEnable)
        {
            return D3D12_FILTER_ANISOTROPIC;
        }

        if (min_linear && mag_linear && mip_linear)
        {
            return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        }
        if (min_linear && mag_linear)
        {
            return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        }
        return D3D12_FILTER_MIN_MAG_MIP_POINT;
    }
}  // namespace

std::vector<std::type_index> DX12RHI::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(WindowSystem), GET_SYSTEM_TYPE(ThreadManager)};
}

bool DX12RHI::EnsureRtvDescriptorHeap()
{
    if (m_RtvHeap)
    {
        return true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = m_RtvDescriptorCapacity;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!CheckDX12(m_Device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_RtvHeap)),
                   "Create RTV descriptor heap failed"))
    {
        return false;
    }

    m_RtvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_NextRtvDescriptor = 0;
    return true;
}

bool DX12RHI::EnsureDsvDescriptorHeap()
{
    if (m_DsvHeap)
    {
        return true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = m_DsvDescriptorCapacity;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!CheckDX12(m_Device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_DsvHeap)),
                   "Create DSV descriptor heap failed"))
    {
        return false;
    }

    m_DsvDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_NextDsvDescriptor = 0;
    return true;
}

bool DX12RHI::AllocateRtvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle)
{
    if (!EnsureRtvDescriptorHeap() || m_NextRtvDescriptor >= m_RtvDescriptorCapacity)
    {
        LOG_ERROR(ZRender, "DX12 RTV descriptor heap exhausted");
        return false;
    }

    cpu_handle = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(m_NextRtvDescriptor) * m_RtvDescriptorSize;
    ++m_NextRtvDescriptor;
    return true;
}

bool DX12RHI::TryGetSwapchainBackBufferRtv(uint8_t back_buffer_index,
                                           D3D12_CPU_DESCRIPTOR_HANDLE& out_rtv) const
{
    if (back_buffer_index >= m_SwapchainImageviews.size())
    {
        return false;
    }

    const auto* dx12_view = static_cast<const DX12ImageView*>(m_SwapchainImageviews[back_buffer_index]);
    if (dx12_view == nullptr || !dx12_view->hasRenderTargetHandle())
    {
        return false;
    }

    out_rtv = dx12_view->getRenderTargetHandle();
    return true;
}

bool DX12RHI::AllocateDsvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle)
{
    if (!EnsureDsvDescriptorHeap() || m_NextDsvDescriptor >= m_DsvDescriptorCapacity)
    {
        LOG_ERROR(ZRender, "DX12 DSV descriptor heap exhausted");
        return false;
    }

    cpu_handle = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(m_NextDsvDescriptor) * m_DsvDescriptorSize;
    ++m_NextDsvDescriptor;
    return true;
}

void DX12RHI::TransitionImage(DX12Image* image,
                              uint32_t subresource,
                              D3D12_RESOURCE_STATES new_state)
{
    if (image == nullptr || image->getResource() == nullptr || !m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    const uint32_t subresource_count = image->getSubresourceCount();
    if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        for (uint32_t i = 0; i < subresource_count; ++i)
        {
            TransitionImage(image, i, new_state);
        }
        return;
    }

    D3D12_RESOURCE_STATES old_state = image->getState(subresource);
    if (old_state == new_state)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = image->getResource();
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = old_state;
    barrier.Transition.StateAfter = new_state;
    m_CommandLists[m_CurrentFrameIndex]->ResourceBarrier(1, &barrier);
    image->setState(subresource, new_state);
}

void DX12RHI::TransitionImage(DX12Image* image,
                              D3D12_RESOURCE_STATES new_state)
{
    TransitionImage(image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, new_state);
}

void DX12RHI::TransitionSwapchainBuffer(uint32_t back_buffer_index, D3D12_RESOURCE_STATES new_state)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || back_buffer_index >= k_max_frames_in_flight ||
        !m_RenderTargets[back_buffer_index])
    {
        return;
    }

    D3D12_RESOURCE_STATES& tracked_state = m_SwapchainResourceStates[back_buffer_index];
    if (tracked_state == new_state)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_RenderTargets[back_buffer_index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tracked_state;
    barrier.Transition.StateAfter = new_state;
    m_CommandLists[m_CurrentFrameIndex]->ResourceBarrier(1, &barrier);
    tracked_state = new_state;

    if (back_buffer_index == m_CurrentBackBufferIndex)
    {
        m_SwapchainSurfaceState = (new_state == D3D12_RESOURCE_STATE_PRESENT) ? SwapchainSurfaceState::Present : SwapchainSurfaceState::RenderTarget;
    }
}

bool DX12RHI::CreateDynamicBufferGpuHandle(const DX12DescriptorBufferBinding& binding,
                                           uint32_t dynamic_offset,
                                           D3D12_GPU_DESCRIPTOR_HANDLE& out_gpu_handle)
{
    if (binding.buffer == nullptr || binding.buffer->getResource() == nullptr || m_Device == nullptr)
    {
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle {};
    if (!AllocateCbvSrvUavDescriptor(cpu_handle, out_gpu_handle))
    {
        return false;
    }

    const RHIDeviceSize effective_offset = binding.offset + dynamic_offset;
    const RHIDeviceSize buffer_range = binding.range == RHI_WHOLE_SIZE ? binding.buffer->getSize() - effective_offset : binding.range;

    if (binding.type == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
        binding.type == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
    {
        constexpr RHIDeviceSize k_cbv_alignment = 256u;
        const RHIDeviceSize aligned_offset = effective_offset - (effective_offset % k_cbv_alignment);
        if (aligned_offset != effective_offset)
        {
            LOG_WARNING(ZRender,
                        "DX12 CBV dynamic offset {} is not 256-byte aligned; binding aligned slot {}",
                        static_cast<uint64_t>(effective_offset),
                        static_cast<uint64_t>(aligned_offset));
        }

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
        cbv_desc.BufferLocation = binding.buffer->getResource()->GetGPUVirtualAddress() + aligned_offset;
        cbv_desc.SizeInBytes = AlignTo(static_cast<uint32_t>(buffer_range), static_cast<uint32_t>(k_cbv_alignment));
        m_Device->CreateConstantBufferView(&cbv_desc, cpu_handle);
        return true;
    }

    if (binding.type == RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
        binding.type == RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
    {
        constexpr UINT kRenderMeshInstanceStride = 80u;
        const UINT stride =
            (buffer_range >= kRenderMeshInstanceStride && (buffer_range % kRenderMeshInstanceStride) == 0) ? kRenderMeshInstanceStride : 4u;

        RHIDeviceSize aligned_offset = effective_offset;
        if (stride == kRenderMeshInstanceStride)
        {
            const RHIDeviceSize remainder = effective_offset % stride;
            if (remainder != 0)
            {
                aligned_offset = effective_offset + (stride - remainder);
                LOG_ERROR(ZRender,
                          "DX12 structured-buffer dynamic offset {} is not {}-byte aligned; GPU reads slot {} but "
                          "CPU must write the same aligned slot",
                          static_cast<uint64_t>(effective_offset),
                          kRenderMeshInstanceStride,
                          static_cast<uint64_t>(aligned_offset));
            }
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.FirstElement = static_cast<UINT>(aligned_offset / stride);
        srv_desc.Buffer.NumElements = static_cast<UINT>(std::max<RHIDeviceSize>(buffer_range / stride, 1));
        srv_desc.Buffer.StructureByteStride = stride;
        srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        m_Device->CreateShaderResourceView(binding.buffer->getResource(), &srv_desc, cpu_handle);
        return true;
    }

    return false;
}

bool DX12RHI::EnsureCbvSrvUavDescriptorHeap()
{
    if (m_CbvSrvUavHeap)
    {
        return true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = m_CbvSrvUavDescriptorCapacity;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!CheckDX12(m_Device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_CbvSrvUavHeap)),
                   "Create CBV/SRV/UAV descriptor heap failed"))
    {
        return false;
    }

    m_CbvSrvUavDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT& next_descriptor : m_NextCbvSrvUavDescriptorPerFrame)
    {
        next_descriptor = 0;
    }
    return true;
}

bool DX12RHI::EnsureSamplerDescriptorHeap()
{
    if (m_SamplerHeap)
    {
        return true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = m_SamplerDescriptorCapacity;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!CheckDX12(m_Device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_SamplerHeap)),
                   "Create sampler descriptor heap failed"))
    {
        return false;
    }

    m_SamplerDescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    m_NextSamplerDescriptor = 0;
    return true;
}

bool DX12RHI::AllocateCbvSrvUavDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle,
                                          D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle)
{
    if (!EnsureCbvSrvUavDescriptorHeap())
    {
        LOG_ERROR(ZRender, "DX12 CBV/SRV/UAV descriptor heap exhausted");
        return false;
    }

    const UINT per_frame_capacity = std::max<UINT>(1u, m_CbvSrvUavDescriptorCapacity / k_max_frames_in_flight);
    const UINT frame_slot = static_cast<UINT>(m_CurrentFrameIndex);
    const UINT frame_base = frame_slot * per_frame_capacity;
    UINT& next_descriptor = m_NextCbvSrvUavDescriptorPerFrame[frame_slot];
    if (next_descriptor >= per_frame_capacity)
    {
        LOG_ERROR(ZRender,
                  "DX12 CBV/SRV/UAV descriptor heap exhausted (frame slot {}, used {}/{} per frame)",
                  frame_slot,
                  next_descriptor,
                  per_frame_capacity);
        return false;
    }

    const UINT global_index = frame_base + next_descriptor;
    ++next_descriptor;

    cpu_handle = m_CbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
    gpu_handle = m_CbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(global_index) * m_CbvSrvUavDescriptorSize;
    gpu_handle.ptr += static_cast<UINT64>(global_index) * m_CbvSrvUavDescriptorSize;
    return true;
}

bool DX12RHI::AllocateSamplerDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle,
                                        D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle)
{
    if (!EnsureSamplerDescriptorHeap() || m_NextSamplerDescriptor >= m_SamplerDescriptorCapacity)
    {
        LOG_ERROR(ZRender, "DX12 sampler descriptor heap exhausted");
        return false;
    }

    cpu_handle = m_SamplerHeap->GetCPUDescriptorHandleForHeapStart();
    gpu_handle = m_SamplerHeap->GetGPUDescriptorHandleForHeapStart();
    cpu_handle.ptr += static_cast<SIZE_T>(m_NextSamplerDescriptor) * m_SamplerDescriptorSize;
    gpu_handle.ptr += static_cast<UINT64>(m_NextSamplerDescriptor) * m_SamplerDescriptorSize;
    ++m_NextSamplerDescriptor;
    return true;
}

ID3D12DescriptorHeap* DX12RHI::GetCbvSrvUavDescriptorHeap()
{
    EnsureCbvSrvUavDescriptorHeap();
    return m_CbvSrvUavHeap.Get();
}

ID3D12DescriptorHeap* DX12RHI::GetSamplerDescriptorHeap()
{
    EnsureSamplerDescriptorHeap();
    return m_SamplerHeap.Get();
}

bool DX12RHI::SetBindlessDescriptorHeaps()
{
    if (!m_BindlessSupported || !m_BindlessTextureManager)
    {
        return false;
    }

    auto* bindless_mgr = static_cast<DX12BindlessTextureManager*>(m_BindlessTextureManager.get());
    ID3D12DescriptorHeap* heaps[2] = {};
    UINT heap_count = 0;
    heaps[heap_count++] = bindless_mgr->getDescriptorHeap();
    if (m_SamplerHeap)
    {
        heaps[heap_count++] = m_SamplerHeap.Get();
    }
    m_CommandLists[m_CurrentFrameIndex]->SetDescriptorHeaps(heap_count, heaps);
    return true;
}

void DX12RHI::CmdSetRootConstantBufferView(RHIPipelineBindPoint bind_point,
                                           uint32_t root_param_index,
                                           D3D12_GPU_VIRTUAL_ADDRESS gpu_va)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }
    if (bind_point == RHI_PIPELINE_BIND_POINT_COMPUTE)
    {
        m_CommandLists[m_CurrentFrameIndex]->SetComputeRootConstantBufferView(root_param_index, gpu_va);
    }
    else
    {
        m_CommandLists[m_CurrentFrameIndex]->SetGraphicsRootConstantBufferView(root_param_index, gpu_va);
    }
}

DXGI_FORMAT DX12RHI::GetSwapchainDXGIFormat() const
{
    return ToDX12TypelessAwareRTVFormat(m_SwapchainImageFormat);
}

DXGI_FORMAT DX12RHI::GetUiLayerRtvFormat()
{
    return ToDX12Format(RHI_FORMAT_R16G16B16A16_SFLOAT);
}

void DX12RHI::BindUiLayerRenderTarget(RHIImageView* color_view)
{
    ID3D12GraphicsCommandList* command_list = getCurrentCommandList();
    if (command_list == nullptr || color_view == nullptr)
    {
        return;
    }

    auto* dx12_view = static_cast<DX12ImageView*>(color_view);
    if (!dx12_view->hasRenderTargetHandle())
    {
        LOG_WARNING(ZRender, "DX12RHI::BindUiLayerRenderTarget: view has no RTV handle");
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12_view->getRenderTargetHandle();
    command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    EnsureActiveRenderPassViewport();
}

void DX12RHI::EnsureActiveRenderPassViewport()
{
    ID3D12GraphicsCommandList* command_list = getCurrentCommandList();
    if (command_list == nullptr)
    {
        return;
    }

    const uint32_t width = m_SwapchainExtent.width;
    const uint32_t height = m_SwapchainExtent.height;
    if (width == 0 || height == 0)
    {
        return;
    }

    D3D12_VIEWPORT viewport {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    command_list->RSSetViewports(1, &viewport);

    D3D12_RECT scissor {};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);
    command_list->RSSetScissorRects(1, &scissor);
}

void DX12RHI::BeginSwapchainOverlayDraw()
{
    ID3D12GraphicsCommandList* command_list = getCurrentCommandList();
    if (command_list == nullptr || !m_RenderTargets[m_CurrentBackBufferIndex])
    {
        return;
    }

    if (m_SwapchainSurfaceState == SwapchainSurfaceState::Present)
    {
        TransitionSwapchainBuffer(m_CurrentBackBufferIndex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    RestoreSwapchainRenderState();
}

void DX12RHI::RestoreSwapchainRenderState()
{
    ID3D12GraphicsCommandList* command_list = getCurrentCommandList();
    if (command_list == nullptr || !m_RtvHeap || !m_RenderTargets[m_CurrentBackBufferIndex])
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle {};
    if (!TryGetSwapchainBackBufferRtv(m_CurrentBackBufferIndex, rtv_handle))
    {
        return;
    }
    command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    D3D12_VIEWPORT viewport {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_SwapchainExtent.width);
    viewport.Height = static_cast<float>(m_SwapchainExtent.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    command_list->RSSetViewports(1, &viewport);

    D3D12_RECT scissor {};
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(m_SwapchainExtent.width);
    scissor.bottom = static_cast<LONG>(m_SwapchainExtent.height);
    command_list->RSSetScissorRects(1, &scissor);
}

bool DX12RHI::AllocateImGuiSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle,
                                         D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle)
{
    // PR-DX1: ImGui SRVs now live in the bindless heap so that only
    // one CBV/SRV/UAV heap needs to be bound at draw time. The
    // bindless heap is the single active shader-visible heap; the
    // legacy m_CbvSrvUavHeap is retained only as a CPU-side
    // staging area for view creation (non-shader-visible writes).
    if (m_BindlessSupported && m_BindlessTextureManager)
    {
        auto* mgr = static_cast<DX12BindlessTextureManager*>(m_BindlessTextureManager.get());
        uint32_t slot = mgr->AllocateRawSlot();
        if (slot != RHIBindlessTextureManager::kInvalidBindlessIndex)
        {
            cpu_handle = mgr->GetCpuHandleAt(slot);
            gpu_handle = mgr->GetGpuHandleAt(slot);
            return true;
        }
        LOG_WARNING(ZRender, "DX12RHI::allocateImGuiSrvDescriptor: bindless heap full, falling back to legacy heap");
    }
    // Fallback: legacy m_CbvSrvUavHeap (for hardware without
    // bindless support or if the bindless heap is exhausted).
    return AllocateCbvSrvUavDescriptor(cpu_handle, gpu_handle);
}

namespace
{
    void WriteSampledImageSrv(ID3D12Device* device,
                              DX12Image* image,
                              DXGI_FORMAT dxgi_format,
                              RHIImageViewType view_type,
                              uint32_t layout_count,
                              uint32_t miplevels,
                              D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle)
    {
        if (device == nullptr || image == nullptr || cpu_handle.ptr == 0)
        {
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = dxgi_format;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        switch (view_type)
        {
            case RHI_IMAGE_VIEW_TYPE_CUBE:
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srv_desc.TextureCube.MostDetailedMip = 0;
                srv_desc.TextureCube.MipLevels = std::max<uint32_t>(miplevels, 1);
                srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
                break;
            case RHI_IMAGE_VIEW_TYPE_2D_ARRAY:
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv_desc.Texture2DArray.MostDetailedMip = 0;
                srv_desc.Texture2DArray.MipLevels = std::max<uint32_t>(miplevels, 1);
                srv_desc.Texture2DArray.FirstArraySlice = 0;
                srv_desc.Texture2DArray.ArraySize = std::max<uint32_t>(layout_count, 1);
                break;
            case RHI_IMAGE_VIEW_TYPE_2D:
            default:
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv_desc.Texture2D.MostDetailedMip = 0;
                srv_desc.Texture2D.MipLevels = std::max<uint32_t>(miplevels, 1);
                srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
                break;
        }

        device->CreateShaderResourceView(image->getResource(), &srv_desc, cpu_handle);
    }
}  // namespace

void DX12RHI::EnsureShaderVisibleImageView(RHIImageView* image_view)
{
    if (!m_BindlessSupported || !m_BindlessTextureManager || image_view == nullptr || !m_Device)
    {
        return;
    }

    auto* dx12_view = static_cast<DX12ImageView*>(image_view);
    DX12Image* dx12_image = dx12_view->getImage();
    if (dx12_image == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle {};
    if (!AllocateImGuiSrvDescriptor(cpu_handle, gpu_handle))
    {
        LOG_WARNING(ZRender, "DX12RHI::EnsureShaderVisibleImageView: bindless SRV allocation failed");
        return;
    }

    WriteSampledImageSrv(m_Device.Get(),
                         dx12_image,
                         dx12_view->getFormat(),
                         dx12_view->getViewType(),
                         dx12_view->getArrayCount(),
                         dx12_view->getMipLevels(),
                         cpu_handle);
    dx12_view->setResource(dx12_image,
                           dx12_view->getFormat(),
                           cpu_handle,
                           gpu_handle,
                           dx12_view->getViewType(),
                           dx12_view->getArrayCount(),
                           dx12_view->getMipLevels());
}

bool DX12RHI::EnsureDispatchIndirectSignature()
{
    if (m_DispatchIndirectSignature)
    {
        return true;
    }
    if (!m_Device)
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument_desc = {};
    argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &argument_desc;

    return CheckDX12(m_Device->CreateCommandSignature(&signature_desc,
                                                      nullptr,
                                                      IID_PPV_ARGS(&m_DispatchIndirectSignature)),
                     "Create dispatch indirect command signature failed");
}

bool DX12RHI::IsDeviceRemoved(const char* context) const
{
    if (!m_Device)
    {
        return true;
    }

    const HRESULT device_reason = m_Device->GetDeviceRemovedReason();
    if (device_reason == S_OK)
    {
        return false;
    }

    if (IsActualDx12DeviceRemoval(device_reason))
    {
        LOG_ERROR(ZRender,
                  "DX12 device removed{}: HRESULT=0x{:08X}",
                  context != nullptr ? context : "",
                  static_cast<unsigned int>(device_reason));
        return true;
    }

    // GetDeviceRemovedReason can return DXGI_ERROR_INVALID_CALL on a healthy device.
    static bool s_warned_non_loss_reason = false;
    if (!s_warned_non_loss_reason)
    {
        s_warned_non_loss_reason = true;
        LOG_WARNING(ZRender,
                    "DX12 GetDeviceRemovedReason returned non-loss HRESULT=0x{:08X}{}",
                    static_cast<unsigned int>(device_reason),
                    context != nullptr ? context : "");
    }
    return false;
}

bool DX12RHI::ExecuteDedicatedUploadCommands(
    const std::function<void(ID3D12GraphicsCommandList*)>& record_commands)
{
    if (!m_Device || !m_CommandQueue || !record_commands)
    {
        return false;
    }
    if (IsDeviceRemoved(" before dedicated texture upload"))
    {
        return false;
    }

    ComPtr<ID3D12CommandAllocator> upload_allocator;
    if (!CheckDX12(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&upload_allocator)),
                   "DX12 dedicated upload: CreateCommandAllocator failed"))
    {
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> upload_command_list;
    if (!CheckDX12(m_Device->CreateCommandList(0,
                                               D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               upload_allocator.Get(),
                                               nullptr,
                                               IID_PPV_ARGS(&upload_command_list)),
                   "DX12 dedicated upload: CreateCommandList failed"))
    {
        return false;
    }

    // CreateCommandList returns the list in the recording state (same as bindless
    // placeholder upload). Reset() here would return E_FAIL before the first Close().
    record_commands(upload_command_list.Get());

    if (!CheckDX12(upload_command_list->Close(), "DX12 dedicated upload: command list Close failed"))
    {
        return false;
    }

    ID3D12CommandList* command_lists[] = {upload_command_list.Get()};
    m_CommandQueue->ExecuteCommandLists(1, command_lists);

    ComPtr<ID3D12Fence> upload_fence;
    if (!CheckDX12(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&upload_fence)),
                   "DX12 dedicated upload: CreateFence failed"))
    {
        return false;
    }

    HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr)
    {
        LOG_ERROR(ZRender, "DX12 dedicated upload: CreateEvent failed");
        return false;
    }

    if (!CheckDX12(m_CommandQueue->Signal(upload_fence.Get(), 1), "DX12 dedicated upload: Signal failed"))
    {
        CloseHandle(fence_event);
        return false;
    }

    if (upload_fence->GetCompletedValue() < 1)
    {
        if (!CheckDX12(upload_fence->SetEventOnCompletion(1, fence_event),
                       "DX12 dedicated upload: SetEventOnCompletion failed"))
        {
            CloseHandle(fence_event);
            return false;
        }
        constexpr DWORD k_fence_timeout_ms = 30000;
        if (!ZEngine::DX12HostSync::WaitForFenceValue(upload_fence.Get(),
                                                      1,
                                                      fence_event,
                                                      k_fence_timeout_ms,
                                                      "DX12 dedicated upload"))
        {
            CloseHandle(fence_event);
            return false;
        }
    }
    CloseHandle(fence_event);

    return !IsDeviceRemoved(" after dedicated texture upload");
}

bool DX12RHI::UploadTextureData(DX12Image* image,
                                const void* pixels,
                                uint32_t width,
                                uint32_t height,
                                uint32_t array_layer,
                                uint32_t mip_level,
                                uint32_t bytes_per_pixel)
{
    if (!image || !pixels || !m_Device || !m_CommandQueue)
    {
        return false;
    }

    const uint32_t source_bpp = bytes_per_pixel;
    uint32_t upload_bpp = bytes_per_pixel;
    if (source_bpp == 16)
    {
        upload_bpp = 8;
    }

    TextureUploadStaging staging {};
    if (!CreateTextureUploadStaging(m_Device.Get(),
                                    image->getResource(),
                                    pixels,
                                    width,
                                    height,
                                    array_layer,
                                    mip_level,
                                    image->getMipLevels(),
                                    image->getArrayLayers(),
                                    source_bpp,
                                    upload_bpp,
                                    staging))
    {
        return false;
    }

    return ExecuteDedicatedUploadCommands([&](ID3D12GraphicsCommandList* command_list) {
        TransitionImageOnList(image, staging.subresource_index, D3D12_RESOURCE_STATE_COPY_DEST, command_list);
        RecordTextureCopy(command_list, image->getResource(), staging);
        TransitionImageOnList(image,
                              staging.subresource_index,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              command_list);
    });
}

bool DX12RHI::UploadCubeMapMip0(DX12Image* image,
                                const std::array<const void*, 6>& face_pixels,
                                uint32_t width,
                                uint32_t height,
                                uint32_t bytes_per_pixel)
{
    return UploadCubeMapAllMips(image, face_pixels, width, height, 1u, bytes_per_pixel);
}

bool DX12RHI::UploadCubeMapAllMips(DX12Image* image,
                                   const std::array<const void*, 6>& face_pixels,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t mip_levels,
                                   uint32_t bytes_per_pixel)
{
    if (!image || !m_Device || !m_CommandQueue || width == 0 || height == 0 || mip_levels == 0)
    {
        return false;
    }

    const uint32_t source_bpp = bytes_per_pixel;
    uint32_t upload_bpp = bytes_per_pixel;
    if (source_bpp == 16)
    {
        upload_bpp = 8;
    }

    std::vector<TextureUploadStaging> stagings;
    stagings.reserve(6);
    for (uint32_t face = 0; face < 6; ++face)
    {
        if (face_pixels[face] == nullptr)
        {
            continue;
        }

        TextureUploadStaging staging {};
        if (!CreateTextureUploadStaging(m_Device.Get(),
                                        image->getResource(),
                                        face_pixels[face],
                                        width,
                                        height,
                                        face,
                                        0,
                                        image->getMipLevels(),
                                        image->getArrayLayers(),
                                        source_bpp,
                                        upload_bpp,
                                        staging))
        {
            return false;
        }
        stagings.push_back(std::move(staging));
    }

    if (stagings.empty())
    {
        return true;
    }

    const bool gpu_mipgen = mip_levels > 1;
    if (gpu_mipgen &&
        !m_CubemapMipGenerator.EnsureInitialized(m_Device.Get(), image->getFormat()))
    {
        LOG_ERROR(ZRender,
                  "DX12 UploadCubeMapAllMips: GPU mipgen init failed (format={})",
                  static_cast<uint32_t>(image->getFormat()));
        return false;
    }
    if (gpu_mipgen)
    {
        m_IblMipGenReady = true;
        m_IblMipGenFormat = image->getFormat();
    }

    bool record_ok = true;
    const bool submitted = ExecuteDedicatedUploadCommands([&](ID3D12GraphicsCommandList* command_list) {
        for (const TextureUploadStaging& staging : stagings)
        {
            TransitionImageOnList(image, staging.subresource_index, D3D12_RESOURCE_STATE_COPY_DEST, command_list);
            RecordTextureCopy(command_list, image->getResource(), staging);
            TransitionImageOnList(image,
                                  staging.subresource_index,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  command_list);
        }

        if (gpu_mipgen && !m_CubemapMipGenerator.GenerateMipChain(command_list, image))
        {
            LOG_ERROR(ZRender, "DX12 UploadCubeMapAllMips: GPU mipgen failed");
            record_ok = false;
        }
    });
    return submitted && record_ok;
}

bool DX12RHI::Initialize()
{
    // Wire up the engine-wide default DXIL on-disk cache directory before
    // any DX12ShaderCompiler instance is constructed (DX12RHI itself uses
    // lazy construction in createShaderModuleFromFile/Source; preview /
    // inspector own their own instances). Pointing the default at
    // <project>/Intermediate/Shaders/ means every default-constructed
    // compiler will pick up the same cache root automatically -- no
    // call-site changes needed.
    //
    // ProjectInfo may be unavailable on the launcher / "no project loaded"
    // screen; in that case we simply leave caching disabled, which is the
    // pre-existing behaviour.
    if (auto project_info = GET_SYSTEM(ProjectInfo))
    {
        const std::filesystem::path interm_shaders = project_info->GetIntermediateShadersRoot();
        if (!interm_shaders.empty())
        {
            DX12ShaderCompiler::SetDefaultCacheDirectory(interm_shaders);
        }
    }

    std::array<int, 2> framebuffer_size = GET_SYSTEM(WindowSystem)->GetFramebufferSize();
    framebuffer_size[0] = std::max(framebuffer_size[0], 1);
    framebuffer_size[1] = std::max(framebuffer_size[1], 1);

    for (int i = 0; i < std::size(m_Viewports); i++)
    {
        m_Viewports[i] = {0.0f,
                          0.0f,
                          static_cast<float>(framebuffer_size[0]),
                          static_cast<float>(framebuffer_size[1]),
                          0.0f,
                          1.0f};
    }

    for (int i = 0; i < std::size(m_Scissors); i++)
    {
        m_Scissors[i] = {{0, 0},
                         {static_cast<uint32_t>(framebuffer_size[0]), static_cast<uint32_t>(framebuffer_size[1])}};
    }

    m_SwapchainExtent = {static_cast<uint32_t>(framebuffer_size[0]), static_cast<uint32_t>(framebuffer_size[1])};
    m_SwapchainImageFormat = RHI_FORMAT_R8G8B8A8_UNORM;
    m_DepthImageFormat = RHI_FORMAT_D32_SFLOAT;

    // Enable DX12 debug layer BEFORE creating any DX12 objects.
    {
        ID3D12Debug* debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&debug))))
        {
            debug->EnableDebugLayer();
            debug->Release();
            LOG_INFO(ZRender, "DX12RHI: DX12 debug layer enabled");
        }
        else
        {
            LOG_WARNING(ZRender, "DX12RHI: failed to enable DX12 debug layer");
        }
    }
    {
    }

    if (!CheckDX12(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_DxgiFactory)), "CreateDXGIFactory2 failed"))
    {
        return false;
    }

    if (!CheckDX12(m_DxgiFactory->EnumAdapterByGpuPreference(
                       0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&m_Adapter)),
                   "EnumAdapterByGpuPreference failed"))
    {
        return false;
    }

    if (!CheckDX12(D3D12CreateDevice(m_Adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device)),
                   "D3D12CreateDevice failed"))
    {
        return false;
    }
    LOG_INFO(ZRender, "DX12RHI: D3D12 device created");

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!CheckDX12(m_Device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_CommandQueue)),
                   "CreateCommandQueue failed"))
    {
        return false;
    }

    m_GraphicsQueue = new RHIQueue();
    m_ComputeQueue = m_GraphicsQueue;

    CreateSwapchain();
    if (!m_Swapchain)
    {
        return false;
    }
    LOG_INFO(ZRender, "DX12RHI: swapchain ready ({}x{})", m_SwapchainExtent.width, m_SwapchainExtent.height);

    CreateSwapchainImageViews();
    CreateFramebufferImageAndView();
    LOG_INFO(ZRender, "DX12RHI: swapchain RTVs + depth buffer ready");

    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (!CheckDX12(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&m_CommandAllocators[i])),
                       "CreateCommandAllocator failed"))
        {
            return false;
        }

        if (!CheckDX12(m_Device->CreateCommandList(0,
                                                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   m_CommandAllocators[i].Get(),
                                                   nullptr,
                                                   IID_PPV_ARGS(&m_CommandLists[i])),
                       "CreateCommandList failed"))
        {
            return false;
        }
        m_CommandLists[i]->Close();

        if (!CheckDX12(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fences[i])),
                       "CreateFence failed"))
        {
            return false;
        }

        m_CommandBuffers[i] = new RHICommandBuffer();
        m_RhiIsFrameInFlightFences[i] = new RHIFence();
        m_ImageAvailableForTexturescopySemaphores[i] = nullptr;
        m_FenceValues[i] = 0;
    }

    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_FenceEvent == nullptr)
    {
        LOG_ERROR(ZRender, "CreateEvent for DX12 fence failed");
        return false;
    }

    // -----------------------------------------------------------------
    // PR4: probe SM 6.6 + Resource Binding Tier and bring up the
    // bindless SRV table.
    //
    // Requirements (all must hold):
    //   - HighestShaderModel >= D3D_SHADER_MODEL_6_6 (so HLSL can use
    //     `ResourceDescriptorHeap[NonUniformResourceIndex(idx)]`
    //     without a root-signature descriptor table).
    //   - ResourceBindingTier >= TIER_2 (TIER_1 caps SRV-per-table at
    //     128, well below any useful bindless capacity; TIER_3 lifts
    //     the cap entirely but TIER_2 is sufficient because we only
    //     read via dynamic indexing into a single heap).
    //
    // Capacity defaults to 8192 SRVs -- comfortably above scene-scale
    // texture counts on PC, and small enough that the dedicated heap
    // fits in a few MB. We do NOT clamp against any driver-reported
    // maximum because TIER_2/3 already guarantees >=1M descriptors per
    // shader-visible heap. If a driver later disagrees,
    // DX12BindlessTextureManager::Initialize() will surface the
    // CreateDescriptorHeap failure and the path demotes cleanly.
    // -----------------------------------------------------------------
    {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        {
            m_MaxSupportedShaderModel = sm.HighestShaderModel;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
        if (SUCCEEDED(m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
        {
            m_ResourceBindingTier = opts.ResourceBindingTier;
        }

        const bool sm66_ok = (m_MaxSupportedShaderModel >= D3D_SHADER_MODEL_6_6);
        const bool tier_ok = (m_ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2);
        if (sm66_ok && tier_ok)
        {
            LOG_INFO(ZRender, "DX12RHI: bringing up bindless texture manager...");
            constexpr uint32_t kBindlessCapacity = 8192u;
            m_BindlessTextureManager = std::make_unique<DX12BindlessTextureManager>();
            // PR5a: manager now owns its own slot-0 placeholder
            // upload (1x1 white), so it needs the command queue too.
            if (m_BindlessTextureManager->Initialize(m_Device.Get(),
                                                     m_CommandQueue.Get(),
                                                     kBindlessCapacity))
            {
                m_BindlessSupported = true;
                m_MaxBindlessSampledImages = kBindlessCapacity;
                m_MaxBindlessStorageBuffers = 0;  // not implemented yet on DX12 (PR6+)
                LOG_INFO(ZRender,
                         "DX12 bindless: ENABLED (SM {:#x}, ResourceBindingTier {}, capacity {})",
                         static_cast<unsigned int>(m_MaxSupportedShaderModel),
                         static_cast<int>(m_ResourceBindingTier),
                         kBindlessCapacity);
            }
            else
            {
                m_BindlessTextureManager.reset();
                LOG_WARNING(ZRender,
                            "DX12 bindless manager init failed -- demoting to legacy descriptor path");
            }
        }
        else
        {
            LOG_INFO(ZRender,
                     "DX12 bindless: DISABLED (SM {:#x}{}, ResourceBindingTier {}{})",
                     static_cast<unsigned int>(m_MaxSupportedShaderModel),
                     sm66_ok ? "" : " <6.6",
                     static_cast<int>(m_ResourceBindingTier),
                     tier_ok ? "" : " <Tier2");
        }
    }

    PrepareContext();
    LOG_INFO(ZRender, "DX12RHI initialized with minimal clear/present path");
    return true;
}

void DX12RHI::Shutdown()
{
    clear();
}

void DX12RHI::PrepareContext()
{
    if (m_Swapchain)
    {
        m_CurrentBackBufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
        m_CurrentFrameIndex = static_cast<uint8_t>(m_CurrentBackBufferIndex % k_max_frames_in_flight);
    }
    m_CurrentCommandBuffer = m_CommandBuffers[m_CurrentFrameIndex];
}

bool DX12RHI::IsPointLightShadowEnabled()
{
    // Legacy name: gates mesh shadow-map draws (directional + point) on all backends.
    return true;
}

bool DX12RHI::AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                     RHICommandBuffer*& pCommandBuffers)
{
    // TODO: Implement DX12 allocateCommandBuffers
    return false;
}

bool DX12RHI::AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                     RHIDescriptorSet*& pDescriptorSets)
{
    pDescriptorSets = nullptr;
    if (pAllocateInfo == nullptr || pAllocateInfo->descriptorSetCount == 0 || pAllocateInfo->pSetLayouts == nullptr)
    {
        return false;
    }

    if (pAllocateInfo->descriptorSetCount != 1)
    {
        LOG_ERROR(ZRender, "DX12 allocateDescriptorSets currently supports one descriptor set per call");
        return false;
    }

    DX12DescriptorSet* descriptor_set = new DX12DescriptorSet();
    descriptor_set->setLayout(static_cast<DX12DescriptorSetLayout*>(
        const_cast<RHIDescriptorSetLayout*>(pAllocateInfo->pSetLayouts[0])));
    pDescriptorSets = descriptor_set;
    return true;
}

void DX12RHI::CreateSwapchain()
{
    if (!m_DxgiFactory || !m_CommandQueue)
    {
        LOG_ERROR(ZRender, "DX12 createSwapchain called before factory/queue initialization");
        return;
    }

    HWND hwnd = static_cast<HWND>(GET_SYSTEM(WindowSystem)->GetNativeWindowHandle());
    if (hwnd == nullptr)
    {
        LOG_ERROR(ZRender, "GetNativeWindowHandle failed for DX12 swapchain");
        return;
    }

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
    swapchain_desc.Width = m_SwapchainExtent.width;
    swapchain_desc.Height = m_SwapchainExtent.height;
    swapchain_desc.Format = ToDX12SwapchainFormat(m_SwapchainImageFormat);
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = k_max_frames_in_flight;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapchain;
    if (!CheckDX12(m_DxgiFactory->CreateSwapChainForHwnd(
                       m_CommandQueue.Get(), hwnd, &swapchain_desc, nullptr, nullptr, swapchain.GetAddressOf()),
                   "CreateSwapChainForHwnd failed"))
    {
        return;
    }

    m_DxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    if (!CheckDX12(swapchain.As(&m_Swapchain), "Query IDXGISwapChain3 failed"))
    {
        return;
    }

    m_CurrentBackBufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
    m_CurrentFrameIndex = static_cast<uint8_t>(m_CurrentBackBufferIndex % k_max_frames_in_flight);
}

void DX12RHI::WaitForGpuIdle()
{
    if (m_FenceEvent == nullptr)
    {
        return;
    }

    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (m_Fences[i] && m_FenceValues[i] > 0 && m_Fences[i]->GetCompletedValue() < m_FenceValues[i])
        {
            if (CheckDX12(m_Fences[i]->SetEventOnCompletion(m_FenceValues[i], m_FenceEvent),
                          "DX12 SetEventOnCompletion (WaitForGpuIdle) failed"))
            {
                WaitForSingleObject(m_FenceEvent, INFINITE);
            }
        }
    }
}

void DX12RHI::RecreateSwapchain()
{
    if (!m_Device || !m_Swapchain)
    {
        return;
    }

    // UE parity (FD3D12Viewport::Resize): drain ALL in-flight frames before touching
    // the swapchain, not just the current slot. DXGI's ResizeBuffers requires that no
    // GPU work still references any backbuffer; WaitForFences() only covers
    // m_CurrentFrameIndex, which is why a maximize used to fault the device.
    WaitForGpuIdle();

    std::array<int, 2> framebuffer_size = GET_SYSTEM(WindowSystem)->GetFramebufferSize();
    framebuffer_size[0] = std::max(framebuffer_size[0], 1);
    framebuffer_size[1] = std::max(framebuffer_size[1], 1);
    m_SwapchainExtent = {static_cast<uint32_t>(framebuffer_size[0]),
                         static_cast<uint32_t>(framebuffer_size[1])};

    for (auto& render_target : m_RenderTargets)
    {
        render_target.Reset();
    }
    m_HasSwapchainRtvReuse = false;
    m_SwapchainRtvReuse.fill({});
    for (uint8_t i = 0; i < m_SwapchainImageviews.size() && i < k_max_frames_in_flight; ++i)
    {
        const auto* dx12_view = static_cast<const DX12ImageView*>(m_SwapchainImageviews[i]);
        if (dx12_view != nullptr && dx12_view->hasRenderTargetHandle())
        {
            m_SwapchainRtvReuse[i] = dx12_view->getRenderTargetHandle();
            m_HasSwapchainRtvReuse = true;
        }
    }

    for (RHIImageView*& image_view : m_SwapchainImageviews)
    {
        RHI_DELETE_PTR(image_view);
    }
    m_SwapchainImageviews.clear();

    if (!CheckDX12(m_Swapchain->ResizeBuffers(k_max_frames_in_flight,
                                              m_SwapchainExtent.width,
                                              m_SwapchainExtent.height,
                                              ToDX12SwapchainFormat(m_SwapchainImageFormat),
                                              0),
                   "DX12 ResizeBuffers failed"))
    {
        return;
    }

    m_CurrentBackBufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
    m_CurrentFrameIndex = static_cast<uint8_t>(m_CurrentBackBufferIndex % k_max_frames_in_flight);
    CreateSwapchainImageViews();

    // CreateSwapchainImageViews() resets every per-buffer state to PRESENT (line below sets
    // m_SwapchainResourceStates[i] = D3D12_RESOURCE_STATE_PRESENT). The aggregate tracker must
    // agree, otherwise the next PrepareBeforePass thinks the surface is still a RenderTarget and
    // skips the PRESENT->RT barrier on the fresh buffers, producing a black/garbage first frame.
    m_SwapchainSurfaceState = SwapchainSurfaceState::Present;

    // NOTE: the scene's G-buffer + its depth live in MainCameraFramebufferResources (recreated by
    // DX12MainCameraPass::UpdateAfterFramebufferRecreate via passUpdateAfterRecreateSwapchain).
    // The RHI-shared m_DepthImage is NOT bound by the DX12 scene path (PrepareBeforePass binds the
    // backbuffer with a null DSV), so we deliberately do NOT tear it down here.
}

void DX12RHI::NotifyWindowFocusGained()
{
    // Force a full swapchain recreate on next PrepareBeforePass. This recovers
    // from DXGI surface invalidation that occurs when Alt-Tabbing away with a
    // FLIP-model swapchain: Windows may internally reset the backbuffer
    // chain, causing subsequent Presents to show blank / stale content.
    m_SwapchainNeedsRecreate = true;
}

// ---------------------------------------------------------------------------
// Editor floating-panel surfaces (tear-off). See DX12RHI::CreateFloatingSurface
// header comment + the DX12FloatingSurface struct in DX12RHI.h.
// ---------------------------------------------------------------------------
bool DX12RHI::CreateFloatingSurfaceRtvs(DX12FloatingSurface* surface)
{
    if (surface == nullptr || surface->swapchain == nullptr || m_Device == nullptr)
    {
        return false;
    }
    if (surface->rtv_heap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.NumDescriptors = k_max_frames_in_flight;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (!CheckDX12(m_Device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&surface->rtv_heap)),
                       "DX12 floating surface RTV heap failed"))
        {
            return false;
        }
        surface->rtv_descriptor_size = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = surface->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (!CheckDX12(surface->swapchain->GetBuffer(i, IID_PPV_ARGS(&surface->render_targets[i])),
                       "DX12 floating surface GetBuffer failed"))
        {
            return false;
        }
        m_Device->CreateRenderTargetView(surface->render_targets[i].Get(), nullptr, rtv);
        surface->states[i] = D3D12_RESOURCE_STATE_PRESENT;
        rtv.ptr += surface->rtv_descriptor_size;
    }
    return true;
}

void DX12RHI::ReleaseFloatingSurfaceRtvs(DX12FloatingSurface* surface)
{
    if (surface == nullptr)
    {
        return;
    }
    for (auto& render_target : surface->render_targets)
    {
        render_target.Reset();
    }
}

DX12FloatingSurface* DX12RHI::CreateFloatingSurface(void* hwnd, uint32_t width, uint32_t height)
{
    if (m_Device == nullptr || m_CommandQueue == nullptr || m_DxgiFactory == nullptr || hwnd == nullptr)
    {
        LOG_ERROR(ZRender, "DX12 CreateFloatingSurface called before device init or with null HWND");
        return nullptr;
    }

    width = std::max<uint32_t>(width, 1u);
    height = std::max<uint32_t>(height, 1u);

    auto* surface = new DX12FloatingSurface();
    surface->hwnd = reinterpret_cast<HWND>(hwnd);
    surface->width = width;
    surface->height = height;

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
    swapchain_desc.Width = width;
    swapchain_desc.Height = height;
    swapchain_desc.Format = ToDX12SwapchainFormat(m_SwapchainImageFormat);
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = k_max_frames_in_flight;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapchain1;
    if (!CheckDX12(m_DxgiFactory->CreateSwapChainForHwnd(m_CommandQueue.Get(),
                                                         surface->hwnd,
                                                         &swapchain_desc,
                                                         nullptr,
                                                         nullptr,
                                                         swapchain1.GetAddressOf()),
                   "DX12 floating CreateSwapChainForHwnd failed"))
    {
        delete surface;
        return nullptr;
    }
    m_DxgiFactory->MakeWindowAssociation(surface->hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    if (!CheckDX12(swapchain1.As(&surface->swapchain), "DX12 floating QI IDXGISwapChain3 failed"))
    {
        delete surface;
        return nullptr;
    }
    if (!CreateFloatingSurfaceRtvs(surface))
    {
        surface->swapchain.Reset();
        delete surface;
        return nullptr;
    }
    surface->back_buffer_index = surface->swapchain->GetCurrentBackBufferIndex();
    return surface;
}

void DX12RHI::DestroyFloatingSurface(DX12FloatingSurface* surface)
{
    if (surface == nullptr)
    {
        return;
    }
    // Drop any pending present referencing this surface before its GPU resources go.
    m_PendingFloatingPresents.erase(
        std::remove(m_PendingFloatingPresents.begin(), m_PendingFloatingPresents.end(), surface),
        m_PendingFloatingPresents.end());
    // The frame command list may still reference this surface's back buffers; drain
    // all in-flight frames before releasing them (same constraint as ResizeBuffers).
    WaitForGpuIdle();
    ReleaseFloatingSurfaceRtvs(surface);
    surface->rtv_heap.Reset();
    surface->swapchain.Reset();
    delete surface;
}

void DX12RHI::ResizeFloatingSurface(DX12FloatingSurface* surface, uint32_t width, uint32_t height)
{
    if (surface == nullptr || surface->swapchain == nullptr)
    {
        return;
    }
    width = std::max<uint32_t>(width, 1u);
    height = std::max<uint32_t>(height, 1u);
    if (surface->width == width && surface->height == height)
    {
        return;
    }

    WaitForGpuIdle();
    ReleaseFloatingSurfaceRtvs(surface);
    if (!CheckDX12(surface->swapchain->ResizeBuffers(k_max_frames_in_flight,
                                                     width,
                                                     height,
                                                     ToDX12SwapchainFormat(m_SwapchainImageFormat),
                                                     0),
                   "DX12 floating ResizeBuffers failed"))
    {
        return;
    }
    surface->width = width;
    surface->height = height;
    CreateFloatingSurfaceRtvs(surface);
    surface->back_buffer_index = surface->swapchain->GetCurrentBackBufferIndex();
}

void DX12RHI::BeginFloatingSurfaceDraw(DX12FloatingSurface* surface, const float clear_color[4])
{
    ID3D12GraphicsCommandList* command_list = getCurrentCommandList();
    if (surface == nullptr || command_list == nullptr || surface->swapchain == nullptr ||
        surface->rtv_heap == nullptr)
    {
        return;
    }
    const UINT bb = surface->back_buffer_index;
    if (bb >= k_max_frames_in_flight || surface->render_targets[bb] == nullptr)
    {
        return;
    }

    if (surface->states[bb] != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = surface->render_targets[bb].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = surface->states[bb];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command_list->ResourceBarrier(1, &barrier);
        surface->states[bb] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = surface->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bb) * surface->rtv_descriptor_size;
    command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    command_list->ClearRenderTargetView(rtv, clear_color != nullptr ? clear_color : black, 0, nullptr);

    D3D12_VIEWPORT viewport {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(surface->width);
    viewport.Height = static_cast<float>(surface->height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    command_list->RSSetViewports(1, &viewport);

    D3D12_RECT scissor {0, 0, static_cast<LONG>(surface->width), static_cast<LONG>(surface->height)};
    command_list->RSSetScissorRects(1, &scissor);

    // Queue for present at frame end (dedup -- Begin may be reached more than once
    // if the editor re-records, e.g. a window-refresh redraw during a resize loop).
    if (std::find(m_PendingFloatingPresents.begin(), m_PendingFloatingPresents.end(), surface) ==
        m_PendingFloatingPresents.end())
    {
        m_PendingFloatingPresents.push_back(surface);
    }
}

void DX12RHI::EndFloatingSurfaceDraw(DX12FloatingSurface* surface)
{
    ID3D12GraphicsCommandList* command_list = getCurrentCommandList();
    if (surface == nullptr || command_list == nullptr)
    {
        return;
    }
    const UINT bb = surface->back_buffer_index;
    if (bb >= k_max_frames_in_flight || surface->render_targets[bb] == nullptr)
    {
        return;
    }
    if (surface->states[bb] != D3D12_RESOURCE_STATE_PRESENT)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = surface->render_targets[bb].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = surface->states[bb];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list->ResourceBarrier(1, &barrier);
        surface->states[bb] = D3D12_RESOURCE_STATE_PRESENT;
    }
    // Rebind the main swapchain RTV so any subsequent main-surface draw this frame
    // targets the correct surface again.
    RestoreSwapchainRenderState();
}

void DX12RHI::PresentPendingFloatingSurfaces()
{
    for (DX12FloatingSurface* surface : m_PendingFloatingPresents)
    {
        if (surface == nullptr || surface->swapchain == nullptr)
        {
            continue;
        }
        const HRESULT present_result = surface->swapchain->Present(1, 0);
        if (SUCCEEDED(present_result))
        {
            surface->back_buffer_index = surface->swapchain->GetCurrentBackBufferIndex();
        }
    }
    m_PendingFloatingPresents.clear();
}

void DX12RHI::CreateSwapchainImageViews()
{
    if (!m_Device || !m_Swapchain)
    {
        return;
    }

    if (!EnsureRtvDescriptorHeap())
    {
        return;
    }

    for (RHIImageView*& image_view : m_SwapchainImageviews)
    {
        RHI_DELETE_PTR(image_view);
    }
    m_SwapchainImageviews.clear();
    m_SwapchainImageviews.reserve(k_max_frames_in_flight);

    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (!CheckDX12(m_Swapchain->GetBuffer(i, IID_PPV_ARGS(&m_RenderTargets[i])), "Get swapchain buffer failed"))
        {
            return;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle {};
        if (m_HasSwapchainRtvReuse && m_SwapchainRtvReuse[i].ptr != 0)
        {
            rtv_handle = m_SwapchainRtvReuse[i];
        }
        else if (!AllocateRtvDescriptor(rtv_handle))
        {
            return;
        }
        m_Device->CreateRenderTargetView(m_RenderTargets[i].Get(), nullptr, rtv_handle);

        DX12ImageView* swapchain_view = new DX12ImageView();
        swapchain_view->setResource(nullptr, ToDX12SwapchainFormat(m_SwapchainImageFormat));
        swapchain_view->setRenderTargetHandle(rtv_handle);
        swapchain_view->setIsSwapchainView(true);
        swapchain_view->setSwapchainBackBufferIndex(i);
        m_SwapchainResourceStates[i] = D3D12_RESOURCE_STATE_PRESENT;
        m_SwapchainImageviews.push_back(swapchain_view);
    }

    m_HasSwapchainRtvReuse = false;
}

void DX12RHI::CreateFramebufferImageAndView()
{
    if (m_DepthImageView != nullptr)
    {
        return;
    }

    CreateImage(m_SwapchainExtent.width,
                m_SwapchainExtent.height,
                m_DepthImageFormat,
                RHI_IMAGE_TILING_OPTIMAL,
                RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                m_DepthImage,
                m_DepthImageMemory,
                0,
                1,
                1);

    CreateImageView(m_DepthImage,
                    m_DepthImageFormat,
                    RHI_IMAGE_ASPECT_DEPTH_BIT,
                    RHI_IMAGE_VIEW_TYPE_2D,
                    1,
                    1,
                    m_DepthImageView);
}

RHISampler* DX12RHI::GetOrCreateDefaultSampler(RHIDefaultSamplerType type)
{
    switch (type)
    {
        case Default_Sampler_Linear:
            if (m_LinearSampler == nullptr)
            {
                RHISamplerCreateInfo sampler_info {};
                sampler_info.magFilter = RHI_FILTER_LINEAR;
                sampler_info.minFilter = RHI_FILTER_LINEAR;
                sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
                sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
                sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
                sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
                sampler_info.maxAnisotropy = 1.0f;
                sampler_info.minLod = 0.0f;
                sampler_info.maxLod = D3D12_FLOAT32_MAX;
                CreateSampler(&sampler_info, m_LinearSampler);
            }
            return m_LinearSampler;

        case Default_Sampler_Nearest:
            if (m_NearestSampler == nullptr)
            {
                RHISamplerCreateInfo sampler_info {};
                sampler_info.magFilter = RHI_FILTER_NEAREST;
                sampler_info.minFilter = RHI_FILTER_NEAREST;
                sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_NEAREST;
                sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
                sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
                sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
                sampler_info.maxAnisotropy = 1.0f;
                sampler_info.minLod = 0.0f;
                sampler_info.maxLod = D3D12_FLOAT32_MAX;
                CreateSampler(&sampler_info, m_NearestSampler);
            }
            return m_NearestSampler;

        default:
            return nullptr;
    }
}

RHISampler* DX12RHI::GetOrCreateMipmapSampler(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return nullptr;
    }

    const uint32_t mip_levels =
        static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(std::max(1u, std::max(width, height)))))) + 1u;
    if (const auto found = m_MipmapSamplerMap.find(mip_levels); found != m_MipmapSamplerMap.end())
    {
        return found->second;
    }

    RHISamplerCreateInfo sampler_info {};
    sampler_info.magFilter = RHI_FILTER_LINEAR;
    sampler_info.minFilter = RHI_FILTER_LINEAR;
    sampler_info.mipmapMode = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = RHI_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = static_cast<float>(mip_levels);

    RHISampler* sampler = nullptr;
    if (CreateSampler(&sampler_info, sampler))
    {
        m_MipmapSamplerMap.emplace(mip_levels, sampler);
    }
    return sampler;
}

RHIShader* DX12RHI::CreateShaderModule(const std::vector<unsigned char>& shader_code)
{
    if (shader_code.empty())
    {
        LOG_ERROR(ZShader, "Shader code is empty");
        return nullptr;
    }

    // Create DX12 shader object
    DX12Shader* shader = new DX12Shader();

    // Create D3D blob from shader code
    ComPtr<ID3DBlob> blob;
    HRESULT hr = D3DCreateBlob(shader_code.size(), blob.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ERROR(ZShader, "Failed to create D3D blob for shader");
        delete shader;
        return nullptr;
    }

    // Copy shader code to blob
    std::memcpy(blob->GetBufferPointer(), shader_code.data(), shader_code.size());

    // Set shader blob
    shader->setResource(blob);

    return shader;
}

RHIShader* DX12RHI::CreateShaderModuleFromFile(const std::string& file_path,
                                               ShaderStage shader_stage,
                                               const std::vector<std::string>& include_paths,
                                               const ShaderMacros& macros,
                                               std::vector<uint8_t>& output_binary,
                                               const std::string& entry_point,
                                               bool embed_debug)
{
    // Initialize shader compiler if not already initialized
    if (!m_ShaderCompiler)
    {
        m_ShaderCompiler = std::make_unique<DX12ShaderCompiler>();
    }

    // Convert RHI::ShaderMacros to compiler's ShaderMacros
    std::map<std::string, std::string> compiler_macros(macros.begin(), macros.end());

    // Compile shader from file (pass embed_debug through).
    // Also infer target_profile / hlsl_version from shader_stage if needed
    // (CompileFromFile defaults to SM 6.0 / HV 2018 when those are empty).
    DX12ShaderCompileResult result = m_ShaderCompiler->CompileFromFile(
        file_path, shader_stage, include_paths, compiler_macros,
        entry_point, "", "", embed_debug);

    if (!result.success)
    {
        LOG_ERROR(ZShader, "Failed to compile shader from file: {}", file_path);
        LOG_ERROR(ZShader, "Error: {}", result.error_message);
        return nullptr;
    }

    // Optionally copy DXIL bytecode to caller (used by LoadRp1ShaderFromFile).
    if (!result.dxil_code.empty())
    {
        output_binary = result.dxil_code;
    }

    // Create shader module from compiled DXIL
    return CreateShaderModule(result.dxil_code);
}

RHIShader* DX12RHI::CreateShaderModuleFromSource(const std::string& source_code,
                                                 ShaderStage shader_stage,
                                                 const std::string& shader_name,
                                                 const std::vector<std::string>& include_paths,
                                                 const ShaderMacros& macros)
{
    // Initialize shader compiler if not already initialized
    if (!m_ShaderCompiler)
    {
        m_ShaderCompiler = std::make_unique<DX12ShaderCompiler>();
    }

    // Convert RHI::ShaderMacros to compiler's ShaderMacros
    std::map<std::string, std::string> compiler_macros(macros.begin(), macros.end());

    // Compile shader from source
    DX12ShaderCompileResult result =
        m_ShaderCompiler->CompileFromSource(source_code, shader_stage, shader_name, include_paths, compiler_macros);

    if (!result.success)
    {
        LOG_ERROR(ZShader, "Failed to compile shader from source");
        if (!shader_name.empty())
        {
            LOG_ERROR(ZShader, "Shader name: {}", shader_name);
        }
        LOG_ERROR(ZShader, "Error: {}", result.error_message);
        return nullptr;
    }

    // Create shader module from compiled DXIL
    return CreateShaderModule(result.dxil_code);
}

void DX12RHI::CreateBuffer(RHIDeviceSize size,
                           RHIBufferUsageFlags usage,
                           RHIMemoryPropertyFlags properties,
                           RHIBuffer*& buffer,
                           RHIDeviceMemory*& buffer_memory)
{
    buffer = nullptr;
    buffer_memory = nullptr;

    if (!m_Device || size == 0)
    {
        return;
    }

    const D3D12_HEAP_TYPE heap_type = ToDX12BufferHeapType(usage, properties);

    D3D12_HEAP_PROPERTIES heap_properties = {};
    heap_properties.Type = heap_type;
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Alignment = 0;
    resource_desc.Width = size;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if ((usage & RHI_BUFFER_USAGE_STORAGE_BUFFER_BIT) && heap_type == D3D12_HEAP_TYPE_DEFAULT)
    {
        resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_STATES initial_state = ToDX12BufferInitialState(heap_type);

    ComPtr<ID3D12Resource> resource;
    if (!CheckDX12(m_Device->CreateCommittedResource(&heap_properties,
                                                     D3D12_HEAP_FLAG_NONE,
                                                     &resource_desc,
                                                     initial_state,
                                                     nullptr,
                                                     IID_PPV_ARGS(&resource)),
                   "DX12 create buffer failed"))
    {
        return;
    }

    DX12Buffer* dx12_buffer = new DX12Buffer();
    DX12DeviceMemory* dx12_memory = new DX12DeviceMemory();
    dx12_buffer->setResource(resource, size);
    dx12_memory->setResource(resource, heap_type);

    buffer = dx12_buffer;
    buffer_memory = dx12_memory;
}

void DX12RHI::CreateBufferAndInitialize(RHIBufferUsageFlags usage,
                                        RHIMemoryPropertyFlags properties,
                                        RHIBuffer*& buffer,
                                        RHIDeviceMemory*& buffer_memory,
                                        RHIDeviceSize size,
                                        void* data,
                                        int datasize)
{
    RHIMemoryPropertyFlags effective_properties = properties;
    if (data != nullptr && !(effective_properties & RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        effective_properties |= RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    CreateBuffer(size, usage, effective_properties, buffer, buffer_memory);

    if (buffer_memory == nullptr || data == nullptr || datasize <= 0)
    {
        return;
    }

    void* mapped_data = nullptr;
    if (MapMemory(buffer_memory, 0, size, 0, &mapped_data) && mapped_data != nullptr)
    {
        std::memcpy(mapped_data, data, std::min<RHIDeviceSize>(static_cast<RHIDeviceSize>(datasize), size));
        UnmapMemory(buffer_memory);
    }
}

void DX12RHI::CopyBuffer(RHIBuffer* srcBuffer,
                         RHIBuffer* dstBuffer,
                         RHIDeviceSize srcOffset,
                         RHIDeviceSize dstOffset,
                         RHIDeviceSize size)
{
    DX12Buffer* src = static_cast<DX12Buffer*>(srcBuffer);
    DX12Buffer* dst = static_cast<DX12Buffer*>(dstBuffer);
    if (src == nullptr || dst == nullptr || src->getResource() == nullptr || dst->getResource() == nullptr ||
        size == 0)
    {
        return;
    }

    const auto record_copy = [&](ID3D12GraphicsCommandList* command_list) {
        RecordDefaultHeapBufferCopy(command_list,
                                    src->getResource(),
                                    dst->getResource(),
                                    srcOffset,
                                    dstOffset,
                                    size,
                                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    };

    if (m_CommandLists[m_CurrentFrameIndex])
    {
        record_copy(m_CommandLists[m_CurrentFrameIndex].Get());
        return;
    }

    if (!ExecuteDedicatedUploadCommands(record_copy))
    {
        LOG_ERROR(ZRender, "DX12 CopyBuffer: dedicated upload failed (size={})", static_cast<uint64_t>(size));
    }
}

void DX12RHI::CopyBufferImmediate(RHIBuffer* srcBuffer,
                                   RHIBuffer* dstBuffer,
                                   RHIDeviceSize srcOffset,
                                   RHIDeviceSize dstOffset,
                                   RHIDeviceSize size)
{
    DX12Buffer* src = static_cast<DX12Buffer*>(srcBuffer);
    DX12Buffer* dst = static_cast<DX12Buffer*>(dstBuffer);
    if (src == nullptr || dst == nullptr || src->getResource() == nullptr || dst->getResource() == nullptr ||
        size == 0)
    {
        return;
    }

    const auto record_copy = [&](ID3D12GraphicsCommandList* command_list) {
        RecordDefaultHeapBufferCopy(command_list,
                                    src->getResource(),
                                    dst->getResource(),
                                    srcOffset,
                                    dstOffset,
                                    size,
                                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    };

    if (!ExecuteDedicatedUploadCommands(record_copy))
    {
        LOG_ERROR(ZRender, "DX12 CopyBufferImmediate: dedicated upload failed (size={})", static_cast<uint64_t>(size));
    }
}

void DX12RHI::CreateImage(uint32_t image_width,
                          uint32_t image_height,
                          RHIFormat format,
                          RHIImageTiling image_tiling,
                          RHIImageUsageFlags image_usage_flags,
                          RHIMemoryPropertyFlags memory_property_flags,
                          RHIImage*& image,
                          RHIDeviceMemory*& memory,
                          RHIImageCreateFlags image_create_flags,
                          uint32_t array_layers,
                          uint32_t miplevels)
{
    image = nullptr;
    memory = nullptr;

    if (!m_Device || image_width == 0 || image_height == 0)
    {
        return;
    }

    DXGI_FORMAT dxgi_format = ToDX12Format(format);
    if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    {
        LOG_ERROR(ZRender, "DX12 unsupported image format: {}", static_cast<int>(format));
        return;
    }

    D3D12_HEAP_PROPERTIES heap_properties = {};
    heap_properties.Type = ToDX12HeapType(memory_property_flags);
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Alignment = 0;
    resource_desc.Width = image_width;
    resource_desc.Height = image_height;
    resource_desc.DepthOrArraySize = static_cast<UINT16>(std::max<uint32_t>(array_layers, 1));
    resource_desc.MipLevels = static_cast<UINT16>(std::max<uint32_t>(miplevels, 1));
    resource_desc.Format = dxgi_format;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.SampleDesc.Quality = 0;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource_desc.Flags = ToDX12ResourceFlags(image_usage_flags);

    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    if (image_usage_flags & RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    D3D12_CLEAR_VALUE clear_value = {};
    D3D12_CLEAR_VALUE* clear_value_ptr = nullptr;
    if (resource_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    {
        clear_value.Format = dxgi_format;
        clear_value.DepthStencil.Depth = 1.0f;
        clear_value.DepthStencil.Stencil = 0;
        clear_value_ptr = &clear_value;
    }

    ComPtr<ID3D12Resource> resource;
    if (!CheckDX12(m_Device->CreateCommittedResource(&heap_properties,
                                                     D3D12_HEAP_FLAG_NONE,
                                                     &resource_desc,
                                                     initial_state,
                                                     clear_value_ptr,
                                                     IID_PPV_ARGS(&resource)),
                   "DX12 create image failed"))
    {
        return;
    }

    DX12Image* dx12_image = new DX12Image();
    DX12DeviceMemory* dx12_memory = new DX12DeviceMemory();
    dx12_image->setResource(resource,
                            dxgi_format,
                            image_width,
                            image_height,
                            std::max<uint32_t>(array_layers, 1),
                            std::max<uint32_t>(miplevels, 1),
                            initial_state);
    dx12_memory->setResource(resource, heap_properties.Type);

    image = dx12_image;
    memory = dx12_memory;
}

void DX12RHI::CreateImageView(RHIImage* image,
                              RHIFormat format,
                              RHIImageAspectFlags image_aspect_flags,
                              RHIImageViewType view_type,
                              uint32_t layout_count,
                              uint32_t miplevels,
                              RHIImageView*& image_view)
{
    image_view = nullptr;
    DX12Image* dx12_image = static_cast<DX12Image*>(image);
    if (!dx12_image)
    {
        return;
    }

    DXGI_FORMAT dxgi_format = ToDX12Format(format);
    if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    {
        dxgi_format = dx12_image->getFormat();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle {};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle {};
    bool has_dsv = false;

    if (m_Device && (image_aspect_flags & RHI_IMAGE_ASPECT_DEPTH_BIT))
    {
        if (AllocateDsvDescriptor(dsv_handle))
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
            dsv_desc.Format = dxgi_format;
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
            m_Device->CreateDepthStencilView(dx12_image->getResource(), &dsv_desc, dsv_handle);
            has_dsv = true;
        }
    }
    else if (m_Device && AllocateCbvSrvUavDescriptor(cpu_handle, gpu_handle))
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = dxgi_format;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        switch (view_type)
        {
            case RHI_IMAGE_VIEW_TYPE_CUBE:
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srv_desc.TextureCube.MostDetailedMip = 0;
                srv_desc.TextureCube.MipLevels = std::max<uint32_t>(miplevels, 1);
                srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
                break;
            case RHI_IMAGE_VIEW_TYPE_2D_ARRAY:
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv_desc.Texture2DArray.MostDetailedMip = 0;
                srv_desc.Texture2DArray.MipLevels = std::max<uint32_t>(miplevels, 1);
                srv_desc.Texture2DArray.FirstArraySlice = 0;
                srv_desc.Texture2DArray.ArraySize = std::max<uint32_t>(layout_count, 1);
                break;
            case RHI_IMAGE_VIEW_TYPE_2D:
            default:
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv_desc.Texture2D.MostDetailedMip = 0;
                srv_desc.Texture2D.MipLevels = std::max<uint32_t>(miplevels, 1);
                srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
                break;
        }

        m_Device->CreateShaderResourceView(dx12_image->getResource(), &srv_desc, cpu_handle);
    }

    DX12ImageView* dx12_view = new DX12ImageView();
    dx12_view->setResource(dx12_image,
                           dxgi_format,
                           cpu_handle,
                           gpu_handle,
                           view_type,
                           std::max<uint32_t>(layout_count, 1),
                           std::max<uint32_t>(miplevels, 1));
    if (has_dsv)
    {
        dx12_view->setDepthStencilHandle(dsv_handle);
    }
    image_view = dx12_view;
}

void DX12RHI::CreateGlobalImage(RHIImage*& image,
                                RHIImageView*& image_view,
                                void* image_allocation,
                                uint32_t texture_image_width,
                                uint32_t texture_image_height,
                                void* texture_image_pixels,
                                RHIFormat texture_image_format,
                                uint32_t miplevels)
{
    RHIFormat storage_format = texture_image_format;
    uint32_t source_bpp = 0;
    uint32_t upload_bpp = 0;
    PickDx12StorageFormat(texture_image_format, storage_format, source_bpp, upload_bpp);

    RHIDeviceMemory* memory = nullptr;
    CreateImage(texture_image_width,
                texture_image_height,
                storage_format,
                RHI_IMAGE_TILING_OPTIMAL,
                RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT,
                RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                image,
                memory,
                0,
                1,
                std::max<uint32_t>(miplevels, 1));
    if (texture_image_pixels != nullptr)
    {
        DX12Image* dx12_image = static_cast<DX12Image*>(image);
        const bool block_compressed = IsBlockCompressedRHIFormat(storage_format);
        const bool needs_f32_to_f16 = (source_bpp == 16 && upload_bpp == 8);
        const uint32_t effective_mips = std::max<uint32_t>(miplevels, 1);

        // HDR float32->float16 conversion is handled only by the legacy single-
        // subresource path (global IBL / LUT textures are always single-mip
        // uncompressed). Everything else -- LDR uncompressed and block-
        // compressed, single- OR multi-mip -- goes through the packed mip-chain
        // uploader, which slices `texture_image_pixels` per mip from format+dims
        // (matching TextureCompressor / Texture2D::m_Pixels layout exactly).
        if (needs_f32_to_f16 || (effective_mips <= 1 && !block_compressed))
        {
            TextureUploadStaging staging {};
            if (!CreateTextureUploadStaging(m_Device.Get(),
                                            dx12_image->getResource(),
                                            texture_image_pixels,
                                            texture_image_width,
                                            texture_image_height,
                                            0,
                                            0,
                                            dx12_image->getMipLevels(),
                                            dx12_image->getArrayLayers(),
                                            source_bpp,
                                            upload_bpp,
                                            staging))
            {
                LOG_ERROR(ZRender, "DX12 CreateGlobalImage: staging failed ({}x{})", texture_image_width, texture_image_height);
            }
            else if (!ExecuteDedicatedUploadCommands([&](ID3D12GraphicsCommandList* command_list) {
                         TransitionImageOnList(dx12_image,
                                               staging.subresource_index,
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               command_list);
                         RecordTextureCopy(command_list, dx12_image->getResource(), staging);
                         TransitionImageOnList(dx12_image,
                                               staging.subresource_index,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               command_list);
                     }))
            {
                LOG_ERROR(ZRender, "DX12 CreateGlobalImage: upload failed ({}x{})", texture_image_width, texture_image_height);
            }
        }
        else if (!UploadPackedMipChain(m_Device.Get(),
                                       dx12_image,
                                       static_cast<const uint8_t*>(texture_image_pixels),
                                       texture_image_width,
                                       texture_image_height,
                                       effective_mips,
                                       storage_format,
                                       upload_bpp,
                                       [this](std::function<void(ID3D12GraphicsCommandList*)> rec) {
                                           return ExecuteDedicatedUploadCommands(rec);
                                       }))
        {
            LOG_ERROR(ZRender,
                      "DX12 CreateGlobalImage: mip-chain upload failed ({}x{}, mips={}, fmt={})",
                      texture_image_width,
                      texture_image_height,
                      effective_mips,
                      static_cast<int>(storage_format));
        }
    }

    CreateImageView(image,
                    storage_format,
                    RHI_IMAGE_ASPECT_COLOR_BIT,
                    RHI_IMAGE_VIEW_TYPE_2D,
                    1,
                    std::max<uint32_t>(miplevels, 1),
                    image_view);
    RHI_DELETE_PTR(memory);
}

void DX12RHI::CreateCubeMap(RHIImage*& image,
                            RHIImageView*& image_view,
                            void* image_allocation,
                            uint32_t texture_image_width,
                            uint32_t texture_image_height,
                            std::array<void*, 6> texture_image_pixels,
                            RHIFormat texture_image_format,
                            uint32_t miplevels)
{
    const uint32_t effective_miplevels = std::max(1u, miplevels);

    RHIFormat storage_format = texture_image_format;
    uint32_t source_bpp = 0;
    uint32_t upload_bpp = 0;
    PickDx12StorageFormat(texture_image_format, storage_format, source_bpp, upload_bpp);

    RHIDeviceMemory* memory = nullptr;
    RHIImageUsageFlags cubemap_usage =
        RHI_IMAGE_USAGE_SAMPLED_BIT | RHI_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (effective_miplevels > 1)
    {
        // GPU mipgen binds each mip as RTV (DX12CubemapMipGenerator).
        cubemap_usage |= RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    CreateImage(texture_image_width,
                texture_image_height,
                storage_format,
                RHI_IMAGE_TILING_OPTIMAL,
                cubemap_usage,
                RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                image,
                memory,
                0,
                6,
                effective_miplevels);
    if (image == nullptr)
    {
        LOG_ERROR(ZRender,
                  "DX12 CreateCubeMap: CreateImage failed ({}x{}, mips={})",
                  texture_image_width,
                  texture_image_height,
                  effective_miplevels);
        return;
    }
    std::array<const void*, 6> cubemap_face_pixels {};
    for (size_t face = 0; face < cubemap_face_pixels.size(); ++face)
    {
        cubemap_face_pixels[face] = texture_image_pixels[face];
    }
    if (!UploadCubeMapAllMips(static_cast<DX12Image*>(image),
                              cubemap_face_pixels,
                              texture_image_width,
                              texture_image_height,
                              effective_miplevels,
                              source_bpp))
    {
        LOG_ERROR(ZRender,
                  "DX12 CreateCubeMap: upload failed ({}x{}, mips={})",
                  texture_image_width,
                  texture_image_height,
                  effective_miplevels);
    }
    else if (effective_miplevels > 1)
    {
        ++m_IblGpuCubemapUploadCount;
        m_IblGpuCubemapLastMips = effective_miplevels;
        m_IblGpuCubemapLastWidth = texture_image_width;
        LOG_INFO(ZRender,
                 "DX12 CreateCubeMap: uploaded mip0 + GPU-generated {} mips ({}x{})",
                 effective_miplevels,
                 texture_image_width,
                 texture_image_height);
    }

    CreateImageView(image,
                    storage_format,
                    RHI_IMAGE_ASPECT_COLOR_BIT,
                    RHI_IMAGE_VIEW_TYPE_CUBE,
                    6,
                    effective_miplevels,
                    image_view);
    RHI_DELETE_PTR(memory);
}

void DX12RHI::LogDeferredIblCubemapDiagnostics() const
{
    if (m_IblMipGenReady)
    {
        LOG_INFO(ZRender,
                 "DX12CubemapMipGenerator: ready (format={})",
                 static_cast<uint32_t>(m_IblMipGenFormat));
    }
    if (m_IblGpuCubemapUploadCount > 0)
    {
        LOG_INFO(ZRender,
                 "DX12 CreateCubeMap: uploaded mip0 + GPU-generated {} mips ({}x{}, {} cubemap(s))",
                 m_IblGpuCubemapLastMips,
                 m_IblGpuCubemapLastWidth,
                 m_IblGpuCubemapUploadCount);
    }
}

void DX12RHI::CreateCommandPool()
{
    // TODO: Implement DX12 createCommandPool
}

bool DX12RHI::CreateCommandPool(const RHICommandPoolCreateInfo* pCreateInfo, RHICommandPool*& pCommandPool)
{
    // TODO: Implement DX12 createCommandPool
    return false;
}

bool DX12RHI::CreateDescriptorPool(const RHIDescriptorPoolCreateInfo* pCreateInfo, RHIDescriptorPool*& pDescriptorPool)
{
    pDescriptorPool = new DX12DescriptorPool();
    return true;
}

bool DX12RHI::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* pCreateInfo,
                                        RHIDescriptorSetLayout*& pSetLayout)
{
    pSetLayout = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }

    DX12DescriptorSetLayout* layout = new DX12DescriptorSetLayout();
    if (pCreateInfo->bindingCount > 0 && pCreateInfo->pBindings != nullptr)
    {
        layout->setBindings(pCreateInfo->pBindings, pCreateInfo->bindingCount);
    }
    pSetLayout = layout;
    return true;
}

bool DX12RHI::CreateFence(const RHIFenceCreateInfo* pCreateInfo, RHIFence*& pFence)
{
    // TODO: Implement DX12 createFence
    return false;
}

bool DX12RHI::CreateFramebuffer(const RHIFramebufferCreateInfo* pCreateInfo, RHIFramebuffer*& pFramebuffer)
{
    if (pFramebuffer != nullptr)
    {
        RHI_DELETE_PTR(pFramebuffer);
    }
    pFramebuffer = nullptr;

    if (pCreateInfo == nullptr || pCreateInfo->attachmentCount == 0 || pCreateInfo->pAttachments == nullptr)
    {
        return false;
    }

    DX12RenderPass* render_pass = static_cast<DX12RenderPass*>(pCreateInfo->renderPass);
    DX12Framebuffer* framebuffer = new DX12Framebuffer();
    framebuffer->setRenderPass(render_pass);
    framebuffer->SetSize(pCreateInfo->width, pCreateInfo->height, pCreateInfo->layers);

    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        DX12ImageView* image_view = static_cast<DX12ImageView*>(pCreateInfo->pAttachments[i]);
        if (image_view == nullptr)
        {
            continue;
        }

        RHIFormat attachment_format = RHI_FORMAT_UNDEFINED;
        if (render_pass != nullptr && i < render_pass->getAttachments().size())
        {
            attachment_format = render_pass->getAttachments()[i].format;
        }
        const bool is_depth = IsDX12DepthFormat(attachment_format) || image_view->hasDepthStencilHandle();

        if (is_depth)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = image_view->getDepthStencilHandle();
            if (!image_view->hasDepthStencilHandle() && image_view->getImage() != nullptr)
            {
                if (AllocateDsvDescriptor(dsv_handle))
                {
                    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
                    dsv_desc.Format = image_view->getFormat();
                    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
                    m_Device->CreateDepthStencilView(image_view->getImage()->getResource(), &dsv_desc, dsv_handle);
                    image_view->setDepthStencilHandle(dsv_handle);
                }
            }
            if (image_view->hasDepthStencilHandle())
            {
                framebuffer->setDsv(image_view->getDepthStencilHandle(), image_view);
            }
        }
        else
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = image_view->getRenderTargetHandle();
            if (!image_view->hasRenderTargetHandle() && image_view->getImage() != nullptr)
            {
                if (AllocateRtvDescriptor(rtv_handle))
                {
                    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
                    rtv_desc.Format = image_view->getFormat();
                    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    m_Device->CreateRenderTargetView(image_view->getImage()->getResource(), &rtv_desc, rtv_handle);
                    image_view->setRenderTargetHandle(rtv_handle);
                }
            }
            if (image_view->hasRenderTargetHandle())
            {
                framebuffer->addRtv(image_view->getRenderTargetHandle(), image_view);
            }
        }

        framebuffer->setAttachmentView(i, image_view);
    }

    pFramebuffer = framebuffer;
    return true;
}

bool DX12RHI::CreateGraphicsPipelines(RHIPipelineCache* pipelineCache,
                                      uint32_t createInfoCount,
                                      const RHIGraphicsPipelineCreateInfo* pCreateInfos,
                                      RHIPipeline*& pPipelines)
{
    pPipelines = nullptr;
    if (!m_Device || createInfoCount == 0 || pCreateInfos == nullptr)
    {
        return false;
    }

    const RHIGraphicsPipelineCreateInfo& create_info = pCreateInfos[0];
    DX12PipelineLayout* pipeline_layout = static_cast<DX12PipelineLayout*>(create_info.layout);
    if (pipeline_layout == nullptr || pipeline_layout->getRootSignature() == nullptr)
    {
        LOG_ERROR(ZRender, "DX12 createGraphicsPipelines requires a valid pipeline layout/root signature");
        return false;
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
    std::vector<uint32_t> vertex_strides;
    if (create_info.pVertexInputState != nullptr)
    {
        const auto* vertex_input = create_info.pVertexInputState;
        vertex_strides.resize(vertex_input->vertexBindingDescriptionCount);
        for (uint32_t i = 0; i < vertex_input->vertexBindingDescriptionCount; ++i)
        {
            const RHIVertexInputBindingDescription& binding = vertex_input->pVertexBindingDescriptions[i];
            if (binding.binding >= vertex_strides.size())
            {
                vertex_strides.resize(binding.binding + 1);
            }
            vertex_strides[binding.binding] = binding.stride;
        }

        input_elements.reserve(vertex_input->vertexAttributeDescriptionCount);
        for (uint32_t i = 0; i < vertex_input->vertexAttributeDescriptionCount; ++i)
        {
            const RHIVertexInputAttributeDescription& attribute = vertex_input->pVertexAttributeDescriptions[i];
            D3D12_INPUT_ELEMENT_DESC element = {};
            element.SemanticName = ToDX12SemanticName(attribute.location);
            element.SemanticIndex = ToDX12SemanticIndex(attribute.location);
            element.Format = ToDX12Format(attribute.format);
            element.InputSlot = attribute.binding;
            element.AlignedByteOffset = attribute.offset;
            element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            element.InstanceDataStepRate = 0;

            for (uint32_t binding_index = 0; binding_index < vertex_input->vertexBindingDescriptionCount; ++binding_index)
            {
                const RHIVertexInputBindingDescription& binding = vertex_input->pVertexBindingDescriptions[binding_index];
                if (binding.binding == attribute.binding)
                {
                    element.InputSlotClass = ToDX12InputClassification(binding.inputRate);
                    element.InstanceDataStepRate = binding.inputRate == RHI_VERTEX_INPUT_RATE_INSTANCE ? 1 : 0;
                    break;
                }
            }
            input_elements.push_back(element);
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = pipeline_layout->getRootSignature();

    for (uint32_t i = 0; i < create_info.stageCount; ++i)
    {
        const RHIPipelineShaderStageCreateInfo& shader_stage = create_info.pStages[i];
        DX12Shader* shader = static_cast<DX12Shader*>(shader_stage.module);
        if (shader == nullptr || shader->getBufferPointer() == nullptr || shader->getBufferSize() == 0)
        {
            continue;
        }

        D3D12_SHADER_BYTECODE bytecode {shader->getBufferPointer(), shader->getBufferSize()};
        if (shader_stage.stage == RHI_SHADER_STAGE_VERTEX_BIT)
        {
            pso_desc.VS = bytecode;
        }
        else if (shader_stage.stage == RHI_SHADER_STAGE_FRAGMENT_BIT)
        {
            pso_desc.PS = bytecode;
        }
    }

    if (pso_desc.VS.pShaderBytecode == nullptr || pso_desc.VS.BytecodeLength == 0 ||
        pso_desc.PS.pShaderBytecode == nullptr || pso_desc.PS.BytecodeLength == 0)
    {
        LOG_ERROR(ZRender,
                  "DX12 createGraphicsPipelines: missing VS or PS bytecode (VS={} PS={})",
                  pso_desc.VS.BytecodeLength,
                  pso_desc.PS.BytecodeLength);
        return false;
    }

    pso_desc.InputLayout = {input_elements.data(), static_cast<UINT>(input_elements.size())};

    RHIPrimitiveTopology rhi_topology = RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (create_info.pInputAssemblyState != nullptr)
    {
        rhi_topology = create_info.pInputAssemblyState->topology;
    }
    pso_desc.PrimitiveTopologyType = ToDX12PrimitiveTopologyType(rhi_topology);

    pso_desc.RasterizerState.FillMode = create_info.pRasterizationState ? ToDX12FillMode(create_info.pRasterizationState->polygonMode) : D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = create_info.pRasterizationState ? ToDX12CullMode(create_info.pRasterizationState->cullMode) : D3D12_CULL_MODE_BACK;
    pso_desc.RasterizerState.FrontCounterClockwise = create_info.pRasterizationState ? (create_info.pRasterizationState->frontFace == RHI_FRONT_FACE_COUNTER_CLOCKWISE) : TRUE;
    pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.MultisampleEnable = FALSE;
    pso_desc.RasterizerState.AntialiasedLineEnable = FALSE;
    pso_desc.RasterizerState.ForcedSampleCount = 0;
    pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < 8; ++i)
    {
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso_desc.BlendState.RenderTarget[i];
        blend.BlendEnable = FALSE;
        blend.LogicOpEnable = FALSE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_ZERO;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    if (create_info.pColorBlendState != nullptr && create_info.pColorBlendState->attachmentCount > 0)
    {
        const RHIPipelineColorBlendAttachmentState& rhi_blend = create_info.pColorBlendState->pAttachments[0];
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso_desc.BlendState.RenderTarget[0];
        blend.BlendEnable = rhi_blend.blendEnable;
        blend.SrcBlend = ToDX12BlendFactor(rhi_blend.srcColorBlendFactor);
        blend.DestBlend = ToDX12BlendFactor(rhi_blend.dstColorBlendFactor);
        blend.BlendOp = ToDX12BlendOp(rhi_blend.colorBlendOp);
        blend.SrcBlendAlpha = ToDX12BlendFactor(rhi_blend.srcAlphaBlendFactor);
        blend.DestBlendAlpha = ToDX12BlendFactor(rhi_blend.dstAlphaBlendFactor);
        blend.BlendOpAlpha = ToDX12BlendOp(rhi_blend.alphaBlendOp);
        blend.RenderTargetWriteMask = ToDX12ColorWriteMask(rhi_blend.colorWriteMask);
    }

    pso_desc.DepthStencilState.DepthEnable = create_info.pDepthStencilState ? create_info.pDepthStencilState->depthTestEnable : FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = (create_info.pDepthStencilState && create_info.pDepthStencilState->depthWriteEnable) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.DepthStencilState.DepthFunc = create_info.pDepthStencilState ? ToDX12ComparisonFunc(create_info.pDepthStencilState->depthCompareOp) : D3D12_COMPARISON_FUNC_ALWAYS;
    pso_desc.DepthStencilState.StencilEnable = FALSE;

    pso_desc.SampleMask = UINT_MAX;
    ApplyPsoRenderTargetFormats(create_info, m_SwapchainImageFormat, m_DepthImageFormat, pso_desc);
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;
    pso_desc.NodeMask = 1;
    pso_desc.CachedPSO = {};
    pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pipeline_state;
    if (!CheckDX12(m_Device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state)),
                   "CreateGraphicsPipelineState failed"))
    {
        LOG_ERROR(ZRender,
                  "DX12 PSO dump: NumRTV={} RTV0={} DSV={} DepthEnable={} inputElements={} VS={} PS={}",
                  pso_desc.NumRenderTargets,
                  static_cast<uint32_t>(pso_desc.RTVFormats[0]),
                  static_cast<uint32_t>(pso_desc.DSVFormat),
                  pso_desc.DepthStencilState.DepthEnable ? 1 : 0,
                  static_cast<uint32_t>(input_elements.size()),
                  pso_desc.VS.BytecodeLength,
                  pso_desc.PS.BytecodeLength);
        return false;
    }

    DX12Pipeline* pipeline = new DX12Pipeline();
    pipeline->setPipelineState(pipeline_state);
    pipeline->setPipelineLayout(pipeline_layout);
    pipeline->setPrimitiveTopology(ToDX12PrimitiveTopology(rhi_topology));
    pipeline->setVertexStrides(std::move(vertex_strides));
    pPipelines = pipeline;
    return true;
}

bool DX12RHI::CreateComputePipelines(RHIPipelineCache* pipelineCache,
                                     uint32_t createInfoCount,
                                     const RHIComputePipelineCreateInfo* pCreateInfos,
                                     RHIPipeline*& pPipelines)
{
    pPipelines = nullptr;
    if (!m_Device || createInfoCount == 0 || pCreateInfos == nullptr)
    {
        return false;
    }

    const RHIComputePipelineCreateInfo& create_info = pCreateInfos[0];
    DX12PipelineLayout* pipeline_layout = static_cast<DX12PipelineLayout*>(create_info.layout);
    if (pipeline_layout == nullptr || pipeline_layout->getRootSignature() == nullptr || create_info.pStages == nullptr)
    {
        LOG_ERROR(ZRender, "DX12 createComputePipelines requires a valid compute shader and pipeline layout/root signature");
        return false;
    }

    DX12Shader* shader = static_cast<DX12Shader*>(create_info.pStages->module);
    if (create_info.pStages->stage != RHI_SHADER_STAGE_COMPUTE_BIT || shader == nullptr ||
        shader->getBufferPointer() == nullptr || shader->getBufferSize() == 0)
    {
        LOG_ERROR(ZRender, "DX12 createComputePipelines received an invalid compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = pipeline_layout->getRootSignature();
    pso_desc.CS = {shader->getBufferPointer(), shader->getBufferSize()};
    pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ComPtr<ID3D12PipelineState> pipeline_state;
    if (!CheckDX12(m_Device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state)),
                   "CreateComputePipelineState failed"))
    {
        return false;
    }

    DX12Pipeline* pipeline = new DX12Pipeline();
    pipeline->setPipelineState(pipeline_state);
    pipeline->setPipelineLayout(pipeline_layout);
    pPipelines = pipeline;
    return true;
}

bool DX12RHI::CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* pCreateInfo, RHIPipelineLayout*& pPipelineLayout)
{
    pPipelineLayout = nullptr;
    if (!m_Device || pCreateInfo == nullptr)
    {
        return false;
    }

    // ---- PR6: detect bindless descriptor sets ------------------------
    // A descriptor set is treated as bindless iff *any* of its bindings
    // carries RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT.
    // Mixing bindless + bindful inside the same set is not supported on
    // either DX12 or Vulkan (the engine's BindlessTextureManager always
    // uses a single binding=0 slot in space0, so this restriction is
    // expressive enough). Bindless sets contribute zero descriptor-table
    // ranges -- their bindings are sourced directly from the bindless
    // heap via ResourceDescriptorHeap[NonUniformResourceIndex(idx)] in
    // HLSL, with the index pushed at draw time through
    // cmdSetBindlessIndexPFN -> a 32-bit root constant we reserve below.
    auto is_set_bindless = [](DX12DescriptorSetLayout* set_layout) -> bool {
        if (set_layout == nullptr)
        {
            return false;
        }
        for (const RHIDescriptorSetLayoutBinding& binding : set_layout->getBindings())
        {
            if ((binding.bindingFlags & RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0)
            {
                return true;
            }
        }
        return false;
    };

    bool any_bindless_set = false;
    for (uint32_t set_index = 0; set_index < pCreateInfo->setLayoutCount; ++set_index)
    {
        if (is_set_bindless(static_cast<DX12DescriptorSetLayout*>(pCreateInfo->pSetLayouts[set_index])))
        {
            any_bindless_set = true;
            break;
        }
    }

    // Hard-fail: if the user requested bindless but the device cannot
    // support it (SM<6.6 or ResourceBindingTier<2), we cannot silently
    // fall back to bindful -- the shader's compiled DXIL will still
    // reference ResourceDescriptorHeap[] and PSO creation would fail
    // later with a confusing error. Surface a clear diagnostic now.
    if (any_bindless_set && !m_BindlessSupported)
    {
        LOG_ERROR(ZRender,
                  "DX12RHI::createPipelineLayout: a descriptor set declared "
                  "RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT but the device "
                  "does not support bindless (SM 6.6 + ResourceBindingTier>=2 required). "
                  "Refusing to build a non-bindless fallback root signature -- shaders "
                  "would mismatch.");
        return false;
    }
    // -------------------------------------------------------------------

    uint32_t total_ranges = 0;
    for (uint32_t set_index = 0; set_index < pCreateInfo->setLayoutCount; ++set_index)
    {
        DX12DescriptorSetLayout* set_layout = static_cast<DX12DescriptorSetLayout*>(pCreateInfo->pSetLayouts[set_index]);
        if (set_layout == nullptr)
        {
            continue;
        }
        // PR6: bindless sets contribute zero descriptor-table ranges.
        if (is_set_bindless(set_layout))
        {
            continue;
        }
        for (const RHIDescriptorSetLayoutBinding& binding : set_layout->getBindings())
        {
            total_ranges += binding.descriptorType == RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? 2 : 1;
        }
    }

    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    std::vector<D3D12_ROOT_PARAMETER> root_parameters;
    std::vector<DX12RootParameterBinding> parameter_bindings;
    // Reserve one extra slot for the PR6 bindless root constant if
    // needed (cheap, avoids a reallocation that would invalidate
    // pDescriptorRanges pointers below).
    ranges.reserve(total_ranges);
    root_parameters.reserve(total_ranges + (any_bindless_set ? 1u : 0u));
    parameter_bindings.reserve(total_ranges);

    for (uint32_t set_index = 0; set_index < pCreateInfo->setLayoutCount; ++set_index)
    {
        DX12DescriptorSetLayout* set_layout = static_cast<DX12DescriptorSetLayout*>(pCreateInfo->pSetLayouts[set_index]);
        if (set_layout == nullptr)
        {
            continue;
        }
        // PR6: skip bindless sets -- they don't contribute root
        // parameters AND their bindings stay out of m_Bindings, so the
        // existing cmdBindDescriptorSetsPFN naturally skips them.
        if (is_set_bindless(set_layout))
        {
            continue;
        }

        for (const RHIDescriptorSetLayoutBinding& binding : set_layout->getBindings())
        {
            const bool is_combined_image_sampler = binding.descriptorType == RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            const uint32_t range_count = is_combined_image_sampler ? 2 : 1;

            for (uint32_t range_index = 0; range_index < range_count; ++range_index)
            {
                D3D12_DESCRIPTOR_RANGE_TYPE range_type = ToDX12DescriptorRangeType(binding.descriptorType);
                DX12DescriptorHeapKind heap_kind = DX12DescriptorHeapKind::CbvSrvUav;
                if ((is_combined_image_sampler && range_index == 1) || binding.descriptorType == RHI_DESCRIPTOR_TYPE_SAMPLER)
                {
                    range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                    heap_kind = DX12DescriptorHeapKind::Sampler;
                }

                ranges.push_back({});
                D3D12_DESCRIPTOR_RANGE& range = ranges.back();
                range.RangeType = range_type;
                range.NumDescriptors = std::max<uint32_t>(binding.descriptorCount, 1);
                range.BaseShaderRegister = binding.binding;
                range.RegisterSpace = set_index;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                D3D12_ROOT_PARAMETER root_parameter = {};
                root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                root_parameter.ShaderVisibility = ToDX12ShaderVisibility(binding.stageFlags);
                root_parameter.DescriptorTable.NumDescriptorRanges = 1;
                root_parameter.DescriptorTable.pDescriptorRanges = &ranges.back();

                const uint32_t root_parameter_index = static_cast<uint32_t>(root_parameters.size());
                root_parameters.push_back(root_parameter);
                parameter_bindings.push_back({set_index, binding.binding, heap_kind, root_parameter_index});
            }
        }
    }

    // ---- PR6: append the bindless root constant ----------------------
    // One DWORD at b0/space0, ALL stages visible. Caller pushes the
    // 32-bit payload produced by `BindlessIndex::Pack(tex, sampler)`
    // (rhi.h) through RHI::CmdSetBindlessIndexPFN, which translates to
    // SetGraphicsRoot32BitConstant on this very parameter. The shader
    // unpacks via `BindlessIndex::UnpackTexture / unpackSampler` --
    // see bindless_smoke.hlsl for the HLSL-side equivalent.
    uint32_t bindless_root_constant_index = 0;
    if (any_bindless_set)
    {
        D3D12_ROOT_PARAMETER root_parameter = {};
        root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameter.Constants.ShaderRegister = 0;
        root_parameter.Constants.RegisterSpace = 0;
        root_parameter.Constants.Num32BitValues = 1;

        bindless_root_constant_index = static_cast<uint32_t>(root_parameters.size());
        root_parameters.push_back(root_parameter);
    }
    // -------------------------------------------------------------------

    // ---- PR7: bindless static-sampler bank (s0..s3) -------------------
    // Whenever a bindless set participates in this layout, we attach a
    // fixed 4-entry static sampler bank at b0/space0 ShaderRegister 0..3:
    //   s0 = LinearWrap, s1 = LinearClamp, s2 = PointWrap, s3 = PointClamp
    // Rationale: bindless HLSL (see runtime/.../dx12/test/bindless_smoke.hlsl
    // and runtime/.../utility/shaders/bindless_blit_ps.hlsl) selects a
    // sampler from a uint8 packed into the bindless root constant -- no
    // descriptor-table sampler binding is possible because the HLSL has
    // to switch between samplers at runtime. Hard-coding the bank in the
    // RHI keeps every bindless shader on the same contract; the contract
    // is the same one already validated by dx12_bindless_smoke_test.cpp
    // (phase 4). ShaderVisibility = PIXEL matches the smoke-test exactly
    // (current bindless workloads only sample in PS); revisit if a VS
    // case ever needs `ResourceDescriptorHeap[]` reads.
    D3D12_STATIC_SAMPLER_DESC bindless_static_samplers[kBindlessStaticSamplerCount] = {};
    if (any_bindless_set)
    {
        auto fill_sampler = [](D3D12_STATIC_SAMPLER_DESC& s,
                               D3D12_FILTER filter,
                               D3D12_TEXTURE_ADDRESS_MODE addr,
                               UINT slot) {
            s.Filter = filter;
            s.AddressU = addr;
            s.AddressV = addr;
            s.AddressW = addr;
            s.MipLODBias = 0.0f;
            s.MaxAnisotropy = 1;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            s.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            s.MinLOD = 0.0f;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ShaderRegister = slot;
            s.RegisterSpace = 0;
            s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        };
        // Index order MUST match BindlessBlitSampler in
        // dx12/utility/bindless_texture_blit_pipeline.h. Pinned by a
        // static_assert in dx12_bindless_smoke_test.cpp on
        // kBindlessStaticSamplerCount == 4.
        fill_sampler(bindless_static_samplers[0], D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0);
        fill_sampler(bindless_static_samplers[1], D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1);
        fill_sampler(bindless_static_samplers[2], D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 2);
        fill_sampler(bindless_static_samplers[3], D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 3);
    }
    // -------------------------------------------------------------------

    // Repin range pointers: root_parameters.push_back() may reallocate `ranges`,
    // leaving earlier DescriptorTable.pDescriptorRanges dangling (shows up as
    // CreateRootSignature returning DEVICE_REMOVED on an otherwise healthy device).
    {
        size_t range_cursor = 0;
        for (D3D12_ROOT_PARAMETER& root_parameter : root_parameters)
        {
            if (root_parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            {
                continue;
            }
            if (range_cursor >= ranges.size())
            {
                LOG_ERROR(ZRender,
                          "DX12 CreatePipelineLayout: descriptor-table / range count mismatch "
                          "(tables={}, ranges={})",
                          root_parameters.size(),
                          ranges.size());
                return false;
            }
            root_parameter.DescriptorTable.pDescriptorRanges = &ranges[range_cursor++];
        }
    }

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
    root_signature_desc.NumParameters = static_cast<UINT>(root_parameters.size());
    root_signature_desc.pParameters = root_parameters.empty() ? nullptr : root_parameters.data();
    root_signature_desc.NumStaticSamplers = any_bindless_set ? kBindlessStaticSamplerCount : 0u;
    root_signature_desc.pStaticSamplers = any_bindless_set ? bindless_static_samplers : nullptr;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (any_bindless_set)
    {
        root_signature_desc.Flags |= GetBindlessRootSignatureFlags();
    }

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> error_blob;
    HRESULT serialize_result = D3D12SerializeRootSignature(&root_signature_desc,
                                                           D3D_ROOT_SIGNATURE_VERSION_1,
                                                           signature_blob.GetAddressOf(),
                                                           error_blob.GetAddressOf());
    if (FAILED(serialize_result))
    {
        if (error_blob)
        {
            LOG_ERROR(ZRender,
                      "D3D12SerializeRootSignature failed: {}",
                      static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        return false;
    }

    const HRESULT device_reason_before = m_Device->GetDeviceRemovedReason();
    if (IsActualDx12DeviceRemoval(device_reason_before))
    {
        LOG_ERROR(ZRender,
                  "DX12 CreatePipelineLayout: device already removed before CreateRootSignature "
                  "(sets={}, root_params={}, HRESULT=0x{:08X})",
                  pCreateInfo->setLayoutCount,
                  root_parameters.size(),
                  static_cast<unsigned int>(device_reason_before));
        return false;
    }

    ComPtr<ID3D12RootSignature> root_signature;
    const HRESULT create_root_sig_result =
        m_Device->CreateRootSignature(0,
                                      signature_blob->GetBufferPointer(),
                                      signature_blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root_signature));
    if (!CheckDX12(create_root_sig_result, "CreateRootSignature failed"))
    {
        if (create_root_sig_result == DXGI_ERROR_DEVICE_REMOVED)
        {
            IsDeviceRemoved(" during CreateRootSignature");
            LOG_ERROR(ZRender,
                      "DX12 CreateRootSignature: device already removed (sets={}, root_params={}). "
                      "Root cause is usually an earlier GPU fault (often IBL HDR cubemap upload). "
                      "See 'device removed' lines above.",
                      pCreateInfo->setLayoutCount,
                      root_parameters.size());
        }
        return false;
    }

    DX12PipelineLayout* pipeline_layout = new DX12PipelineLayout();
    pipeline_layout->setRootSignature(root_signature, std::move(parameter_bindings));
    pipeline_layout->setBindlessInfo(any_bindless_set, bindless_root_constant_index);
    pPipelineLayout = pipeline_layout;
    return true;
}

// ---- PR6: bindless root signature flag set ---------------------------
// Single source of truth for which D3D12_ROOT_SIGNATURE_FLAGS bits a
// pipeline layout earns when it contains at least one bindless set.
// Currently CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED only; sampler-bindless
// (SAMPLER_HEAP_DIRECTLY_INDEXED) is deliberately deferred -- the
// engine's current sampler model is "static sampler array baked into
// the root signature", which is incompatible with directly-indexed
// sampler heaps. Adding it would require rewriting every sampler
// binding in the engine. Keep this hook here so the future PR is a
// one-line change.
D3D12_ROOT_SIGNATURE_FLAGS DX12RHI::GetBindlessRootSignatureFlags()
{
    return D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
}
// --------------------------------------------------------------------

bool DX12RHI::CreateRenderPass(const RHIRenderPassCreateInfo* pCreateInfo, RHIRenderPass*& pRenderPass)
{
    pRenderPass = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }

    DX12RenderPass* render_pass = new DX12RenderPass();
    if (!render_pass->setCreateInfo(pCreateInfo))
    {
        delete render_pass;
        return false;
    }
    pRenderPass = render_pass;
    return true;
}

void DX12RHI::DestroyRenderPass(RHIRenderPass* renderPass)
{
    if (renderPass == nullptr)
    {
        return;
    }
    DX12RenderPass* dx12_render_pass = static_cast<DX12RenderPass*>(renderPass);
    delete dx12_render_pass;
}

void DX12RHI::TransitionSubpassAttachments(const RHISubpassDescription& subpass, bool for_color_output)
{
    if (m_ActiveFramebuffer == nullptr || m_ActiveRenderPass == nullptr)
    {
        return;
    }

    const auto& attachments = m_ActiveRenderPass->getAttachments();

    auto transition_attachment = [&](uint32_t attachment_index, D3D12_RESOURCE_STATES state) {
        DX12ImageView* view = m_ActiveFramebuffer->getAttachmentView(attachment_index);
        if (view == nullptr)
        {
            return;
        }
        if (auto* dx12_view = static_cast<DX12ImageView*>(view); dx12_view->isSwapchainView())
        {
            const uint32_t swapchain_index = dx12_view->getSwapchainBackBufferIndex();
            if (swapchain_index < k_max_frames_in_flight)
            {
                TransitionSwapchainBuffer(swapchain_index, state);
            }
            return;
        }
        if (view->getImage() != nullptr)
        {
            TransitionImage(static_cast<DX12Image*>(view->getImage()), state);
        }
    };

    if (for_color_output)
    {
        if (subpass.pColorAttachments != nullptr)
        {
            for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i)
            {
                const uint32_t attachment_index = subpass.pColorAttachments[i].attachment;
                transition_attachment(attachment_index, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
        if (subpass.pDepthStencilAttachment != nullptr)
        {
            transition_attachment(subpass.pDepthStencilAttachment->attachment,
                                  D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }
    else if (subpass.pInputAttachments != nullptr)
    {
        for (uint32_t i = 0; i < subpass.inputAttachmentCount; ++i)
        {
            const uint32_t attachment_index = subpass.pInputAttachments[i].attachment;
            transition_attachment(attachment_index,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }

    (void)attachments;
}

void DX12RHI::ApplySubpassDependencies(uint32_t src_subpass, uint32_t dst_subpass)
{
    if (m_ActiveRenderPass == nullptr || m_ActiveFramebuffer == nullptr)
    {
        return;
    }

    const RHISubpassDescription* dst = m_ActiveRenderPass->getSubpass(dst_subpass);
    if (dst == nullptr)
    {
        return;
    }

    for (const RHISubpassDependency& dependency : m_ActiveRenderPass->getDependencies())
    {
        if (dependency.dstSubpass != dst_subpass)
        {
            continue;
        }
        const bool external_src = (dependency.srcSubpass == RHI_SUBPASS_EXTERNAL);
        if (!external_src && dependency.srcSubpass != src_subpass)
        {
            continue;
        }
        if (external_src && src_subpass != UINT32_MAX)
        {
            continue;
        }

        // Ensure dst subpass inputs are shader-readable after the dependency edge.
        TransitionSubpassAttachments(*dst, false);
    }
}

void DX12RHI::BindSubpassRenderTargets(const RHISubpassDescription& subpass,
                                       const RHIRenderPassBeginInfo* begin_info,
                                       bool clear_on_load)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || m_ActiveFramebuffer == nullptr || m_ActiveRenderPass == nullptr)
    {
        return;
    }

    const auto& attachment_descs = m_ActiveRenderPass->getAttachments();

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    if (subpass.pColorAttachments != nullptr)
    {
        for (uint32_t color_index = 0; color_index < subpass.colorAttachmentCount; ++color_index)
        {
            const uint32_t attachment_index = subpass.pColorAttachments[color_index].attachment;
            DX12ImageView* view = m_ActiveFramebuffer->getAttachmentView(attachment_index);
            if (view == nullptr || !view->hasRenderTargetHandle())
            {
                continue;
            }

            const D3D12_CPU_DESCRIPTOR_HANDLE rtv = view->getRenderTargetHandle();
            rtvs.push_back(rtv);

            // Per-Vulkan spec: only clear on FIRST use of the attachment.
            const bool already_used =
                (attachment_index < 64) &&
                (m_ActiveSubpassAttachmentsUsed & (1ULL << attachment_index));
            if (clear_on_load && attachment_index < attachment_descs.size() &&
                attachment_descs[attachment_index].loadOp == RHI_ATTACHMENT_LOAD_OP_CLEAR &&
                !already_used)
            {
                float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (begin_info != nullptr && begin_info->pClearValues != nullptr &&
                    attachment_index < begin_info->clearValueCount)
                {
                    clear_color[0] = begin_info->pClearValues[attachment_index].color.float32[0];
                    clear_color[1] = begin_info->pClearValues[attachment_index].color.float32[1];
                    clear_color[2] = begin_info->pClearValues[attachment_index].color.float32[2];
                    clear_color[3] = begin_info->pClearValues[attachment_index].color.float32[3];
                }
                m_CommandLists[m_CurrentFrameIndex]->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
            }
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE* dsv_ptr = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
    if (subpass.pDepthStencilAttachment != nullptr)
    {
        DX12ImageView* depth_view =
            m_ActiveFramebuffer->getAttachmentView(subpass.pDepthStencilAttachment->attachment);
        if (depth_view != nullptr && depth_view->hasDepthStencilHandle())
        {
            dsv = depth_view->getDepthStencilHandle();
            dsv_ptr = &dsv;

            const uint32_t depth_attachment_index = subpass.pDepthStencilAttachment->attachment;
            // Per-Vulkan spec: only clear on FIRST use of the attachment.
            const bool already_used =
                (depth_attachment_index < 64) &&
                (m_ActiveSubpassAttachmentsUsed & (1ULL << depth_attachment_index));
            if (clear_on_load &&
                depth_attachment_index < attachment_descs.size() &&
                attachment_descs[depth_attachment_index].loadOp == RHI_ATTACHMENT_LOAD_OP_CLEAR &&
                !already_used)
            {
                float depth = 1.0f;
                UINT8 stencil = 0;
                if (begin_info != nullptr && begin_info->pClearValues != nullptr &&
                    depth_attachment_index < begin_info->clearValueCount)
                {
                    depth = begin_info->pClearValues[depth_attachment_index].depthStencil.depth;
                    stencil = static_cast<UINT8>(
                        begin_info->pClearValues[depth_attachment_index].depthStencil.stencil);
                }
                m_CommandLists[m_CurrentFrameIndex]->ClearDepthStencilView(
                    dsv, D3D12_CLEAR_FLAG_DEPTH, depth, stencil, 0, nullptr);
            }
        }
    }

    if (!rtvs.empty())
    {
        m_CommandLists[m_CurrentFrameIndex]->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),
                                                                rtvs.data(),
                                                                FALSE,
                                                                dsv_ptr);
    }
    else if (dsv_ptr != nullptr)
    {
        m_CommandLists[m_CurrentFrameIndex]->OMSetRenderTargets(0, nullptr, FALSE, dsv_ptr);
    }

    // Mark all attachments referenced by this subpass as "used" so that
    // subsequent subpasses will NOT clear them again.
    auto mark_used = [&](uint32_t idx) {
        if (idx != RHI_ATTACHMENT_UNUSED && idx < 64)
        {
            m_ActiveSubpassAttachmentsUsed |= (1ULL << idx);
        }
    };
    if (subpass.pColorAttachments != nullptr)
    {
        for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i)
        {
            mark_used(subpass.pColorAttachments[i].attachment);
        }
    }
    if (subpass.pDepthStencilAttachment != nullptr)
    {
        mark_used(subpass.pDepthStencilAttachment->attachment);
    }
    if (subpass.pInputAttachments != nullptr)
    {
        for (uint32_t i = 0; i < subpass.inputAttachmentCount; ++i)
        {
            mark_used(subpass.pInputAttachments[i].attachment);
        }
    }
}

bool DX12RHI::CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler)
{
    pSampler = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }

    D3D12_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = ToDX12Filter(pCreateInfo);
    sampler_desc.AddressU = ToDX12AddressMode(pCreateInfo->addressModeU);
    sampler_desc.AddressV = ToDX12AddressMode(pCreateInfo->addressModeV);
    sampler_desc.AddressW = ToDX12AddressMode(pCreateInfo->addressModeW);
    sampler_desc.MipLODBias = pCreateInfo->mipLodBias;
    sampler_desc.MaxAnisotropy = pCreateInfo->anisotropyEnable ? static_cast<UINT>(std::max(1.0f, pCreateInfo->maxAnisotropy)) : 1;
    sampler_desc.ComparisonFunc = pCreateInfo->compareEnable ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
    sampler_desc.BorderColor[0] = 0.0f;
    sampler_desc.BorderColor[1] = 0.0f;
    sampler_desc.BorderColor[2] = 0.0f;
    sampler_desc.BorderColor[3] = 1.0f;
    sampler_desc.MinLOD = pCreateInfo->minLod;
    sampler_desc.MaxLOD = pCreateInfo->maxLod > 0.0f ? pCreateInfo->maxLod : D3D12_FLOAT32_MAX;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle {};
    if (m_Device && AllocateSamplerDescriptor(cpu_handle, gpu_handle))
    {
        m_Device->CreateSampler(&sampler_desc, cpu_handle);
    }

    DX12Sampler* sampler = new DX12Sampler();
    sampler->setDesc(sampler_desc, cpu_handle, gpu_handle);
    pSampler = sampler;
    return true;
}

bool DX12RHI::CreateSemaphore(const RHISemaphoreCreateInfo* pCreateInfo, RHISemaphore*& pSemaphore)
{
    // TODO: Implement DX12 createSemaphore
    return false;
}

bool DX12RHI::WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFence, RHIBool32 waitAll, uint64_t timeout)
{
    WaitForFences();
    return true;
}

bool DX12RHI::ResetFencesPFN(uint32_t fenceCount, RHIFence* const* pFences)
{
    return true;
}

bool DX12RHI::ResetCommandPoolPFN(RHICommandPool* commandPool, RHICommandPoolResetFlags flags)
{
    ResetCommandPool();
    return true;
}

bool DX12RHI::BeginCommandBufferPFN(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return false;
    }
    HRESULT hr = m_CommandLists[m_CurrentFrameIndex]->Reset(m_CommandAllocators[m_CurrentFrameIndex].Get(), nullptr);
    return CheckDX12(hr, "DX12 command list reset failed");
}

bool DX12RHI::EndCommandBufferPFN(RHICommandBuffer* commandBuffer)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return false;
    }
    return CheckDX12(m_CommandLists[m_CurrentFrameIndex]->Close(), "DX12 command list close failed");
}

void DX12RHI::CmdBeginRenderPassPFN(RHICommandBuffer* commandBuffer,
                                    const RHIRenderPassBeginInfo* pRenderPassBegin,
                                    RHISubpassContents contents)
{
    (void)commandBuffer;
    (void)contents;

    if (!m_CommandLists[m_CurrentFrameIndex] || pRenderPassBegin == nullptr || pRenderPassBegin->framebuffer == nullptr)
    {
        return;
    }

    m_ActiveFramebuffer = static_cast<DX12Framebuffer*>(pRenderPassBegin->framebuffer);
    m_ActiveRenderPass = static_cast<DX12RenderPass*>(pRenderPassBegin->renderPass);
    m_ActiveSubpassIndex = 0;
    m_ActiveSubpassAttachmentsUsed = 0;

    m_ActiveRenderPassClearValueCount = 0;
    if (pRenderPassBegin->pClearValues != nullptr && pRenderPassBegin->clearValueCount > 0)
    {
        m_ActiveRenderPassClearValueCount =
            std::min(pRenderPassBegin->clearValueCount, k_max_stored_render_pass_clear_values);
        for (uint32_t i = 0; i < m_ActiveRenderPassClearValueCount; ++i)
        {
            m_ActiveRenderPassClearValues[i] = pRenderPassBegin->pClearValues[i];
        }
    }

    const RHISubpassDescription* subpass = m_ActiveRenderPass->getSubpass(0);
    if (subpass == nullptr)
    {
        return;
    }

    ApplySubpassDependencies(UINT32_MAX, 0);
    TransitionSubpassAttachments(*subpass, true);
    BindSubpassRenderTargets(*subpass, pRenderPassBegin, true);

    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = static_cast<float>(pRenderPassBegin->renderArea.offset.x);
    viewport.TopLeftY = static_cast<float>(pRenderPassBegin->renderArea.offset.y);
    viewport.Width = static_cast<float>(pRenderPassBegin->renderArea.extent.width);
    viewport.Height = static_cast<float>(pRenderPassBegin->renderArea.extent.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_CommandLists[m_CurrentFrameIndex]->RSSetViewports(1, &viewport);

    D3D12_RECT scissor = {};
    scissor.left = pRenderPassBegin->renderArea.offset.x;
    scissor.top = pRenderPassBegin->renderArea.offset.y;
    scissor.right = pRenderPassBegin->renderArea.offset.x + static_cast<LONG>(pRenderPassBegin->renderArea.extent.width);
    scissor.bottom = pRenderPassBegin->renderArea.offset.y + static_cast<LONG>(pRenderPassBegin->renderArea.extent.height);
    m_CommandLists[m_CurrentFrameIndex]->RSSetScissorRects(1, &scissor);
}

void DX12RHI::CmdNextSubpassPFN(RHICommandBuffer* commandBuffer, RHISubpassContents contents)
{
    (void)commandBuffer;
    (void)contents;

    if (!m_CommandLists[m_CurrentFrameIndex] || m_ActiveRenderPass == nullptr || m_ActiveFramebuffer == nullptr)
    {
        return;
    }

    const uint32_t next_subpass = m_ActiveSubpassIndex + 1;
    const RHISubpassDescription* subpass = m_ActiveRenderPass->getSubpass(next_subpass);
    if (subpass == nullptr)
    {
        return;
    }

    ApplySubpassDependencies(m_ActiveSubpassIndex, next_subpass);
    TransitionSubpassAttachments(*subpass, false);
    TransitionSubpassAttachments(*subpass, true);

    RHIRenderPassBeginInfo begin_for_clear {};
    const RHIRenderPassBeginInfo* begin_ptr = nullptr;
    if (m_ActiveRenderPassClearValueCount > 0)
    {
        begin_for_clear.clearValueCount = m_ActiveRenderPassClearValueCount;
        begin_for_clear.pClearValues = m_ActiveRenderPassClearValues;
        begin_ptr = &begin_for_clear;
    }
    BindSubpassRenderTargets(*subpass, begin_ptr, true);
    m_ActiveSubpassIndex = next_subpass;
}

void DX12RHI::CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer)
{
    (void)commandBuffer;

    if (m_ActiveFramebuffer != nullptr && m_ActiveRenderPass != nullptr)
    {
        const auto& attachments = m_ActiveRenderPass->getAttachments();
        for (uint32_t attachment_index = 0; attachment_index < attachments.size(); ++attachment_index)
        {
            DX12ImageView* view = static_cast<DX12ImageView*>(m_ActiveFramebuffer->getAttachmentView(attachment_index));
            if (view == nullptr)
            {
                continue;
            }

            const RHIImageLayout final_layout = attachments[attachment_index].finalLayout;
            const D3D12_RESOURCE_STATES final_state = ToDX12ResourceState(final_layout);

            if (view->isSwapchainView())
            {
                const uint32_t swapchain_index = view->getSwapchainBackBufferIndex();
                if (swapchain_index < k_max_frames_in_flight)
                {
                    TransitionSwapchainBuffer(swapchain_index, final_state);
                }
                continue;
            }

            if (view->getImage() != nullptr)
            {
                TransitionImage(view->getImage(), final_state);
            }
        }
    }

    m_ActiveFramebuffer = nullptr;
    m_ActiveRenderPass = nullptr;
    m_ActiveSubpassIndex = 0;
    m_ActiveRenderPassClearValueCount = 0;
}

void DX12RHI::CmdBindPipelinePFN(RHICommandBuffer* commandBuffer,
                                 RHIPipelineBindPoint pipelineBindPoint,
                                 RHIPipeline* pipeline)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || pipeline == nullptr)
    {
        return;
    }

    if (pipelineBindPoint == RHI_PIPELINE_BIND_POINT_GRAPHICS)
    {
        m_CurrentGraphicsPipeline = static_cast<DX12Pipeline*>(pipeline);
        if (m_CurrentGraphicsPipeline->getPipelineState())
        {
            m_CommandLists[m_CurrentFrameIndex]->SetPipelineState(m_CurrentGraphicsPipeline->getPipelineState());
        }
        else
        {
            LOG_ERROR(ZRender, "CmdBindPipelinePFN: GRAPHICS pipeline has NULL PSO! pipeline=0x{:016X} — SetPipelineState SKIPPED",
                      (uint64_t)(uintptr_t)pipeline);
        }
        if (m_CurrentGraphicsPipeline->getPipelineLayout() && m_CurrentGraphicsPipeline->getPipelineLayout()->getRootSignature())
        {
            m_CommandLists[m_CurrentFrameIndex]->SetGraphicsRootSignature(
                m_CurrentGraphicsPipeline->getPipelineLayout()->getRootSignature());
        }
        m_CommandLists[m_CurrentFrameIndex]->IASetPrimitiveTopology(m_CurrentGraphicsPipeline->getPrimitiveTopology());
    }
    else if (pipelineBindPoint == RHI_PIPELINE_BIND_POINT_COMPUTE)
    {
        m_CurrentComputePipeline = static_cast<DX12Pipeline*>(pipeline);
        if (m_CurrentComputePipeline->getPipelineState())
        {
            m_CommandLists[m_CurrentFrameIndex]->SetPipelineState(m_CurrentComputePipeline->getPipelineState());
        }
        if (m_CurrentComputePipeline->getPipelineLayout() && m_CurrentComputePipeline->getPipelineLayout()->getRootSignature())
        {
            m_CommandLists[m_CurrentFrameIndex]->SetComputeRootSignature(
                m_CurrentComputePipeline->getPipelineLayout()->getRootSignature());
        }
    }
}

void DX12RHI::CmdSetViewportPFN(RHICommandBuffer* commandBuffer,
                                uint32_t firstViewport,
                                uint32_t viewportCount,
                                const RHIViewport* pViewports)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || pViewports == nullptr || viewportCount == 0)
    {
        return;
    }

    std::vector<D3D12_VIEWPORT> viewports(viewportCount);
    for (uint32_t i = 0; i < viewportCount; ++i)
    {
        viewports[i].TopLeftX = pViewports[i].x;
        viewports[i].TopLeftY = pViewports[i].y;
        viewports[i].Width = pViewports[i].width;
        viewports[i].Height = pViewports[i].height;
        viewports[i].MinDepth = pViewports[i].minDepth;
        viewports[i].MaxDepth = pViewports[i].maxDepth;
    }
    m_CommandLists[m_CurrentFrameIndex]->RSSetViewports(viewportCount, viewports.data());
}

void DX12RHI::CmdSetScissorPFN(RHICommandBuffer* commandBuffer,
                               uint32_t firstScissor,
                               uint32_t scissorCount,
                               const RHIRect2D* pScissors)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || pScissors == nullptr || scissorCount == 0)
    {
        return;
    }

    std::vector<D3D12_RECT> scissors(scissorCount);
    for (uint32_t i = 0; i < scissorCount; ++i)
    {
        scissors[i].left = pScissors[i].offset.x;
        scissors[i].top = pScissors[i].offset.y;
        scissors[i].right = pScissors[i].offset.x + static_cast<LONG>(pScissors[i].extent.width);
        scissors[i].bottom = pScissors[i].offset.y + static_cast<LONG>(pScissors[i].extent.height);
    }
    m_CommandLists[m_CurrentFrameIndex]->RSSetScissorRects(scissorCount, scissors.data());
}

void DX12RHI::CmdBindVertexBuffersPFN(RHICommandBuffer* commandBuffer,
                                      uint32_t firstBinding,
                                      uint32_t bindingCount,
                                      RHIBuffer* const* pBuffers,
                                      const RHIDeviceSize* pOffsets)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || pBuffers == nullptr || bindingCount == 0)
    {
        return;
    }

    std::vector<D3D12_VERTEX_BUFFER_VIEW> views(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i)
    {
        DX12Buffer* buffer = static_cast<DX12Buffer*>(pBuffers[i]);
        if (buffer == nullptr || buffer->getResource() == nullptr)
        {
            continue;
        }

        const RHIDeviceSize offset = pOffsets ? pOffsets[i] : 0;
        const uint32_t binding = firstBinding + i;
        const uint32_t stride = m_CurrentGraphicsPipeline ? m_CurrentGraphicsPipeline->getVertexStride(binding) : 0;
        views[i].BufferLocation = buffer->getResource()->GetGPUVirtualAddress() + offset;
        views[i].SizeInBytes = static_cast<UINT>(buffer->getSize() > offset ? buffer->getSize() - offset : 0);
        views[i].StrideInBytes = stride;
    }

    m_CommandLists[m_CurrentFrameIndex]->IASetVertexBuffers(firstBinding, bindingCount, views.data());
}

void DX12RHI::CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer,
                                    RHIBuffer* buffer,
                                    RHIDeviceSize offset,
                                    RHIIndexType indexType)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || buffer == nullptr)
    {
        return;
    }

    DX12Buffer* dx12_buffer = static_cast<DX12Buffer*>(buffer);
    if (dx12_buffer == nullptr || dx12_buffer->getResource() == nullptr)
    {
        return;
    }

    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = dx12_buffer->getResource()->GetGPUVirtualAddress() + offset;
    view.SizeInBytes = static_cast<UINT>(dx12_buffer->getSize() > offset ? dx12_buffer->getSize() - offset : 0);
    view.Format = ToDX12IndexFormat(indexType);
    m_CommandLists[m_CurrentFrameIndex]->IASetIndexBuffer(&view);
}

void DX12RHI::CmdBindDescriptorSetsPFN(RHICommandBuffer* commandBuffer,
                                       RHIPipelineBindPoint pipelineBindPoint,
                                       RHIPipelineLayout* layout,
                                       uint32_t firstSet,
                                       uint32_t descriptorSetCount,
                                       const RHIDescriptorSet* const* pDescriptorSets,
                                       uint32_t dynamicOffsetCount,
                                       const uint32_t* pDynamicOffsets)
{
    (void)commandBuffer;
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    ID3D12DescriptorHeap* heaps[2] = {};
    UINT heap_count = 0;
    if (m_OverlayDescriptorBindActive && m_BindlessSupported && m_BindlessTextureManager)
    {
        auto* bindless_mgr = static_cast<DX12BindlessTextureManager*>(m_BindlessTextureManager.get());
        heaps[heap_count++] = bindless_mgr->getDescriptorHeap();
        if (m_SamplerHeap)
        {
            heaps[heap_count++] = m_SamplerHeap.Get();
        }
    }
    else
    {
        if (m_CbvSrvUavHeap != nullptr)
        {
            heaps[heap_count++] = m_CbvSrvUavHeap.Get();
        }
        if (m_SamplerHeap != nullptr)
        {
            heaps[heap_count++] = m_SamplerHeap.Get();
        }
    }
    if (heap_count > 0)
    {
        m_CommandLists[m_CurrentFrameIndex]->SetDescriptorHeaps(heap_count, heaps);
    }

    DX12PipelineLayout* pipeline_layout = static_cast<DX12PipelineLayout*>(layout);
    if (pipeline_layout == nullptr || pipeline_layout->getRootSignature() == nullptr || pDescriptorSets == nullptr)
    {
        return;
    }

    if (pipelineBindPoint == RHI_PIPELINE_BIND_POINT_COMPUTE)
    {
        m_CommandLists[m_CurrentFrameIndex]->SetComputeRootSignature(pipeline_layout->getRootSignature());
    }
    else
    {
        m_CommandLists[m_CurrentFrameIndex]->SetGraphicsRootSignature(pipeline_layout->getRootSignature());
    }

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> dynamic_offset_by_binding;
    uint32_t dynamic_offset_cursor = 0;
    for (uint32_t set_index = 0; set_index < descriptorSetCount; ++set_index)
    {
        const uint32_t set_number = firstSet + set_index;
        const DX12DescriptorSet* descriptor_set = static_cast<const DX12DescriptorSet*>(pDescriptorSets[set_index]);
        if (descriptor_set == nullptr || descriptor_set->getLayout() == nullptr)
        {
            continue;
        }

        std::vector<RHIDescriptorSetLayoutBinding> sorted_bindings = descriptor_set->getLayout()->getBindings();
        std::sort(sorted_bindings.begin(),
                  sorted_bindings.end(),
                  [](const RHIDescriptorSetLayoutBinding& lhs, const RHIDescriptorSetLayoutBinding& rhs) {
                      return lhs.binding < rhs.binding;
                  });

        for (const RHIDescriptorSetLayoutBinding& layout_binding : sorted_bindings)
        {
            if (!IsDynamicDescriptorType(layout_binding.descriptorType))
            {
                continue;
            }
            const uint32_t dynamic_offset =
                (pDynamicOffsets != nullptr && dynamic_offset_cursor < dynamicOffsetCount) ? pDynamicOffsets[dynamic_offset_cursor++] : 0;
            dynamic_offset_by_binding[{set_number, layout_binding.binding}] = dynamic_offset;
        }
    }

    for (const DX12RootParameterBinding& binding : pipeline_layout->getBindings())
    {
        if (binding.set < firstSet || binding.set >= firstSet + descriptorSetCount)
        {
            continue;
        }

        const uint32_t descriptor_set_index = binding.set - firstSet;
        const DX12DescriptorSet* descriptor_set =
            static_cast<const DX12DescriptorSet*>(pDescriptorSets[descriptor_set_index]);
        if (descriptor_set == nullptr)
        {
            continue;
        }

        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle {};
        bool has_handle = false;

        if (binding.heap_kind == DX12DescriptorHeapKind::Sampler)
        {
            has_handle = descriptor_set->getSamplerHandle(binding.binding, gpu_handle);
        }
        else
        {
            const DX12DescriptorBufferBinding* buffer_binding = descriptor_set->getBufferBinding(binding.binding);
            const auto dynamic_iter = dynamic_offset_by_binding.find({binding.set, binding.binding});
            const uint32_t dynamic_offset =
                dynamic_iter != dynamic_offset_by_binding.end() ? dynamic_iter->second : 0;
            if (buffer_binding != nullptr && IsDynamicDescriptorType(buffer_binding->type))
            {
                has_handle = CreateDynamicBufferGpuHandle(*buffer_binding, dynamic_offset, gpu_handle);
            }
            else
            {
                has_handle = descriptor_set->getCbvSrvUavHandle(binding.binding, gpu_handle);
            }
        }

        if (!has_handle || gpu_handle.ptr == 0)
        {
            continue;
        }

        if (pipelineBindPoint == RHI_PIPELINE_BIND_POINT_COMPUTE)
        {
            m_CommandLists[m_CurrentFrameIndex]->SetComputeRootDescriptorTable(binding.root_parameter_index, gpu_handle);
        }
        else
        {
            m_CommandLists[m_CurrentFrameIndex]->SetGraphicsRootDescriptorTable(binding.root_parameter_index,
                                                                                gpu_handle);
        }
    }
}

void DX12RHI::CmdSetBindlessIndexPFN(RHICommandBuffer* /*commandBuffer*/,
                                     RHIPipelineBindPoint pipelineBindPoint,
                                     RHIPipelineLayout* layout,
                                     uint32_t packed_index)
{
    // PR6: bindless-index push.
    //
    // Contract:
    // - 'layout' MUST be the same DX12PipelineLayout that was bound to
    //   the command list by a preceding cmdBindDescriptorSetsPFN /
    //   cmdBindPipeline. We do NOT call SetGraphicsRootSignature here
    //   on purpose -- duplicating that call from cmdBindDescriptorSets
    //   would silently invalidate any descriptor tables set in between
    //   on the same root signature (D3D12 spec: changing the bound
    //   root signature resets all root parameters). cmdBindDescriptors
    //   is therefore the single source of truth for "the active root
    //   signature is now this layout's"; we just push 32 bits onto it.
    // - On a non-bindless layout this is a no-op (matches the base
    //   class default), so legacy materials calling this by mistake
    //   degrade silently rather than blow away their root parameters.
    if (!m_CommandLists[m_CurrentFrameIndex] || layout == nullptr)
    {
        return;
    }
    DX12PipelineLayout* pipeline_layout = static_cast<DX12PipelineLayout*>(layout);
    if (!pipeline_layout->usesBindless())
    {
        return;
    }
    const UINT root_parameter_index = pipeline_layout->getBindlessRootConstantParameterIndex();
    if (pipelineBindPoint == RHI_PIPELINE_BIND_POINT_COMPUTE)
    {
        m_CommandLists[m_CurrentFrameIndex]->SetComputeRoot32BitConstant(
            root_parameter_index, static_cast<UINT>(packed_index), 0);
    }
    else
    {
        m_CommandLists[m_CurrentFrameIndex]->SetGraphicsRoot32BitConstant(
            root_parameter_index, static_cast<UINT>(packed_index), 0);
    }
}

void DX12RHI::CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer,
                                uint32_t indexCount,
                                uint32_t instanceCount,
                                uint32_t firstIndex,
                                int32_t vertexOffset,
                                uint32_t firstInstance)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    m_CommandLists[m_CurrentFrameIndex]->DrawIndexedInstanced(
        indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void DX12RHI::CmdClearAttachmentsPFN(RHICommandBuffer* commandBuffer,
                                     uint32_t attachmentCount,
                                     const RHIClearAttachment* pAttachments,
                                     uint32_t rectCount,
                                     const RHIClearRect* pRects)
{
    (void)commandBuffer;

    ID3D12GraphicsCommandList* command_list = m_CommandLists[m_CurrentFrameIndex].Get();
    if (command_list == nullptr || m_ActiveRenderPass == nullptr || m_ActiveFramebuffer == nullptr ||
        pAttachments == nullptr || pRects == nullptr || attachmentCount == 0 || rectCount == 0)
    {
        return;
    }

    const RHISubpassDescription* subpass = m_ActiveRenderPass->getSubpass(m_ActiveSubpassIndex);
    if (subpass == nullptr || subpass->pColorAttachments == nullptr || subpass->colorAttachmentCount == 0)
    {
        return;
    }

    for (uint32_t attachment_index = 0; attachment_index < attachmentCount; ++attachment_index)
    {
        const RHIClearAttachment& clear_attachment = pAttachments[attachment_index];
        if ((clear_attachment.aspectMask & RHI_IMAGE_ASPECT_COLOR_BIT) == 0)
        {
            continue;
        }
        if (clear_attachment.colorAttachment >= subpass->colorAttachmentCount)
        {
            continue;
        }

        const uint32_t framebuffer_attachment_index =
            subpass->pColorAttachments[clear_attachment.colorAttachment].attachment;
        DX12ImageView* view = m_ActiveFramebuffer->getAttachmentView(framebuffer_attachment_index);
        if (view == nullptr || !view->hasRenderTargetHandle())
        {
            continue;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = view->getRenderTargetHandle();
        const float clear_color[4] = {clear_attachment.clearValue.color.float32[0],
                                      clear_attachment.clearValue.color.float32[1],
                                      clear_attachment.clearValue.color.float32[2],
                                      clear_attachment.clearValue.color.float32[3]};

        for (uint32_t rect_index = 0; rect_index < rectCount; ++rect_index)
        {
            const RHIRect2D& rect = pRects[rect_index].rect;
            D3D12_RECT d3d_rect {};
            d3d_rect.left = rect.offset.x;
            d3d_rect.top = rect.offset.y;
            d3d_rect.right = rect.offset.x + static_cast<LONG>(rect.extent.width);
            d3d_rect.bottom = rect.offset.y + static_cast<LONG>(rect.extent.height);
            command_list->ClearRenderTargetView(rtv, clear_color, 1, &d3d_rect);
        }
    }
}

bool DX12RHI::BeginCommandBuffer(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo* pBeginInfo)
{
    return BeginCommandBufferPFN(commandBuffer, pBeginInfo);
}

void DX12RHI::CmdCopyImageToBuffer(RHICommandBuffer* commandBuffer,
                                   RHIImage* srcImage,
                                   RHIImageLayout srcImageLayout,
                                   RHIBuffer* dstBuffer,
                                   uint32_t regionCount,
                                   const RHIBufferImageCopy* pRegions)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || srcImage == nullptr || dstBuffer == nullptr || pRegions == nullptr)
    {
        return;
    }

    DX12Image* src_image = static_cast<DX12Image*>(srcImage);
    DX12Buffer* dst_buffer = static_cast<DX12Buffer*>(dstBuffer);
    if (src_image->getResource() == nullptr || dst_buffer->getResource() == nullptr)
    {
        return;
    }

    const uint32_t bytes_per_pixel = GetDXGIFormatBytesPerPixel(src_image->getFormat());
    for (uint32_t i = 0; i < regionCount; ++i)
    {
        const RHIBufferImageCopy& region = pRegions[i];
        const uint32_t mip = region.imageSubresource.mipLevel;
        const uint32_t array_layer = region.imageSubresource.baseArrayLayer;
        const uint32_t subresource = mip + array_layer * src_image->getMipLevels();
        TransitionImage(src_image, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE);

        const uint32_t copy_width = region.imageExtent.width;
        const uint32_t copy_height = region.imageExtent.height;
        const uint32_t row_width = region.bufferRowLength != 0 ? region.bufferRowLength : copy_width;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = src_image->getResource();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = subresource;

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = dst_buffer->getResource();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = region.bufferOffset;
        dst.PlacedFootprint.Footprint.Format = src_image->getFormat();
        dst.PlacedFootprint.Footprint.Width = copy_width;
        dst.PlacedFootprint.Footprint.Height = copy_height;
        dst.PlacedFootprint.Footprint.Depth = std::max<uint32_t>(region.imageExtent.depth, 1);
        dst.PlacedFootprint.Footprint.RowPitch = AlignTo(row_width * bytes_per_pixel, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        D3D12_BOX src_box = {};
        src_box.left = region.imageOffset.x;
        src_box.top = region.imageOffset.y;
        src_box.front = region.imageOffset.z;
        src_box.right = region.imageOffset.x + copy_width;
        src_box.bottom = region.imageOffset.y + copy_height;
        src_box.back = region.imageOffset.z + std::max<uint32_t>(region.imageExtent.depth, 1);

        m_CommandLists[m_CurrentFrameIndex]->CopyTextureRegion(&dst, 0, 0, 0, &src, &src_box);
    }
}

void DX12RHI::CmdCopyImageToImage(RHICommandBuffer* commandBuffer,
                                  RHIImage* srcImage,
                                  RHIImageAspectFlagBits srcFlag,
                                  RHIImage* dstImage,
                                  RHIImageAspectFlagBits dstFlag,
                                  uint32_t width,
                                  uint32_t height)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || srcImage == nullptr || dstImage == nullptr)
    {
        return;
    }

    DX12Image* src_image = static_cast<DX12Image*>(srcImage);
    DX12Image* dst_image = static_cast<DX12Image*>(dstImage);
    if (src_image->getResource() == nullptr || dst_image->getResource() == nullptr)
    {
        return;
    }

    TransitionImage(src_image, 0, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionImage(dst_image, 0, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = src_image->getResource();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = dst_image->getResource();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_BOX src_box = {};
    src_box.left = 0;
    src_box.top = 0;
    src_box.front = 0;
    src_box.right = width;
    src_box.bottom = height;
    src_box.back = 1;
    m_CommandLists[m_CurrentFrameIndex]->CopyTextureRegion(&dst, 0, 0, 0, &src, &src_box);
}

void DX12RHI::CmdBlitImage(RHICommandBuffer* commandBuffer,
                           RHIImage* srcImage,
                           RHIImageLayout srcImageLayout,
                           RHIImage* dstImage,
                           RHIImageLayout dstImageLayout,
                           uint32_t srcX0,
                           uint32_t srcY0,
                           uint32_t srcX1,
                           uint32_t srcY1,
                           uint32_t dstX0,
                           uint32_t dstY0,
                           uint32_t dstX1,
                           uint32_t dstY1,
                           RHIFilter filter)
{
    if (!m_CommandLists[m_CurrentFrameIndex] || srcImage == nullptr || dstImage == nullptr)
    {
        return;
    }

    DX12Image* src_image = static_cast<DX12Image*>(srcImage);
    DX12Image* dst_image = static_cast<DX12Image*>(dstImage);
    if (src_image->getResource() == nullptr || dst_image->getResource() == nullptr)
    {
        return;
    }

    TransitionImage(src_image, 0, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionImage(dst_image, 0, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = src_image->getResource();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = dst_image->getResource();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_BOX src_box = {};
    src_box.left = srcX0;
    src_box.top = srcY0;
    src_box.front = 0;
    src_box.right = srcX1;
    src_box.bottom = srcY1;
    src_box.back = 1;

    m_CommandLists[m_CurrentFrameIndex]->CopyTextureRegion(&dst, dstX0, dstY0, 0, &src, &src_box);
}

void DX12RHI::CmdCopyBuffer(RHICommandBuffer* commandBuffer,
                            RHIBuffer* srcBuffer,
                            RHIBuffer* dstBuffer,
                            uint32_t regionCount,
                            RHIBufferCopy* pRegions)
{
    if (srcBuffer == nullptr || dstBuffer == nullptr || pRegions == nullptr || !m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    DX12Buffer* src = static_cast<DX12Buffer*>(srcBuffer);
    DX12Buffer* dst = static_cast<DX12Buffer*>(dstBuffer);
    for (uint32_t i = 0; i < regionCount; ++i)
    {
        m_CommandLists[m_CurrentFrameIndex]->CopyBufferRegion(dst->getResource(),
                                                              pRegions[i].dstOffset,
                                                              src->getResource(),
                                                              pRegions[i].srcOffset,
                                                              pRegions[i].size);
    }
}

void DX12RHI::CmdDraw(RHICommandBuffer* commandBuffer,
                      uint32_t vertexCount,
                      uint32_t instanceCount,
                      uint32_t firstVertex,
                      uint32_t firstInstance)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    m_CommandLists[m_CurrentFrameIndex]->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void DX12RHI::CmdDispatch(RHICommandBuffer* commandBuffer,
                          uint32_t groupCountX,
                          uint32_t groupCountY,
                          uint32_t groupCountZ)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    m_CommandLists[m_CurrentFrameIndex]->Dispatch(groupCountX, groupCountY, groupCountZ);
}

void DX12RHI::CmdDispatchIndirect(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset)
{
    DX12Buffer* dx12_buffer = static_cast<DX12Buffer*>(buffer);
    if (!m_CommandLists[m_CurrentFrameIndex] || dx12_buffer == nullptr || dx12_buffer->getResource() == nullptr ||
        !EnsureDispatchIndirectSignature())
    {
        return;
    }

    m_CommandLists[m_CurrentFrameIndex]->ExecuteIndirect(m_DispatchIndirectSignature.Get(),
                                                         1,
                                                         dx12_buffer->getResource(),
                                                         offset,
                                                         nullptr,
                                                         0);
}

void DX12RHI::CmdPipelineBarrier(RHICommandBuffer* commandBuffer,
                                 RHIPipelineStageFlags srcStageMask,
                                 RHIPipelineStageFlags dstStageMask,
                                 RHIDependencyFlags dependencyFlags,
                                 uint32_t memoryBarrierCount,
                                 const RHIMemoryBarrier* pMemoryBarriers,
                                 uint32_t bufferMemoryBarrierCount,
                                 const RHIBufferMemoryBarrier* pBufferMemoryBarriers,
                                 uint32_t imageMemoryBarrierCount,
                                 const RHIImageMemoryBarrier* pImageMemoryBarriers)
{
    if (!m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    if (memoryBarrierCount > 0 && pMemoryBarriers != nullptr)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        m_CommandLists[m_CurrentFrameIndex]->ResourceBarrier(1, &barrier);
    }

    for (uint32_t i = 0; i < bufferMemoryBarrierCount && pBufferMemoryBarriers != nullptr; ++i)
    {
        DX12Buffer* buffer = static_cast<DX12Buffer*>(pBufferMemoryBarriers[i].buffer);
        if (buffer == nullptr || buffer->getResource() == nullptr)
        {
            continue;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = buffer->getResource();
        m_CommandLists[m_CurrentFrameIndex]->ResourceBarrier(1, &barrier);
    }

    for (uint32_t i = 0; i < imageMemoryBarrierCount && pImageMemoryBarriers != nullptr; ++i)
    {
        const RHIImageMemoryBarrier& image_barrier = pImageMemoryBarriers[i];
        DX12Image* image = static_cast<DX12Image*>(image_barrier.image);
        if (image == nullptr)
        {
            continue;
        }

        D3D12_RESOURCE_STATES new_state = ToDX12ResourceState(image_barrier.newLayout);
        const uint32_t base_mip = image_barrier.subresourceRange.baseMipLevel;
        const uint32_t level_count = image_barrier.subresourceRange.levelCount == UINT32_MAX ? image->getMipLevels() - base_mip : std::max<uint32_t>(image_barrier.subresourceRange.levelCount, 1);
        const uint32_t base_layer = image_barrier.subresourceRange.baseArrayLayer;
        const uint32_t layer_count = image_barrier.subresourceRange.layerCount == UINT32_MAX ? image->getArrayLayers() - base_layer : std::max<uint32_t>(image_barrier.subresourceRange.layerCount, 1);

        for (uint32_t layer = 0; layer < layer_count; ++layer)
        {
            for (uint32_t mip = 0; mip < level_count; ++mip)
            {
                const uint32_t subresource = (base_mip + mip) + (base_layer + layer) * image->getMipLevels();
                TransitionImage(image, subresource, new_state);
            }
        }
    }
}

bool DX12RHI::EndCommandBuffer(RHICommandBuffer* commandBuffer)
{
    return EndCommandBufferPFN(commandBuffer);
}

void DX12RHI::UpdateDescriptorSets(uint32_t descriptorWriteCount,
                                   const RHIWriteDescriptorSet* pDescriptorWrites,
                                   uint32_t descriptorCopyCount,
                                   const RHICopyDescriptorSet* pDescriptorCopies)
{
    if (pDescriptorWrites == nullptr)
    {
        return;
    }

    for (uint32_t write_index = 0; write_index < descriptorWriteCount; ++write_index)
    {
        const RHIWriteDescriptorSet& write = pDescriptorWrites[write_index];
        DX12DescriptorSet* descriptor_set = static_cast<DX12DescriptorSet*>(write.dstSet);
        if (descriptor_set == nullptr)
        {
            continue;
        }

        if (write.pImageInfo != nullptr)
        {
            RHIDescriptorImageInfo& image_info = *write.pImageInfo;
            DX12ImageView* image_view = static_cast<DX12ImageView*>(image_info.imageView);
            DX12Sampler* sampler = static_cast<DX12Sampler*>(image_info.sampler);

            if ((write.descriptorType == RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                 write.descriptorType == RHI_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                 write.descriptorType == RHI_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) &&
                image_view != nullptr && image_view->getImage() != nullptr)
            {
                // Allocate a fresh per-frame SRV slot and create the SRV there.
                // CreateImageView allocates SRVs from the per-frame CBV/SRV/UAV
                // heap partition. After WaitForFences resets the per-frame counter,
                // subsequent allocations in the same partition overwrite those SRVs.
                // Storing the image view's stale GPU handle would cause the GPU to
                // read wrong descriptor data (black textures). Instead, create a
                // fresh SRV in the current frame's partition — the same pattern
                // already used for buffer CBVs and storage image UAVs above.
                D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu {};
                D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu {};
                if (AllocateCbvSrvUavDescriptor(srv_cpu, srv_gpu))
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                    srv_desc.Format = image_view->getFormat();
                    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

                    switch (image_view->getViewType())
                    {
                        case RHI_IMAGE_VIEW_TYPE_CUBE:
                            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                            srv_desc.TextureCube.MostDetailedMip = 0;
                            srv_desc.TextureCube.MipLevels = image_view->getMipLevels();
                            srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
                            break;
                        case RHI_IMAGE_VIEW_TYPE_2D_ARRAY:
                            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                            srv_desc.Texture2DArray.MostDetailedMip = 0;
                            srv_desc.Texture2DArray.MipLevels = image_view->getMipLevels();
                            srv_desc.Texture2DArray.FirstArraySlice = 0;
                            srv_desc.Texture2DArray.ArraySize = image_view->getArrayCount();
                            break;
                        case RHI_IMAGE_VIEW_TYPE_2D:
                        default:
                            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                            srv_desc.Texture2D.MostDetailedMip = 0;
                            srv_desc.Texture2D.MipLevels = image_view->getMipLevels();
                            srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
                            break;
                    }

                    m_Device->CreateShaderResourceView(
                        image_view->getImage()->getResource(), &srv_desc, srv_cpu);
                    descriptor_set->setCbvSrvUavHandle(write.dstBinding, srv_gpu);
                }
            }
            else if (write.descriptorType == RHI_DESCRIPTOR_TYPE_STORAGE_IMAGE && image_view != nullptr &&
                     image_view->getImage() != nullptr)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle {};
                D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle {};
                if (AllocateCbvSrvUavDescriptor(cpu_handle, gpu_handle))
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
                    uav_desc.Format = ToDX12UavFormat(image_view->getFormat());
                    if (image_view->getViewType() == RHI_IMAGE_VIEW_TYPE_2D_ARRAY ||
                        image_view->getViewType() == RHI_IMAGE_VIEW_TYPE_CUBE ||
                        image_view->getViewType() == RHI_IMAGE_VIEW_TYPE_CUBE_ARRAY ||
                        image_view->getImage()->getArrayLayers() > 1)
                    {
                        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                        uav_desc.Texture2DArray.MipSlice = 0;
                        uav_desc.Texture2DArray.FirstArraySlice = 0;
                        uav_desc.Texture2DArray.ArraySize = image_view->getArrayCount();
                    }
                    else
                    {
                        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        uav_desc.Texture2D.MipSlice = 0;
                    }
                    m_Device->CreateUnorderedAccessView(image_view->getImage()->getResource(), nullptr, &uav_desc, cpu_handle);
                    descriptor_set->setCbvSrvUavHandle(write.dstBinding, gpu_handle);
                }
            }

            if ((write.descriptorType == RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                 write.descriptorType == RHI_DESCRIPTOR_TYPE_SAMPLER) &&
                sampler != nullptr)
            {
                const D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = sampler->getGpuHandle();
                if (sampler_gpu_handle.ptr != 0)
                {
                    descriptor_set->setSamplerHandle(write.dstBinding, sampler_gpu_handle);
                }
            }
        }

        if (write.pBufferInfo != nullptr)
        {
            RHIDescriptorBufferInfo& buffer_info = *write.pBufferInfo;
            DX12Buffer* buffer = static_cast<DX12Buffer*>(buffer_info.buffer);
            if (buffer == nullptr || buffer->getResource() == nullptr)
            {
                continue;
            }

            D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle {};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle {};
            if (!AllocateCbvSrvUavDescriptor(cpu_handle, gpu_handle))
            {
                continue;
            }

            const RHIDeviceSize buffer_range = buffer_info.range == RHI_WHOLE_SIZE ? buffer->getSize() - buffer_info.offset : buffer_info.range;

            if (write.descriptorType == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                write.descriptorType == RHI_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
                cbv_desc.BufferLocation = buffer->getResource()->GetGPUVirtualAddress() + buffer_info.offset;
                cbv_desc.SizeInBytes = AlignTo(static_cast<uint32_t>(buffer_range), 256);
                m_Device->CreateConstantBufferView(&cbv_desc, cpu_handle);
                descriptor_set->setCbvSrvUavHandle(write.dstBinding, gpu_handle);
                descriptor_set->setBufferBinding(
                    write.dstBinding, buffer, buffer_info.offset, buffer_info.range, write.descriptorType);
            }
            else if (write.descriptorType == RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                     write.descriptorType == RHI_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            {
                constexpr UINT kRenderMeshInstanceStride = 80u;
                const UINT stride =
                    (buffer_range >= kRenderMeshInstanceStride && (buffer_range % kRenderMeshInstanceStride) == 0)
                        ? kRenderMeshInstanceStride
                        : 4u;

                D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                srv_desc.Format = DXGI_FORMAT_UNKNOWN;
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv_desc.Buffer.FirstElement = static_cast<UINT>(buffer_info.offset / stride);
                srv_desc.Buffer.NumElements = static_cast<UINT>(std::max<RHIDeviceSize>(buffer_range / stride, 1));
                srv_desc.Buffer.StructureByteStride = stride;
                srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
                m_Device->CreateShaderResourceView(buffer->getResource(), &srv_desc, cpu_handle);
                descriptor_set->setCbvSrvUavHandle(write.dstBinding, gpu_handle);
                descriptor_set->setBufferBinding(
                    write.dstBinding, buffer, buffer_info.offset, buffer_info.range, write.descriptorType);
            }
        }
    }
}

bool DX12RHI::QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence)
{
    if (!m_CommandQueue || !m_CommandLists[m_CurrentFrameIndex])
    {
        return false;
    }

    ID3D12CommandList* command_lists[] = {m_CommandLists[m_CurrentFrameIndex].Get()};
    m_CommandQueue->ExecuteCommandLists(1, command_lists);

    const uint64_t next_fence_value = m_FenceValues[m_CurrentFrameIndex] + 1;
    if (CheckDX12(m_CommandQueue->Signal(m_Fences[m_CurrentFrameIndex].Get(), next_fence_value),
                  "DX12 queue submit signal failed"))
    {
        m_FenceValues[m_CurrentFrameIndex] = next_fence_value;
        WaitForFences();
    }
    return true;
}

bool DX12RHI::QueueWaitIdle(RHIQueue* queue)
{
    WaitForFences();
    return true;
}

void DX12RHI::ResetCommandPool()
{
    if (m_CommandAllocators[m_CurrentFrameIndex])
    {
        CheckDX12(m_CommandAllocators[m_CurrentFrameIndex]->Reset(), "DX12 command allocator reset failed");
    }
}

void DX12RHI::WaitForFences()
{
    if (!m_Fences[m_CurrentFrameIndex] || m_FenceEvent == nullptr)
    {
        return;
    }

    const uint64_t fence_value = m_FenceValues[m_CurrentFrameIndex];
    if (fence_value == 0)
    {
        m_NextCbvSrvUavDescriptorPerFrame[m_CurrentFrameIndex] = 0;
        return;
    }

    if (m_Fences[m_CurrentFrameIndex]->GetCompletedValue() < fence_value)
    {
        if (CheckDX12(m_Fences[m_CurrentFrameIndex]->SetEventOnCompletion(fence_value, m_FenceEvent),
                      "DX12 SetEventOnCompletion failed"))
        {
            WaitForSingleObject(m_FenceEvent, INFINITE);
        }
    }

    // GPU finished with this frame slot; reuse its transient legacy-heap descriptors.
    m_NextCbvSrvUavDescriptorPerFrame[m_CurrentFrameIndex] = 0;
}

void DX12RHI::GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties* pProperties)
{
    if (pProperties == nullptr)
    {
        return;
    }

    *pProperties = {};
    pProperties->limits.minUniformBufferOffsetAlignment = 256;
    pProperties->limits.minStorageBufferOffsetAlignment = 256;
    pProperties->limits.maxStorageBufferRange = 1u << 27;
    pProperties->limits.nonCoherentAtomSize = 256;
}

RHICommandBuffer* DX12RHI::GetCurrentCommandBuffer() const
{
    return m_CurrentCommandBuffer;
}

RHICommandBuffer* const* DX12RHI::GetCommandBufferList() const
{
    return m_CommandBuffers;
}

RHICommandPool* DX12RHI::GetCommandPoor() const
{
    return m_RhiCommandPool;
}

RHIDescriptorPool* DX12RHI::GetDescriptorPoor() const
{
    return m_DescriptorPool;
}

RHIFence* const* DX12RHI::GetFenceList() const
{
    return m_RhiIsFrameInFlightFences;
}

QueueFamilyIndices DX12RHI::GetQueueFamilyIndices() const
{
    return m_QueueIndices;
}

RHIQueue* DX12RHI::GetGraphicsQueue() const
{
    return m_GraphicsQueue;
}

RHIQueue* DX12RHI::GetComputeQueue() const
{
    return m_ComputeQueue;
}

RHISwapChainDesc DX12RHI::GetSwapchainInfo()
{
    RHISwapChainDesc desc;
    desc.image_format = m_SwapchainImageFormat;
    desc.extent = m_SwapchainExtent;
    desc.viewport = m_Viewports;
    desc.viewport_count = m_ViewportCount;
    desc.scissor = m_Scissors;
    desc.imageViews = m_SwapchainImageviews;
    return desc;
}

RHIDepthImageDesc DX12RHI::GetDepthImageInfo() const
{
    RHIDepthImageDesc desc;
    desc.depth_image = nullptr;  // TODO: Set depth_image when implemented
    desc.depth_image_view = m_DepthImageView;
    desc.depth_image_format = m_DepthImageFormat;
    return desc;
}

uint8_t DX12RHI::GetMaxFramesInFlight() const
{
    return k_max_frames_in_flight;
}

uint8_t DX12RHI::GetCurrentFrameIndex() const
{
    return m_CurrentFrameIndex;
}

void DX12RHI::SetCurrentFrameIndex(uint8_t index)
{
    m_CurrentFrameIndex = index;
}

RHICommandBuffer* DX12RHI::BeginSingleTimeCommands()
{
    WaitForFences();
    ResetCommandPool();
    if (!BeginCommandBufferPFN(m_CurrentCommandBuffer, nullptr))
    {
        return nullptr;
    }
    return m_CurrentCommandBuffer;
}

void DX12RHI::EndSingleTimeCommands(RHICommandBuffer* command_buffer)
{
    if (!EndCommandBufferPFN(command_buffer) || !m_CommandQueue || !m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    ID3D12CommandList* command_lists[] = {m_CommandLists[m_CurrentFrameIndex].Get()};
    m_CommandQueue->ExecuteCommandLists(1, command_lists);

    const uint64_t next_fence_value = m_FenceValues[m_CurrentFrameIndex] + 1;
    if (CheckDX12(m_CommandQueue->Signal(m_Fences[m_CurrentFrameIndex].Get(), next_fence_value),
                  "DX12 single time command signal failed"))
    {
        m_FenceValues[m_CurrentFrameIndex] = next_fence_value;
        WaitForFences();
    }
}

bool DX12RHI::PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain)
{
    // Defensive: drop any floating-surface present requests left over from a frame
    // whose main present early-returned (device loss / transient failure). They are
    // re-queued by BeginFloatingSurfaceDraw if the editor records them again.
    m_PendingFloatingPresents.clear();

    // UE parity: when the window is minimized (or on a virtual desktop that is not
    // visible), GetFramebufferSize() returns (0, 0). DXGI's ResizeBuffers(0, 0, ...)
    // would fail, and even clamped-to-(1,1) creates a broken 1×1 swapchain whose
    // RTV/viewport/scissor state leaks into the next real-sized recreation.
    //
    // The correct behaviour (matching UE's FD3D12Viewport::PresentChecked which
    // returns false without presenting when !bIsValid) is to skip the ENTIRE frame:
    //   - no RecreateSwapchain with dummy dimensions
    //   - no command buffer recording / submission / Present
    //   - no fence or frame-index advancement
    //   - just return "skip" so the editor's main loop calls notifySkippedFrameRender()
    //     and avoids leaving the swapchain un-Presented (which produces gray).
    //
    // Recovery path: NotifyWindowFocusGained() sets m_SwapchainNeedsRecreate=true.
    // On the first visible frame, GetFramebufferSize() returns real dimensions,
    // this zero-size guard passes, and the normal recreate+render path runs.
    if (m_Device && m_Swapchain)
    {
        const std::array<int, 2> fb = GET_SYSTEM(WindowSystem)->GetFramebufferSize();
        if (fb[0] <= 0 || fb[1] <= 0)
        {
            // Window is minimized or otherwise zero-area. Skip rendering entirely.
            return true;
        }
    }

    // If the previous frame's Present detected device loss / occlusion /
    // any failure that left the swapchain in a bad state, force a full
    // recreate NOW so this frame renders into a fresh backbuffer.
    // This is the recovery path for Alt-Tab away + return, driver TDR,
    // or any DXGI surface invalidation (mirrors Vulkan's VK_ERROR_OUT_OF_DATE
    // handling which already calls RecreateSwapchain reactively).
    if (m_SwapchainNeedsRecreate && m_Device && m_Swapchain)
    {
        m_SwapchainNeedsRecreate = false;
        LOG_INFO(ZRender, "DX12: recovering swapchain after Present failure");
        RecreateSwapchain();
        if (passUpdateAfterRecreateSwapchain)
        {
            passUpdateAfterRecreateSwapchain();
        }

        // UE parity: FSlateRHIRenderer::Invalidated() +
        // FSlateRHIResourceManager::ReleaseResources().
        // DXGI surface invalidation (Alt-Tab / minimize / driver TDR)
        // can stale ANY UI GPU resource (font atlases, white texture,
        // Texture2D cache entries, dynamic textures, external image views).
        // InvalidateAllGpuResources now re-uploads font atlases and the
        // white texture IN PLACE (keeping GpuTexture* handle_ids stable)
        // so batch commands recorded on the game thread still resolve
        // to valid descriptor sets this same frame.
        if (auto* gpu_res = UIGpuResources::Get())
        {
            gpu_res->InvalidateAllGpuResources();
        }

        // Do NOT early-return: render into the fresh backbuffer this same frame
        // (same rationale as the resize path below).
    }

    // Proactive swapchain resize. Unlike Vulkan (which reports a stale surface via
    // VK_ERROR_OUT_OF_DATE_KHR and recreates reactively), DXGI silently stretches a
    // mismatched backbuffer, so nothing would ever trigger RecreateSwapchain on a
    // window resize. We therefore compare the live framebuffer size against the
    // current swapchain extent every frame and recreate on a delta. This mirrors
    // UE's FSlateRHIRenderer::ResizeViewportIfNeeded (compare desired vs current,
    // resize only when different) and keeps DX12 in parity with our Vulkan path.
    if (m_Device && m_Swapchain)
    {
        const std::array<int, 2> framebuffer_size = GET_SYSTEM(WindowSystem)->GetFramebufferSize();
        if (framebuffer_size[0] > 0 && framebuffer_size[1] > 0 &&
            (static_cast<uint32_t>(framebuffer_size[0]) != m_SwapchainExtent.width ||
             static_cast<uint32_t>(framebuffer_size[1]) != m_SwapchainExtent.height))
        {
            RecreateSwapchain();
            if (passUpdateAfterRecreateSwapchain)
            {
                passUpdateAfterRecreateSwapchain();
            }

            // UE parity: any swapchain recreate (including resize) can stale
            // UI GPU resources. InvalidateAllGpuResources re-uploads font
            // atlases and the white texture IN PLACE (handle_ids stable) so
            // batch commands recorded on the game thread still resolve to
            // valid descriptor sets this same frame.
            if (auto* gpu_res = UIGpuResources::Get())
            {
                gpu_res->InvalidateAllGpuResources();
            }

            // NOTE: do NOT early-return / skip the frame here. Unlike Vulkan, the DX12 editor
            // path does not have the ImGui game/render handshake wired through
            // notifySkippedFrameRender(), so skipping leaves the swapchain un-presented every
            // frame and the whole window goes blank (light gray). passUpdateAfterRecreateSwapchain
            // above already rebuilt the depth target + RP1 deferred-lighting descriptors, so it
            // is safe to keep rendering into the freshly recreated backbuffer this same frame.
        }
    }

    if (!m_Swapchain || !m_CommandLists[m_CurrentFrameIndex] || !m_RenderTargets[m_CurrentBackBufferIndex])
    {
        return true;
    }

    ResetCommandPool();
    if (!BeginCommandBufferPFN(m_CurrentCommandBuffer, nullptr))
    {
        return true;
    }

    if (m_SwapchainSurfaceState == SwapchainSurfaceState::Present)
    {
        TransitionSwapchainBuffer(m_CurrentBackBufferIndex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle {};
    if (!TryGetSwapchainBackBufferRtv(m_CurrentBackBufferIndex, rtv_handle))
    {
        return true;
    }

    const float clear_color[] = {0.04f, 0.05f, 0.07f, 1.0f};
    m_CommandLists[m_CurrentFrameIndex]->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    m_CommandLists[m_CurrentFrameIndex]->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);

    // ZEngine Insights GPU timing: arm this frame slot (drains the previous use's
    // results) and open the whole-frame "GPU Frame" span. Lazily creates the
    // query heap on first use; all work is internally gated on capture being
    // active, so a closed Insights window adds nothing to the command list.
    m_GpuProfiler.EnsureInitialized(m_Device.Get(), m_CommandQueue.Get(), k_max_frames_in_flight);
    if (m_GpuProfiler.IsReady())
    {
        ID3D12GraphicsCommandList* cmd = m_CommandLists[m_CurrentFrameIndex].Get();
        m_GpuProfiler.BeginFrame(cmd, m_CurrentFrameIndex);
        m_GpuProfiler.BeginScope("GPU Frame");
        m_GpuFrameOpen = true;
    }

    return false;
}

void DX12RHI::SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain)
{
    if (!m_Swapchain || !m_CommandQueue || !m_CommandLists[m_CurrentFrameIndex])
    {
        return;
    }

    if (m_SwapchainResourceStates[m_CurrentBackBufferIndex] == D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        TransitionSwapchainBuffer(m_CurrentBackBufferIndex, D3D12_RESOURCE_STATE_PRESENT);
    }

    // ZEngine Insights GPU timing: close the whole-frame span and resolve all
    // timestamp queries into this slot's readback region (must precede Close()).
    const uint8_t gpu_profile_slot = m_CurrentFrameIndex;
    if (m_GpuFrameOpen)
    {
        m_GpuProfiler.EndScope();
        m_GpuFrameOpen = false;
    }
    if (m_GpuProfiler.IsReady())
    {
        m_GpuProfiler.EndFrame(m_CommandLists[m_CurrentFrameIndex].Get());
    }

    if (!EndCommandBufferPFN(m_CurrentCommandBuffer))
    {
        return;
    }

    ID3D12CommandList* command_lists[] = {m_CommandLists[m_CurrentFrameIndex].Get()};
    m_CommandQueue->ExecuteCommandLists(1, command_lists);

    HRESULT present_result = m_Swapchain->Present(1, 0);

    // UE parity (FD3D12Viewport::PresentChecked):
    // DXGI_STATUS_OCCLUDED is a SUCCESS code (SUCCEEDED returns true).
    // Unlike our old early-return path (which left fences / frame indices stale
    // and caused gray-screen recovery after Alt-Tab or minimize/maximize),
    // we now treat it like UE does: advance ALL frame state normally so the
    // next PrepareBeforePass has a consistent starting point. The only thing we
    // skip is floating-surface Presents (they would also be occluded).
    //
    // Historical note: the old early-return was the root cause of the gray-
    // screen bug. After OCCLUDED, m_FenceValues[m_CurrentFrameIndex] was never
    // advanced, so WaitForFences() on a future frame could block forever;
    // m_CurrentBackBufferIndex / m_CurrentFrameIndex were stale, causing
    // ResetCommandPool() to reset an allocator still in-use by the GPU; and
    // m_SwapchainSurfaceState drifted out of sync with the actual resource
    // state, causing missed or redundant barriers.
    if (present_result == DXGI_STATUS_OCCLUDED)
    {
        m_PendingFloatingPresents.clear();
        // Flag for full swapchain recreation on the next non-zero-sized frame.
        // This is the primary recovery path for minimize/restore because the
        // GLFW window-focus callback (which also sets this flag via
        // NotifyWindowFocusGained) may not fire reliably for taskbar-based
        // minimize/restore on Windows, and even when it does fire,
        // PrepareBeforePass runs BEFORE glfwPollEvents in TickOneFrame so
        // the flag would take one extra frame to take effect.
        //
        // For the common minimize case the overhead is zero: all subsequent
        // frames are skipped (0x0 framebuffer) until the window is restored,
        // so RecreateSwapchain fires exactly once on the first visible frame.
        m_SwapchainNeedsRecreate = true;
        // Fall through to normal fence advance + frame index rotation below,
        // exactly as if Present had succeeded. This matches UE's behaviour in
        // FD3D12Viewport::PresentChecked which checks SUCCEEDED(Result) and
        // continues unconditionally for OCCLUDED.
    }

    if (IsActualDx12DeviceRemoval(present_result))
    {
        static bool s_logged_present_device_loss = false;
        if (!s_logged_present_device_loss)
        {
            s_logged_present_device_loss = true;
            const HRESULT device_reason =
                m_Device != nullptr ? m_Device->GetDeviceRemovedReason() : present_result;
            LOG_ERROR(ZRender,
                      "DX12 device removed/reset during Present: HRESULT=0x{:08X} (GetDeviceRemovedReason=0x{:08X})",
                      static_cast<unsigned int>(present_result),
                      static_cast<unsigned int>(device_reason));
        }
        // Flag so PrepareBeforePass forces a full swapchain recreation on the
        // next frame. This recovers from Alt-Tab TDR / driver reset without
        // requiring a full device teardown (which UE's FD3D12Device does, but
        // for an editor-only path a simple ResizeBuffers round-trip suffices).
        m_SwapchainNeedsRecreate = true;
        return;
    }
    if (present_result == DXGI_ERROR_INVALID_CALL)
    {
        static bool s_logged_present_invalid_call = false;
        if (!s_logged_present_invalid_call)
        {
            s_logged_present_invalid_call = true;
            LOG_ERROR(ZRender, "DX12 Present invalid call: HRESULT=0x887A0001");
        }
        m_SwapchainNeedsRecreate = true;
        return;
    }
    if (FAILED(present_result))
    {
        static bool s_logged_present_other_failure = false;
        if (!s_logged_present_other_failure)
        {
            s_logged_present_other_failure = true;
            LOG_ERROR(ZRender,
                      "DX12 Present failed: HRESULT=0x{:08X}",
                      static_cast<unsigned int>(present_result));
        }
        m_SwapchainNeedsRecreate = true;
        return;
    }

    // Present every floating editor-panel surface that recorded a draw into this
    // frame's command list (the commands were just submitted above). Each reuses
    // this same queue, so a plain Present after the main one is correct.
    PresentPendingFloatingSurfaces();

    const uint64_t next_fence_value = m_FenceValues[m_CurrentFrameIndex] + 1;
    if (CheckDX12(m_CommandQueue->Signal(m_Fences[m_CurrentFrameIndex].Get(), next_fence_value),
                  "DX12 queue signal failed"))
    {
        m_FenceValues[m_CurrentFrameIndex] = next_fence_value;
        // Record which fence value gates this slot's GPU-timestamp readback.
        m_GpuProfiler.MarkSubmitted(gpu_profile_slot, m_Fences[gpu_profile_slot].Get(), next_fence_value);
    }

    m_CurrentBackBufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
    m_CurrentFrameIndex = static_cast<uint8_t>(m_CurrentBackBufferIndex % k_max_frames_in_flight);
    m_CurrentCommandBuffer = m_CommandBuffers[m_CurrentFrameIndex];
}

void DX12RHI::BeginGpuTimingScope(const char* name)
{
    // Nested GPU span on the current frame command list (inside "GPU Frame").
    // No-op-cheap unless capture is active (gated inside the profiler).
    m_GpuProfiler.BeginScope(name);
}

void DX12RHI::EndGpuTimingScope()
{
    m_GpuProfiler.EndScope();
}

void DX12RHI::PushEvent(RHICommandBuffer* commond_buffer, const char* name, const float* color)
{
    // TODO: Implement DX12 pushEvent
}

void DX12RHI::PopEvent(RHICommandBuffer* commond_buffer)
{
    // TODO: Implement DX12 popEvent
}

void DX12RHI::clear()
{
    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (m_Fences[i] && m_FenceEvent != nullptr && m_FenceValues[i] > 0 &&
            m_Fences[i]->GetCompletedValue() < m_FenceValues[i])
        {
            if (CheckDX12(m_Fences[i]->SetEventOnCompletion(m_FenceValues[i], m_FenceEvent),
                          "DX12 SetEventOnCompletion during clear failed"))
            {
                WaitForSingleObject(m_FenceEvent, INFINITE);
            }
        }
    }

    // PR4: tear down the bindless heap BEFORE m_Device is released.
    // The manager owns an ID3D12DescriptorHeap whose lifetime is
    // bound to the device.
    if (m_BindlessTextureManager)
    {
        m_BindlessTextureManager->Shutdown();
        m_BindlessTextureManager.reset();
    }
    m_CubemapMipGenerator.Shutdown();
    m_BindlessSupported = false;
    m_MaxBindlessSampledImages = 0;
    m_MaxBindlessStorageBuffers = 0;

    ClearSwapchain();
    m_DepthStencil.Reset();
    m_DsvHeap.Reset();
    m_CbvSrvUavHeap.Reset();
    m_SamplerHeap.Reset();
    m_DispatchIndirectSignature.Reset();
    m_CurrentGraphicsPipeline = nullptr;
    m_CurrentComputePipeline = nullptr;
    for (UINT& next_descriptor : m_NextCbvSrvUavDescriptorPerFrame)
    {
        next_descriptor = 0;
    }
    m_NextSamplerDescriptor = 0;
    m_LinearSampler = nullptr;
    m_NearestSampler = nullptr;
    m_MipmapSamplerMap.clear();

    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        m_CommandLists[i].Reset();
        m_CommandAllocators[i].Reset();
        m_Fences[i].Reset();
        RHI_DELETE_PTR(m_CommandBuffers[i]);
        RHI_DELETE_PTR(m_RhiIsFrameInFlightFences[i]);
    }

    if (m_FenceEvent != nullptr)
    {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

    if (m_ComputeQueue == m_GraphicsQueue)
    {
        m_ComputeQueue = nullptr;
    }
    RHI_DELETE_PTR(m_GraphicsQueue);
    RHI_DELETE_PTR(m_ComputeQueue);

    m_CommandQueue.Reset();
    m_Device.Reset();
    m_Adapter.Reset();
    m_DxgiFactory.Reset();
}

void DX12RHI::ClearSwapchain()
{
    for (auto& render_target : m_RenderTargets)
    {
        render_target.Reset();
    }

    for (RHIImageView*& image_view : m_SwapchainImageviews)
    {
        RHI_DELETE_PTR(image_view);
    }
    m_SwapchainImageviews.clear();

    m_RtvHeap.Reset();
    m_NextRtvDescriptor = 0;
    m_Swapchain.Reset();
}

void DX12RHI::DestroyDefaultSampler(RHIDefaultSamplerType type)
{
    switch (type)
    {
        case Default_Sampler_Linear:
            DestroySampler(m_LinearSampler);
            m_LinearSampler = nullptr;
            break;
        case Default_Sampler_Nearest:
            DestroySampler(m_NearestSampler);
            m_NearestSampler = nullptr;
            break;
        default:
            break;
    }
}

void DX12RHI::DestroyMipmappedSampler()
{
    for (auto& entry : m_MipmapSamplerMap)
    {
        DestroySampler(entry.second);
    }
    m_MipmapSamplerMap.clear();
}

void DX12RHI::DestroyShaderModule(RHIShader* shader)
{
    // TODO: Implement DX12 destroyShaderModule
}

void DX12RHI::DestroySemaphore(RHISemaphore* semaphore)
{
    // TODO: Implement DX12 destroySemaphore
}

void DX12RHI::DestroySampler(RHISampler* sampler)
{
    RHI_DELETE_PTR(sampler);
}

void DX12RHI::DestroyInstance(RHIInstance* instance)
{
    RHI_DELETE_PTR(instance);
}

void DX12RHI::DestroyImageView(RHIImageView* imageView)
{
    RHI_DELETE_PTR(imageView);
}

void DX12RHI::DestroyImage(RHIImage* image)
{
    RHI_DELETE_PTR(image);
}

void DX12RHI::DestroyFramebuffer(RHIFramebuffer* framebuffer)
{
    RHI_DELETE_PTR(framebuffer);
}

void DX12RHI::DestroyFence(RHIFence* fence)
{
    RHI_DELETE_PTR(fence);
}

void DX12RHI::DestroyDevice()
{
    clear();
}

void DX12RHI::DestroyCommandPool(RHICommandPool* commandPool)
{
    RHI_DELETE_PTR(commandPool);
}

void DX12RHI::DestroyBuffer(RHIBuffer*& buffer)
{
    RHI_DELETE_PTR(buffer);
}

void DX12RHI::FreeCommandBuffers(RHICommandPool* commandPool,
                                 uint32_t commandBufferCount,
                                 RHICommandBuffer* pCommandBuffers)
{
    RHI_DELETE_PTR(pCommandBuffers);
}

void DX12RHI::FreeMemory(RHIDeviceMemory*& memory)
{
    if (memory)
    {
        DX12DeviceMemory* dx12_memory = static_cast<DX12DeviceMemory*>(memory);
        if (dx12_memory->getMappedData() != nullptr && dx12_memory->getResource() != nullptr)
        {
            dx12_memory->getResource()->Unmap(0, nullptr);
            dx12_memory->setMappedData(nullptr);
        }
    }
    RHI_DELETE_PTR(memory);
}

bool DX12RHI::MapMemory(RHIDeviceMemory* memory,
                        RHIDeviceSize offset,
                        RHIDeviceSize size,
                        RHIMemoryMapFlags flags,
                        void** ppData)
{
    if (ppData == nullptr || memory == nullptr)
    {
        return false;
    }

    DX12DeviceMemory* dx12_memory = static_cast<DX12DeviceMemory*>(memory);
    ID3D12Resource* resource = dx12_memory->getResource();
    if (resource == nullptr)
    {
        return false;
    }

    D3D12_RANGE read_range {0, 0};
    if (size != RHI_WHOLE_SIZE)
    {
        read_range = {static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + size)};
    }

    void* mapped_data = nullptr;
    if (!CheckDX12(resource->Map(0, &read_range, &mapped_data), "DX12 map memory failed"))
    {
        return false;
    }

    dx12_memory->setMappedData(mapped_data);
    *ppData = static_cast<uint8_t*>(mapped_data) + offset;
    return true;
}

void DX12RHI::UnmapMemory(RHIDeviceMemory* memory)
{
    if (memory == nullptr)
    {
        return;
    }

    DX12DeviceMemory* dx12_memory = static_cast<DX12DeviceMemory*>(memory);
    if (dx12_memory->getMappedData() != nullptr && dx12_memory->getResource() != nullptr)
    {
        dx12_memory->getResource()->Unmap(0, nullptr);
        dx12_memory->setMappedData(nullptr);
    }
}

void DX12RHI::InvalidateMappedMemoryRanges(void* pNext,
                                           RHIDeviceMemory* memory,
                                           RHIDeviceSize offset,
                                           RHIDeviceSize size)
{
    // TODO: Implement DX12 invalidateMappedMemoryRanges
}

void DX12RHI::FlushMappedMemoryRanges(void* pNext, RHIDeviceMemory* memory, RHIDeviceSize offset, RHIDeviceSize size)
{
    // TODO: Implement DX12 flushMappedMemoryRanges
}

RHISemaphore*& DX12RHI::GetTextureCopySemaphore(uint32_t index)
{
    return m_ImageAvailableForTexturescopySemaphores[index];
}

void DX12RHI::RegisterViewport(const int viewport_id, const RHIViewport& viewport)
{
    m_Viewports[viewport_id] = viewport;
    // Also create corresponding scissor
    RHIRect2D scissor;
    scissor.offset = {static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y)};
    scissor.extent = {static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height)};
    m_Scissors[viewport_id] = scissor;
}

void DX12RHI::UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport)
{
    m_Viewports[(int)viewport_id] = viewport;
    RHIRect2D scissor;
    scissor.offset = {static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y)};
    scissor.extent = {static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height)};
    m_Scissors[(int)viewport_id] = scissor;
}

RHIViewport* DX12RHI::GetViewport(ViewportType viewport_id)
{
    return &m_Viewports[(int)viewport_id];
}

void DX12RHI::CreateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height)
{
    // TODO: Implement DX12 createViewportRenderTexture
    // Destroy existing if any
    DestroyViewportRenderTexture(viewport_id);

    ViewportRenderTexture& rt = m_ViewportRenderTextures[viewport_id];
    rt.width = width;
    rt.height = height;

    // TODO: Create color and depth images for DX12
}

void DX12RHI::UpdateViewportRenderTexture(const std::string& viewport_id, uint32_t width, uint32_t height)
{
    // TODO: Implement DX12 updateViewportRenderTexture
    auto it = m_ViewportRenderTextures.find(viewport_id);
    if (it != m_ViewportRenderTextures.end())
    {
        // If size changed, recreate
        if (it->second.width != width || it->second.height != height)
        {
            DestroyViewportRenderTexture(viewport_id);
            CreateViewportRenderTexture(viewport_id, width, height);
        }
    }
    else
    {
        // Create new if doesn't exist
        CreateViewportRenderTexture(viewport_id, width, height);
    }
}

void DX12RHI::DestroyViewportRenderTexture(const std::string& viewport_id)
{
    // TODO: Implement DX12 destroyViewportRenderTexture
    auto it = m_ViewportRenderTextures.find(viewport_id);
    if (it != m_ViewportRenderTextures.end())
    {
        ViewportRenderTexture& rt = it->second;

        if (rt.framebuffer)
        {
            DestroyFramebuffer(rt.framebuffer);
            rt.framebuffer = nullptr;
        }

        if (rt.color_image_view)
        {
            DestroyImageView(rt.color_image_view);
            rt.color_image_view = nullptr;
        }

        if (rt.color_image)
        {
            DestroyImage(rt.color_image);
            rt.color_image = nullptr;
        }

        if (rt.color_memory)
        {
            FreeMemory(rt.color_memory);
            rt.color_memory = nullptr;
        }

        if (rt.depth_image_view)
        {
            DestroyImageView(rt.depth_image_view);
            rt.depth_image_view = nullptr;
        }

        if (rt.depth_image)
        {
            DestroyImage(rt.depth_image);
            rt.depth_image = nullptr;
        }

        if (rt.depth_memory)
        {
            FreeMemory(rt.depth_memory);
            rt.depth_memory = nullptr;
        }

        m_ViewportRenderTextures.erase(it);
    }
}

RHI::ViewportRenderTexture* DX12RHI::GetViewportRenderTexture(const std::string& viewport_id)
{
    auto it = m_ViewportRenderTextures.find(viewport_id);
    if (it != m_ViewportRenderTextures.end())
    {
        return &(it->second);
    }
    return nullptr;
}
