#include "Runtime/Function/Render/Passes/DX12RenderGraphAbCapture.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/RHI.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace
{
    struct FormatInfo
    {
        const char* name;
        uint32_t bytes_per_pixel;
    };

    /// The formats the capture understands -- exactly the ones RP1 / RP2 render into on
    /// DX12 (backup_* are RGBA16F, the swapchain is RGBA8 / BGRA8). Anything else is
    /// recorded as an error in the manifest instead of producing a misread file.
    bool LookupFormat(RHIFormat format, FormatInfo& out)
    {
        switch (format)
        {
            case RHI_FORMAT_R16G16B16A16_SFLOAT:
                out = {"R16G16B16A16_SFLOAT", 8};
                return true;
            case RHI_FORMAT_R8G8B8A8_UNORM:
                out = {"R8G8B8A8_UNORM", 4};
                return true;
            case RHI_FORMAT_R8G8B8A8_SRGB:
                out = {"R8G8B8A8_SRGB", 4};
                return true;
            case RHI_FORMAT_B8G8R8A8_UNORM:
                out = {"B8G8R8A8_UNORM", 4};
                return true;
            case RHI_FORMAT_B8G8R8A8_SRGB:
                out = {"B8G8R8A8_SRGB", 4};
                return true;
            default:
                return false;
        }
    }
}  // namespace

void DX12RenderGraphAbCapture::Initialize(RHI* rhi)
{
    m_Rhi = rhi;
    m_Enabled = false;

    const char* frame_env = std::getenv("ZENGINE_RG_AB_CAPTURE");
    if (frame_env == nullptr || frame_env[0] == '\0')
    {
        return;
    }
    char* end = nullptr;
    const unsigned long long frame = std::strtoull(frame_env, &end, 10);
    if (end == frame_env || frame == 0)
    {
        LOG_WARNING(ZRender,
                    "RenderGraph A/B capture: ZENGINE_RG_AB_CAPTURE='{}' is not a frame number >= 1; capture disabled",
                    frame_env);
        return;
    }
    if (m_Rhi == nullptr)
    {
        return;
    }

    const char* dir_env = std::getenv("ZENGINE_RG_AB_DIR");
    m_Dir = (dir_env != nullptr && dir_env[0] != '\0') ? dir_env : "rg_ab_capture";
    m_CaptureFrame = frame;
    m_Enabled = true;
    LOG_INFO(ZRender, "RenderGraph A/B capture armed: frame {} -> '{}'", m_CaptureFrame, m_Dir);
}

void DX12RenderGraphAbCapture::Shutdown()
{
    if (!m_Pending.empty() && m_Rhi != nullptr)
    {
        // The copies may still be in flight; never free a buffer the GPU is writing.
        m_Rhi->QueueWaitIdle(m_Rhi->GetGraphicsQueue());
    }
    ReleasePending();
    m_Enabled = false;
}

bool DX12RenderGraphAbCapture::BeginFrame()
{
    if (!m_Enabled || m_Done)
    {
        return false;
    }
    ++m_Frame;

    if (m_Captured)
    {
        Resolve();
        return false;
    }
    if (m_Frame != m_CaptureFrame)
    {
        return false;
    }

    m_Captured = true;
    m_CapturedOnFrame = m_Frame;
    m_CaptureSlot = m_Rhi->GetCurrentFrameIndex();
    LOG_INFO(ZRender, "RenderGraph A/B capture: recording frame {} (frame slot {})", m_Frame, m_CaptureSlot);
    return true;
}

void DX12RenderGraphAbCapture::CopyImage(const char* name,
                                         RHIImage* image,
                                         RHIFormat format,
                                         uint32_t width,
                                         uint32_t height,
                                         RHIImageLayout restore_layout)
{
    RenderGraphAbCapture::ManifestImage info;
    info.name = name;
    info.file = std::string(name) + ".zcap";
    info.width = width;
    info.height = height;

    FormatInfo format_info {};
    const bool known_format = LookupFormat(format, format_info);
    info.format = known_format ? format_info.name : ("RHIFormat#" + std::to_string(static_cast<int>(format)));

    if (!m_Captured || m_Done || m_Rhi == nullptr)
    {
        return;
    }
    if (image == nullptr || width == 0 || height == 0)
    {
        info.error = "no image / zero extent";
        m_Images.push_back(info);
        return;
    }
    if (!known_format)
    {
        info.error = "format not supported by the capture";
        m_Images.push_back(info);
        return;
    }

    Pending pending;
    pending.info = info;
    pending.bytes_per_pixel = format_info.bytes_per_pixel;
    pending.row_pitch = RenderGraphAbCapture::Dx12ReadbackRowPitch(width, format_info.bytes_per_pixel);
    const RHIDeviceSize size = static_cast<RHIDeviceSize>(pending.row_pitch * height);

    // HOST_VISIBLE + TRANSFER_DST is what DX12RHI maps to a READBACK heap.
    m_Rhi->CreateBuffer(size,
                        RHI_BUFFER_USAGE_TRANSFER_DST_BIT,
                        RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT | RHI_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        pending.buffer,
                        pending.memory);
    if (pending.buffer == nullptr || pending.memory == nullptr)
    {
        info.error = "readback buffer allocation failed";
        m_Images.push_back(info);
        return;
    }

    RHICommandBuffer* cmd = m_Rhi->GetCurrentCommandBuffer();

    RHIBufferImageCopy region {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;  // tightly packed rows; DX12 aligns the pitch to 256 itself
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    m_Rhi->CmdCopyImageToBuffer(cmd, image, RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pending.buffer, 1, &region);

    // Hand the image back in the state the next consumer expects. (DX12 transitions from
    // its own tracked state, so oldLayout is informational.)
    RHIImageMemoryBarrier restore {};
    restore.sType = RHI_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restore.srcAccessMask = RHI_ACCESS_TRANSFER_READ_BIT;
    restore.dstAccessMask = 0;
    restore.oldLayout = RHI_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore.newLayout = restore_layout;
    restore.srcQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
    restore.dstQueueFamilyIndex = RHI_QUEUE_FAMILY_IGNORED;
    restore.image = image;
    restore.subresourceRange.aspectMask = RHI_IMAGE_ASPECT_COLOR_BIT;
    restore.subresourceRange.baseMipLevel = 0;
    restore.subresourceRange.levelCount = 1;
    restore.subresourceRange.baseArrayLayer = 0;
    restore.subresourceRange.layerCount = 1;
    m_Rhi->CmdPipelineBarrier(cmd,
                              RHI_PIPELINE_STAGE_TRANSFER_BIT,
                              RHI_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &restore);

    m_Pending.push_back(pending);
}

void DX12RenderGraphAbCapture::RecordStage(const char* stage, bool attempted, bool graph_used)
{
    if (!m_Captured || m_Done)
    {
        return;
    }
    RenderGraphAbCapture::ManifestStage entry;
    entry.stage = stage;
    entry.attempted = attempted;
    entry.graph_used = graph_used;
    m_Stages.push_back(entry);
}

void DX12RenderGraphAbCapture::Resolve()
{
    // The capture frame's copies are complete once its frame slot comes round again:
    // the frame loop waits on the current slot's fence before recording into it.
    if (m_Frame <= m_CapturedOnFrame || m_Rhi->GetCurrentFrameIndex() != m_CaptureSlot)
    {
        // Safety valve: if the slot never recurs (frame index reset by a swapchain
        // recreate), fall back to an explicit wait a few frames later.
        if (m_Frame < m_CapturedOnFrame + 8u)
        {
            return;
        }
        m_Rhi->QueueWaitIdle(m_Rhi->GetGraphicsQueue());
        AddNote("readback resolved via the late-frame safety wait (frame slot did not recur)");
    }

    std::error_code ec;
    std::filesystem::create_directories(m_Dir, ec);
    if (ec)
    {
        LOG_ERROR(ZRender, "RenderGraph A/B capture: cannot create '{}': {}", m_Dir, ec.message());
    }

    for (Pending& pending : m_Pending)
    {
        RenderGraphAbCapture::ManifestImage info = pending.info;
        void* mapped = nullptr;
        const RHIDeviceSize size = static_cast<RHIDeviceSize>(pending.row_pitch * info.height);
        if (!m_Rhi->MapMemory(pending.memory, 0, size, 0, &mapped) || mapped == nullptr)
        {
            info.error = "map failed";
        }
        else
        {
            const std::vector<uint8_t> bytes = RenderGraphAbCapture::EncodeZcap(info.width,
                                                                                info.height,
                                                                                pending.bytes_per_pixel,
                                                                                info.format.c_str(),
                                                                                static_cast<const uint8_t*>(mapped),
                                                                                pending.row_pitch);
            m_Rhi->UnmapMemory(pending.memory);
            std::string error;
            const std::string path = (std::filesystem::path(m_Dir) / info.file).string();
            info.written = RenderGraphAbCapture::WriteFileBytes(path, bytes, &error);
            info.error = error;
        }
        m_Images.push_back(info);
    }
    ReleasePending();
    WriteManifest();
    m_Done = true;
}

void DX12RenderGraphAbCapture::ReleasePending()
{
    if (m_Rhi == nullptr)
    {
        m_Pending.clear();
        return;
    }
    for (Pending& pending : m_Pending)
    {
        if (pending.buffer != nullptr)
        {
            m_Rhi->DestroyBuffer(pending.buffer);
        }
        if (pending.memory != nullptr)
        {
            m_Rhi->FreeMemory(pending.memory);
        }
    }
    m_Pending.clear();
}

void DX12RenderGraphAbCapture::WriteManifest()
{
    const std::string json = RenderGraphAbCapture::EncodeManifest(m_CapturedOnFrame, m_Stages, m_Images, m_Notes);
    const std::vector<uint8_t> bytes(json.begin(), json.end());
    std::string error;
    const std::string path = (std::filesystem::path(m_Dir) / "manifest.json").string();
    if (RenderGraphAbCapture::WriteFileBytes(path, bytes, &error))
    {
        size_t written = 0;
        for (const RenderGraphAbCapture::ManifestImage& image : m_Images)
        {
            written += image.written ? 1 : 0;
        }
        LOG_INFO(ZRender,
                 "RenderGraph A/B capture: wrote {}/{} images + manifest to '{}' "
                 "(compare with: python scripts/rg_ab_compare.py \"{}\")",
                 written, m_Images.size(), m_Dir, m_Dir);
    }
    else
    {
        LOG_ERROR(ZRender, "RenderGraph A/B capture: {}", error);
    }
}
