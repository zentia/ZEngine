#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
#include "Runtime/Function/Render/Interface/Metal/MetalBindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/Metal/MetalRHIResource.h"

#include "Runtime/Function/Render/WindowSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

#ifdef __APPLE__
    // SPIR-V -> MSL (the engine's shaders are SPIR-V; Metal only accepts MSL).
    #include <spirv_cross.hpp>
    #include <spirv_msl.hpp>
#endif

// Metal uses one index space for vertex buffers *and* shader argument buffers,
// so descriptor buffers start after the vertex buffer slots. Both the MSL side
// (CreateShaderModule) and the runtime bind side (CmdBindDescriptorSetsPFN) use
// this offset.
static constexpr uint32_t k_msl_descriptor_buffer_offset = 8;
// Metal has a flat index space per resource kind, while Vulkan/GLSL descriptors
// are (set, binding) pairs, so each descriptor set gets its own slice.
static constexpr uint32_t k_msl_descriptor_set_stride = 8;

#ifdef __APPLE__
    #define Component AppleComponent
    #import <Cocoa/Cocoa.h>
    #include <dispatch/dispatch.h>
    #undef Component
#endif

std::vector<std::type_index> MetalRHI::GetDependencies() const
{
    return {GET_SYSTEM_TYPE(WindowSystem)};
}

bool MetalRHI::Initialize()
{
#ifdef __APPLE__
    m_Device = MTLCreateSystemDefaultDevice();
    if (m_Device == nil)
    {
        return false;
    }
    m_CommandQueue = [m_Device newCommandQueue];
    if (m_CommandQueue == nil)
    {
        return false;
    }

    // GetDescriptorPoor() 是 RHI 的公开契约，调用方（Nanite 的剔除管线、各种 pass）
    // 拿它去 AllocateDescriptorSets。以前这个成员从来没被赋过值，Metal 上恒为
    // nullptr —— 目前 AllocateDescriptorSets 不读它所以看不出问题，但任何一个后端
    // 真的按 pool 记账就会立刻空指针。
    if (m_DescriptorPool == nullptr)
    {
        CreateDescriptorPool(nullptr, m_DescriptorPool);
    }

    // Frame pacing: one binary semaphore per in-flight frame (k_max_frames_in_flight).
    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        m_Semaphores[i] = dispatch_semaphore_create(1);
    }

    // ---- Bindless capability probe ----
    // Tier2 argument buffers are required for large texture arrays.
    MTLArgumentBuffersTier tier = [m_Device argumentBuffersSupport];
    if (tier >= MTLArgumentBuffersTier2)
    {
        // Query device limits for bindless capacity.
        // Metal doesn't expose a "max sampled images" query like Vulkan;
        // we use the same default as DX12/Vulkan (16384 on desktop-class,
        // 4096 on mobile). Apple GPUs support up to 65535 textures in
        // argument buffers on Tier2.
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST
        m_MaxBindlessSampledImages  = 4096;
        m_MaxBindlessStorageBuffers = 256;
#else
        m_MaxBindlessSampledImages  = 16384;
        m_MaxBindlessStorageBuffers = 1024;
#endif

        m_BindlessTextureManager = new MetalBindlessTextureManager();
        if (!m_BindlessTextureManager->Initialize(m_Device, m_MaxBindlessSampledImages))
        {
            LOG_WARNING(ZRender,
                        "MetalBindlessTextureManager init failed -- demoting bindless support to OFF");
            delete m_BindlessTextureManager;
        m_BindlessTextureManager = nullptr;
            m_MaxBindlessSampledImages  = 0;
            m_MaxBindlessStorageBuffers = 0;
        }
        else
        {
            m_BindlessSupported = true;
            LOG_INFO(ZRender,
                     "MetalRHI: bindless textures ENABLED (capacity = {}, Tier2)",
                     m_MaxBindlessSampledImages);
        }
    }
    else
    {
        LOG_INFO(ZRender,
                 "MetalRHI: Argument Buffer Tier1 only -- bindless textures DISABLED");
    }

    return true;
#else
    return false;
#endif
}

void MetalRHI::Shutdown()
{
#ifdef __APPLE__
    // Tear down bindless manager before releasing the device.
    if (m_BindlessTextureManager)
    {
        m_BindlessTextureManager->Shutdown();
        delete m_BindlessTextureManager;
        m_BindlessTextureManager = nullptr;
    }
    m_BindlessSupported           = false;
    m_MaxBindlessSampledImages  = 0;
    m_MaxBindlessStorageBuffers = 0;

#if !__has_feature(objc_arc)
    for (uint8_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (m_Semaphores[i] != nullptr)
        {
            dispatch_release(m_Semaphores[i]);
            m_Semaphores[i] = nullptr;
        }
    }
    [m_CommandQueue release];
    [m_Device release];
#endif
    m_CommandQueue = nil;
    m_Device        = nil;
#endif
}

MetalRHI::~MetalRHI() = default;

void MetalRHI::PrepareContext()
{
#ifdef __APPLE__
    // Lazily attach the CAMetalLayer: WindowSystem may create the Cocoa window
    // after the RHI (system init order is platform phase -> rendering phase, but
    // the editor shows a splash and calls ShowWindow() later).
    if (m_MetalLayer == nil)
    {
        CreateSwapchain();
    }
#endif
}
bool MetalRHI::IsPointLightShadowEnabled() { return false; }
void MetalRHI::NotifyWindowFocusGained() {}
void MetalRHI::DestroyRenderPass(RHIRenderPass* /*renderPass*/) {}

bool MetalRHI::AllocateCommandBuffers(const RHICommandBufferAllocateInfo* pAllocateInfo,
                                      RHICommandBuffer*&                  pCommandBuffers)
{
    pCommandBuffers = nullptr;
    if (pAllocateInfo == nullptr || pAllocateInfo->commandBufferCount == 0)
    {
        return false;
    }
    pCommandBuffers = new MetalCommandBuffer();
    return true;
}

bool MetalRHI::AllocateDescriptorSets(const RHIDescriptorSetAllocateInfo* pAllocateInfo,
                                      RHIDescriptorSet*&                  pDescriptorSets)
{
    pDescriptorSets = nullptr;
    if (pAllocateInfo == nullptr || pAllocateInfo->descriptorSetCount == 0)
    {
        return false;
    }
    auto* set = new MetalDescriptorSet();
    if (pAllocateInfo->pSetLayouts != nullptr && pAllocateInfo->pSetLayouts[0] != nullptr)
    {
        auto* layout = (MetalDescriptorSetLayout*)pAllocateInfo->pSetLayouts[0];
        set->layout   = layout;
        set->bindings = layout->bindings;
    }
    pDescriptorSets = set;
    return true;
}

// ---------------------------------------------------------------------------
// Swapchain (CAMetalLayer) + present
//
// Metal hands out a drawable per frame (nextDrawable) instead of a fixed set of
// swapchain images: CreateSwapchain attaches a CAMetalLayer to the main window's
// content view, PrepareBeforePass acquires the drawable and SubmitRendering
// clears + presents it. Everything else (the actual scene / ZSlate batches) still
// needs the Cmd* encoder implementation, so until that lands the macOS editor
// presents a cleared frame -- see the note in EditorUIPass::Draw().
// ---------------------------------------------------------------------------
namespace
{
    // The Cocoa view that hosts the CAMetalLayer: the main window's content view.
    NSView* GetSwapchainHostView()
    {
        auto* window_system = GET_SYSTEM(WindowSystem);
        if (window_system == nullptr)
        {
            return nil;
        }
        NSWindow* ns_window = (__bridge NSWindow*)window_system->GetNativeWindowHandle();
        if (ns_window == nil)
        {
            return nil;
        }
        return ns_window.contentView;
    }

    // Sync the layer drawable size (and the RHI extent / viewport / scissor) with
    // the view's current backing size. Returns false when the view has no area.
    bool SyncLayerMetrics(CAMetalLayer* layer,
                          NSView*      view,
                          RHIExtent2D& extent,
                          RHIViewport* viewports,
                          RHIRect2D*   scissors)
    {
        if (layer == nil || view == nil)
        {
            return false;
        }
        const NSRect backing = [view convertRectToBacking:view.bounds];
        const double width  = backing.size.width;
        const double height = backing.size.height;
        if (width <= 0.0 || height <= 0.0)
        {
            return false;
        }

        const CGFloat scale = view.window != nil ? view.window.backingScaleFactor : 1.0;
        layer.contentsScale  = scale > 0.0 ? scale : 1.0;
        layer.drawableSize   = CGSizeMake(width, height);

        extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        for (int i = 0; i < 2; ++i)
        {
            viewports[i] = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            scissors[i]  = {{0, 0}, {extent.width, extent.height}};
        }
        return true;
    }
}  // namespace

void MetalRHI::CreateSwapchain()
{
#ifdef __APPLE__
    NSView* view = GetSwapchainHostView();
    if (view == nil)
    {
        LOG_WARNING(ZRender, "MetalRHI::CreateSwapchain -- no Cocoa content view yet (window not created?)");
        return;
    }
    m_SwapchainView = (__bridge void*)view;

    if (m_MetalLayer == nil)
    {
        m_MetalLayer                     = [[CAMetalLayer alloc] init];
        m_MetalLayer.device              = m_Device;
        m_MetalLayer.pixelFormat         = MTLPixelFormatBGRA8Unorm;
        // UI overlay / readback need the drawable to stay readable.
        m_MetalLayer.framebufferOnly     = NO;
        m_MetalLayer.maximumDrawableCount = k_max_frames_in_flight;
        view.wantsLayer = YES;
        view.layer      = m_MetalLayer;
    }

    m_SwapchainImageFormat = RHI_FORMAT_B8G8R8A8_UNORM;
    if (SyncLayerMetrics(m_MetalLayer, view, m_SwapchainExtent, m_Viewports, m_Scissors))
    {
        LOG_INFO(ZRender,
                 "MetalRHI: swapchain ready {}x{}",
                 m_SwapchainExtent.width,
                 m_SwapchainExtent.height);
    }
#endif
}

void MetalRHI::RecreateSwapchain()
{
#ifdef __APPLE__
    NSView* view = m_SwapchainView != nullptr ? (__bridge NSView*)m_SwapchainView : GetSwapchainHostView();
    if (view == nil || m_MetalLayer == nil)
    {
        CreateSwapchain();
        return;
    }
    SyncLayerMetrics(m_MetalLayer, view, m_SwapchainExtent, m_Viewports, m_Scissors);
#endif
}

void MetalRHI::CreateSwapchainImageViews()
{
    // Metal hands out drawables per frame (nextDrawable); there are no persistent
    // swapchain image views to create -- see PrepareBeforePass().
}

void MetalRHI::CreateFramebufferImageAndView()
{
    // Metal resolves the drawable per frame; see PrepareBeforePass().
}

namespace
{
    // RHIFormat -> MTLPixelFormat. Unknown / block-compressed formats fall back
    // to RGBA8Unorm so pipeline creation still succeeds (the caller logs the
    // mismatch through the texture import path).
    MTLPixelFormat ToMetalFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHI_FORMAT_R8_UNORM:            return MTLPixelFormatR8Unorm;
            case RHI_FORMAT_R8G8_UNORM:          return MTLPixelFormatRG8Unorm;
            case RHI_FORMAT_R8G8B8A8_UNORM:      return MTLPixelFormatRGBA8Unorm;
            case RHI_FORMAT_R8G8B8A8_SRGB:       return MTLPixelFormatRGBA8Unorm_sRGB;
            case RHI_FORMAT_B8G8R8A8_UNORM:      return MTLPixelFormatBGRA8Unorm;
            case RHI_FORMAT_B8G8R8A8_SRGB:       return MTLPixelFormatBGRA8Unorm_sRGB;
            case RHI_FORMAT_R16_SFLOAT:          return MTLPixelFormatR16Float;
            case RHI_FORMAT_R16G16_SFLOAT:       return MTLPixelFormatRG16Float;
            case RHI_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
            case RHI_FORMAT_R32_SFLOAT:          return MTLPixelFormatR32Float;
            case RHI_FORMAT_R32G32_SFLOAT:       return MTLPixelFormatRG32Float;
            case RHI_FORMAT_R32G32B32_SFLOAT:    return MTLPixelFormatRGBA32Float;
            case RHI_FORMAT_R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
            case RHI_FORMAT_R32_UINT:            return MTLPixelFormatR32Uint;
            case RHI_FORMAT_R32_SINT:            return MTLPixelFormatR32Sint;
            case RHI_FORMAT_R8G8B8A8_UINT:       return MTLPixelFormatRGBA8Uint;
            case RHI_FORMAT_D16_UNORM:           return MTLPixelFormatDepth16Unorm;
            case RHI_FORMAT_D32_SFLOAT:          return MTLPixelFormatDepth32Float;
            case RHI_FORMAT_D24_UNORM_S8_UINT:   return MTLPixelFormatDepth24Unorm_Stencil8;
            case RHI_FORMAT_D32_SFLOAT_S8_UINT:  return MTLPixelFormatDepth32Float_Stencil8;
            default:                             return MTLPixelFormatRGBA8Unorm;
        }
    }

    MTLVertexFormat ToMetalVertexFormat(RHIFormat format)
    {
        switch (format)
        {
            case RHI_FORMAT_R32_SFLOAT:            return MTLVertexFormatFloat;
            case RHI_FORMAT_R32G32_SFLOAT:         return MTLVertexFormatFloat2;
            case RHI_FORMAT_R32G32B32_SFLOAT:      return MTLVertexFormatFloat3;
            case RHI_FORMAT_R32G32B32A32_SFLOAT:   return MTLVertexFormatFloat4;
            case RHI_FORMAT_R16G16_SFLOAT:         return MTLVertexFormatHalf2;
            case RHI_FORMAT_R16G16B16A16_SFLOAT:   return MTLVertexFormatHalf4;
            case RHI_FORMAT_R8G8B8A8_UNORM:        return MTLVertexFormatUChar4Normalized;
            case RHI_FORMAT_R8G8B8A8_UINT:         return MTLVertexFormatUChar4;
            case RHI_FORMAT_R32_UINT:              return MTLVertexFormatUInt;
            case RHI_FORMAT_R32_SINT:              return MTLVertexFormatInt;
            default:                               return MTLVertexFormatFloat4;
        }
    }

    MTLBlendFactor ToMetalBlendFactor(RHIBlendFactor factor)
    {
        switch (factor)
        {
            case RHI_BLEND_FACTOR_ZERO:                     return MTLBlendFactorZero;
            case RHI_BLEND_FACTOR_ONE:                      return MTLBlendFactorOne;
            case RHI_BLEND_FACTOR_SRC_COLOR:                return MTLBlendFactorSourceColor;
            case RHI_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:      return MTLBlendFactorOneMinusSourceColor;
            case RHI_BLEND_FACTOR_DST_COLOR:                return MTLBlendFactorDestinationColor;
            case RHI_BLEND_FACTOR_ONE_MINUS_DST_COLOR:      return MTLBlendFactorOneMinusDestinationColor;
            case RHI_BLEND_FACTOR_SRC_ALPHA:                return MTLBlendFactorSourceAlpha;
            case RHI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:      return MTLBlendFactorOneMinusSourceAlpha;
            case RHI_BLEND_FACTOR_DST_ALPHA:                return MTLBlendFactorDestinationAlpha;
            case RHI_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:      return MTLBlendFactorOneMinusDestinationAlpha;
            case RHI_BLEND_FACTOR_SRC_ALPHA_SATURATE:       return MTLBlendFactorSourceAlphaSaturated;
            default:                                        return MTLBlendFactorOne;
        }
    }

    MTLBlendOperation ToMetalBlendOp(RHIBlendOp op)
    {
        switch (op)
        {
            case RHI_BLEND_OP_SUBTRACT:         return MTLBlendOperationSubtract;
            case RHI_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
            case RHI_BLEND_OP_MIN:              return MTLBlendOperationMin;
            case RHI_BLEND_OP_MAX:              return MTLBlendOperationMax;
            default:                            return MTLBlendOperationAdd;
        }
    }

    bool IsDepthFormat(RHIFormat format)
    {
        return format == RHI_FORMAT_D16_UNORM || format == RHI_FORMAT_D32_SFLOAT ||
               format == RHI_FORMAT_D24_UNORM_S8_UINT || format == RHI_FORMAT_D32_SFLOAT_S8_UINT;
    }

    // 只有带 stencil 分量的深度格式才是 "stencil renderable"。D32_SFLOAT / D16_UNORM
    // 没有 stencil —— 把它们的格式也塞进 stencilAttachmentPixelFormat，Metal 会直接
    // 断言 "MTLPixelFormatDepth32Float is not stencil renderable"（建管线时就炸，
    // 不是等到绘制）。以前只对 swapchain 那套带 stencil 的格式跑过，所以没暴露。
    bool HasStencilComponent(RHIFormat format)
    {
        return format == RHI_FORMAT_D24_UNORM_S8_UINT || format == RHI_FORMAT_D32_SFLOAT_S8_UINT ||
               format == RHI_FORMAT_S8_UINT;
    }

    MTLTextureUsage ToMetalUsage(RHIImageUsageFlags usage)
    {
        MTLTextureUsage result = MTLTextureUsageShaderRead;
        if ((usage & RHI_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 ||
            (usage & RHI_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
        {
            result |= MTLTextureUsageRenderTarget;
        }
        if ((usage & RHI_IMAGE_USAGE_STORAGE_BIT) != 0)
        {
            result |= MTLTextureUsageShaderWrite;
        }
        return result;
    }

    MTLSamplerAddressMode ToMetalAddressMode(RHISamplerAddressMode mode)
    {
        switch (mode)
        {
            case RHI_SAMPLER_ADDRESS_MODE_REPEAT:          return MTLSamplerAddressModeRepeat;
            case RHI_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
            case RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:   return MTLSamplerAddressModeClampToEdge;
            case RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
            default:                                       return MTLSamplerAddressModeClampToEdge;
        }
    }
}  // namespace

RHISampler* MetalRHI::GetOrCreateDefaultSampler(RHIDefaultSamplerType type)
{
    auto& cached = m_DefaultSamplers[static_cast<size_t>(type)];
    if (cached != nullptr)
    {
        return cached;
    }

    RHISamplerCreateInfo info {};
    info.sType            = RHI_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter        = RHI_FILTER_LINEAR;
    info.minFilter        = RHI_FILTER_LINEAR;
    info.mipmapMode       = RHI_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU     = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV     = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW     = RHI_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy    = 1.0f;
    info.minLod           = 0.0f;
    info.maxLod           = 16.0f;

    RHISampler* sampler = nullptr;
    if (CreateSampler(&info, sampler))
    {
        cached = sampler;
    }
    return cached;
}

RHISampler* MetalRHI::GetOrCreateMipmapSampler(uint32_t width, uint32_t height)
{
    (void)width;
    (void)height;
    if (m_MipmapSampler != nullptr)
    {
        return m_MipmapSampler;
    }
    m_MipmapSampler = GetOrCreateDefaultSampler(RHIDefaultSamplerType::Default_Sampler_Linear);
    return m_MipmapSampler;
}

#ifdef __APPLE__
namespace
{
    // 关掉还开着的 compute encoder。Metal 不允许一个 command buffer 上同时存在两个
    // encoder，所以任何"要开 render encoder"或"要收尾"的地方都得先关它。
    void EndComputeEncoder(MetalCommandBuffer* metal_buffer)
    {
        if (metal_buffer == nullptr || metal_buffer->computeEncoder == nil)
        {
            return;
        }
        [metal_buffer->computeEncoder endEncoding];
        [metal_buffer->computeEncoder release];
        metal_buffer->computeEncoder = nil;
    }

    // 拿到一个还活着的 compute encoder（没有就开一个）。
    //
    // 以前只有 CmdDispatch 会开它，于是**在 dispatch 之前**发的
    // CmdBindDescriptorSetsPFN(COMPUTE) 全部落空：那时 computeEncoder 还是 nil，
    // 那句 `if (compute && metal_buffer->computeEncoder != nil)` 直接跳过，一个
    // buffer 都没绑上 —— kernel 读到的 [[buffer(n)]] 全是空的，剔除结果恒为 0。
    // 绑定必须在 dispatch 之前，所以"开 encoder"这一步不能只属于 dispatch。
    id<MTLComputeCommandEncoder> BeginComputeEncoder(MetalCommandBuffer* metal_buffer)
    {
        if (metal_buffer == nullptr || metal_buffer->commandBuffer == nil)
        {
            return nil;
        }
        if (metal_buffer->computeEncoder == nil)
        {
            id<MTLComputeCommandEncoder> encoder = [metal_buffer->commandBuffer computeCommandEncoder];
            metal_buffer->computeEncoder = [encoder retain];
            [encoder release];
        }
        return metal_buffer->computeEncoder;
    }

    // SPIR-V: 扫 OpExecutionMode（opcode 16）找 LocalSize（17）或 LocalSizeId（38）。
    //
    // 为什么不问 SPIRV-Cross：它把 workgroup 尺寸藏在 EntryPoint 结构里，而这个仓库
    // vendored 的版本没有稳定的公开访问器；直接扫字节码只依赖 SPIR-V 自己的布局，
    // 二十行且和版本无关。
    void ReadWorkgroupSizeFromSpirv(const std::vector<uint32_t>& words, uint32_t out_size[3])
    {
        out_size[0] = out_size[1] = out_size[2] = 1;

        constexpr uint32_t kOpExecutionMode = 16;
        constexpr uint32_t kExecutionModeLocalSize = 17;
        constexpr uint32_t kExecutionModeLocalSizeId = 38;

        size_t i = 5;  // 跳过 header（magic/version/generator/bound/schema）
        while (i < words.size())
        {
            const uint32_t word = words[i];
            const uint32_t opcode = word & 0xFFFFu;
            const uint32_t count = word >> 16;
            if (count == 0 || i + count > words.size())
            {
                break;
            }
            if (opcode == kOpExecutionMode && count >= 4)
            {
                const uint32_t mode = words[i + 2];
                if ((mode == kExecutionModeLocalSize || mode == kExecutionModeLocalSizeId) && count >= 6)
                {
                    out_size[0] = words[i + 3];
                    out_size[1] = words[i + 4];
                    out_size[2] = words[i + 5];
                    for (uint32_t axis = 0; axis < 3; ++axis)
                    {
                        if (out_size[axis] == 0)
                        {
                            out_size[axis] = 1;
                        }
                    }
                    return;
                }
            }
            i += count;
        }
    }

    // MSL: `[[threads_per_threadgroup(256, 1, 1)]]`。MSL 源串是引擎自己拼的（Nanite
    // 的剔除 kernel 走 CreateShaderModuleFromSource），所以手写解析就够了。
    void ReadWorkgroupSizeFromMsl(const std::string& source, uint32_t out_size[3])
    {
        out_size[0] = out_size[1] = out_size[2] = 1;

        const std::string marker = "threads_per_threadgroup";
        const size_t at = source.find(marker);
        if (at == std::string::npos)
        {
            return;
        }
        const size_t open = source.find('(', at);
        if (open == std::string::npos)
        {
            return;
        }
        const size_t close = source.find(')', open);
        if (close == std::string::npos)
        {
            return;
        }

        uint32_t axis = 0;
        size_t cursor = open + 1;
        while (axis < 3 && cursor < close)
        {
            const size_t comma = source.find(',', cursor);
            const size_t end = (comma == std::string::npos || comma > close) ? close : comma;
            const std::string token = source.substr(cursor, end - cursor);
            try
            {
                out_size[axis] = static_cast<uint32_t>(std::max(std::stoul(token), 1ul));
            }
            catch (const std::exception&)
            {
                out_size[axis] = 1;
            }
            ++axis;
            if (comma == std::string::npos || comma > close)
            {
                break;
            }
            cursor = comma + 1;
        }
    }

    // Compiles MSL source into a Metal shader module. `entry_point` is the MSL
    // function name -- SPIRV-Cross may rename the SPIR-V entry point (e.g.
    // "main" -> "main0"), so callers pass the name back explicitly.
    RHIShader* CreateMetalShaderFromMSL(id<MTLDevice>      device,
                                        const std::string& source,
                                        ShaderStage        stage,
                                        const std::string& shader_name,
                                        const std::string& entry_point)
    {
        if (device == nil || source.empty())
        {
            return nullptr;
        }

        NSError*         error     = nil;
        NSString*        ns_source = [NSString stringWithUTF8String:source.c_str()];
        MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
        id<MTLLibrary> library = [device newLibraryWithSource:ns_source options:options error:&error];
        [options release];
        if (library == nil)
        {
            LOG_WARNING(ZRender,
                        "MetalRHI: MSL compile failed for '{}': {}",
                        shader_name,
                        error != nil ? error.localizedDescription.UTF8String : "unknown error");
            return nullptr;
        }

        NSString*      ns_entry = [NSString stringWithUTF8String:entry_point.c_str()];
        id<MTLFunction> function = [library newFunctionWithName:ns_entry];
        if (function == nil)
        {
            // Fall back to the first exported function when the entry point is missing.
            NSArray<NSString*>* names = library.functionNames;
            if (names.count > 0)
            {
                function = [library newFunctionWithName:names.firstObject];
            }
        }
        if (function == nil)
        {
            LOG_WARNING(ZRender, "MetalRHI: entry point '{}' not found in '{}'", entry_point, shader_name);
            [library release];
            return nullptr;
        }

        auto* shader = new MetalShader();
        shader->setLibrary(library);
        shader->setFunction(function);
        shader->stage      = static_cast<uint32_t>(stage);
        shader->entryPoint = entry_point;
        if (stage == ShaderStage::Compute)
        {
            ReadWorkgroupSizeFromMsl(source, shader->workgroupSize);
        }
        [library release];
        [function release];
        return shader;
    }


    ShaderStage ToShaderStage(uint32_t execution_model)
    {
        switch (execution_model)
        {
            case spv::ExecutionModelVertex:            return ShaderStage::Vertex;
            case spv::ExecutionModelFragment:          return ShaderStage::Fragment;
            case spv::ExecutionModelGeometry:          return ShaderStage::Geometry;
            case spv::ExecutionModelTessellationControl:   return ShaderStage::TessellationControl;
            case spv::ExecutionModelTessellationEvaluation: return ShaderStage::TessellationEvaluation;
            case spv::ExecutionModelGLCompute:         return ShaderStage::Compute;
            default:                                   return ShaderStage::Vertex;
        }
    }
}  // namespace
#endif

// The engine's shaders are GLSL compiled to SPIR-V (see engine/shader); Metal only
// accepts MSL, so the SPIR-V goes through SPIRV-Cross' MSL backend here.
RHIShader* MetalRHI::CreateShaderModule(const std::vector<unsigned char>& shader_code)
{
#ifdef __APPLE__
    const bool is_spirv = shader_code.size() >= 4 && (shader_code.size() % 4) == 0 &&
                          shader_code[0] == 0x03 && shader_code[1] == 0x02 &&
                          shader_code[2] == 0x23 && shader_code[3] == 0x07;
    if (!is_spirv)
    {
        LOG_WARNING(ZRender, "MetalRHI: shader module is not SPIR-V ({} bytes) -- ignored", shader_code.size());
        return nullptr;
    }
    if (m_Device == nil)
    {
        return nullptr;
    }

    std::vector<uint32_t> words(shader_code.size() / 4);
    std::memcpy(words.data(), shader_code.data(), shader_code.size());

    // **必须在 words 被 move 进 SPIRV-Cross 之前读 workgroup 尺寸。**
    // 以前这一段放在 try 的最后（`spirv_cross::CompilerMSL compiler(std::move(words))`
    // 之后），而 move 过的 vector 通常是空的 —— 扫描器什么都扫不到，于是恒返回
    // (1,1,1)：`local_size_x = 64` 的 kernel 在 Metal 上只有一个线程干活，和 Vulkan
    // 同一份 shader 的行为完全不同。而且它不报错、不警告，只是"结果少得离谱"。
    uint32_t spirv_workgroup_size[3] = {1, 1, 1};
    ReadWorkgroupSizeFromSpirv(words, spirv_workgroup_size);

    try
    {
        spirv_cross::CompilerMSL compiler(std::move(words));

        auto entry_points = compiler.get_entry_points_and_stages();
        if (entry_points.empty())
        {
            LOG_WARNING(ZRender, "MetalRHI: SPIR-V module has no entry point");
            return nullptr;
        }
        const std::string entry = entry_points[0].name;
        const auto        model = entry_points[0].execution_model;

        auto add_msl_binding = [&](const spirv_cross::Resource& resource, bool is_buffer) {
            spirv_cross::MSLResourceBinding binding {};
            binding.stage = model;
            binding.basetype = compiler.get_type(resource.base_type_id).basetype;
            binding.desc_set = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
                                   ? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
                                   : 0u;
            binding.binding = compiler.has_decoration(resource.id, spv::DecorationBinding)
                                  ? compiler.get_decoration(resource.id, spv::DecorationBinding)
                                  : 0u;
            const uint32_t slot = binding.desc_set * k_msl_descriptor_set_stride + binding.binding;
            if (is_buffer)
            {
                // Buffers start past the vertex buffer slots.
                binding.msl_buffer = slot + k_msl_descriptor_buffer_offset;
            }
            else
            {
                binding.msl_texture = slot;
                binding.msl_sampler = slot;
            }
            compiler.add_msl_resource_binding(binding);
        };

        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        for (const spirv_cross::Resource& resource : resources.uniform_buffers)
        {
            add_msl_binding(resource, true);
        }
        for (const spirv_cross::Resource& resource : resources.storage_buffers)
        {
            add_msl_binding(resource, true);
        }
        for (const spirv_cross::Resource& resource : resources.sampled_images)
        {
            add_msl_binding(resource, false);
        }
        for (const spirv_cross::Resource& resource : resources.separate_images)
        {
            add_msl_binding(resource, false);
        }
        for (const spirv_cross::Resource& resource : resources.separate_samplers)
        {
            add_msl_binding(resource, false);
        }

        spirv_cross::CompilerMSL::Options options;
        // Use the SPIR-V `binding` decoration as the MSL [[buffer(n)]] / [[texture(n)]]
        // index (plus k_msl_descriptor_buffer_offset for buffers -- see above).
        options.enable_decoration_binding      = true;
        // macOS 10.15+ / Metal 2.1: argument buffers stay off (we bind per resource).
        options.set_msl_version(2, 1, 0);
        options.pad_fragment_output_components = true;
        options.invariant_float_math           = true;
        compiler.set_msl_options(options);

        compiler.set_entry_point(entry, model);
        const std::string msl_source = compiler.compile();
        const std::string msl_entry  = compiler.get_cleansed_entry_point_name(entry, model);

        auto* shader = CreateMetalShaderFromMSL(m_Device, msl_source, ToShaderStage(model), entry, msl_entry);
        if (shader != nullptr && model == spv::ExecutionModelGLCompute)
        {
            // MSL 侧 SPIRV-Cross 不一定保留 local_size 属性，SPIR-V 里那份才是权威。
            auto* metal_shader = static_cast<MetalShader*>(shader);
            for (int axis = 0; axis < 3; ++axis)
            {
                metal_shader->workgroupSize[axis] = spirv_workgroup_size[axis];
            }
        }
        return shader;
    }
    catch (const std::exception& error)
    {
        LOG_WARNING(ZRender, "MetalRHI: SPIR-V -> MSL failed: {}", error.what());
        return nullptr;
    }
#else
    (void)shader_code;
    return nullptr;
#endif
}

RHIShader* MetalRHI::CreateShaderModuleFromFile(const std::string& file_path,
                                                ShaderStage       shader_stage,
                                                const std::vector<std::string>&,
                                                const ShaderMacros&,
                                                std::vector<uint8_t>& output_binary,
                                                const std::string& entry_point,
                                                bool)
{
    (void)output_binary;
#ifdef __APPLE__
    // .metal sources are loaded verbatim (the runtime Metal compiler handles
    // #include and preprocessing); anything else is rejected here.
    const bool is_metal_source = file_path.size() > 6 &&
                                 file_path.compare(file_path.size() - 6, 6, ".metal") == 0;
    if (!is_metal_source)
    {
        LOG_WARNING(ZRender, "MetalRHI: '{}' is not MSL source -- skipped", file_path);
        return nullptr;
    }

    FILE* file = std::fopen(file_path.c_str(), "rb");
    if (file == nullptr)
    {
        LOG_WARNING(ZRender, "MetalRHI: cannot open shader '{}'", file_path);
        return nullptr;
    }
    std::string source;
    char        buffer[4096];
    size_t      read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        source.append(buffer, read);
    }
    std::fclose(file);

    return CreateShaderModuleFromSource(source, shader_stage, file_path, {}, {});
#else
    (void)file_path;
    (void)shader_stage;
    (void)entry_point;
    return nullptr;
#endif
}

RHIShader* MetalRHI::CreateShaderModuleFromSource(const std::string& source_code,
                                                  ShaderStage       shader_stage,
                                                  const std::string& shader_name,
                                                  const std::vector<std::string>&,
                                                  const ShaderMacros&)
{
#ifdef __APPLE__
    return CreateMetalShaderFromMSL(m_Device, source_code, shader_stage, shader_name, "main");
#else
    (void)source_code;
    (void)shader_stage;
    (void)shader_name;
    return nullptr;
#endif
}

void MetalRHI::CreateBuffer(RHIDeviceSize          size,
                            RHIBufferUsageFlags    usage,
                            RHIMemoryPropertyFlags properties,
                            RHIBuffer*&            buffer,
                            RHIDeviceMemory*&      buffer_memory)
{
    CreateBufferAndInitialize(usage, properties, buffer, buffer_memory, size, nullptr, 0);
}

void MetalRHI::CreateBufferAndInitialize(RHIBufferUsageFlags    usage,
                                         RHIMemoryPropertyFlags properties,
                                         RHIBuffer*&            buffer,
                                         RHIDeviceMemory*&      buffer_memory,
                                         RHIDeviceSize          size,
                                         void*                  data,
                                         int                    datasize)
{
    buffer        = nullptr;
    buffer_memory = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || size == 0)
    {
        return;
    }

    MTLResourceOptions options = MTLResourceStorageModeShared;
    if ((properties & RHI_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
        (properties & RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
    {
        options = MTLResourceStorageModePrivate;
    }

    // **容量是 `size`，不是 `datasize`**。以前带初值时用 `newBufferWithBytes:length:datasize`
    // 分配，于是 size=4096 / datasize=16 在 Metal 上只得到 16 字节的 buffer，而 Vulkan
    // 分配的是 size（只 memcpy datasize）—— 同一个调用在两个后端拿到不同大小的东西，
    // 往第 16 字节之后写的任何数据都是越界。Nanite 的集群/索引缓冲都是"先按容量建、
    // 后面再整块填"，一旦按 datasize 分配就必然越界。
    const size_t byte_size = static_cast<size_t>(size > 0 ? size : 1);
    const size_t copy_bytes =
        (data != nullptr && datasize > 0) ? std::min(static_cast<size_t>(datasize), byte_size) : 0;

    id<MTLBuffer> mtl_buffer = [m_Device newBufferWithLength:static_cast<NSUInteger>(byte_size) options:options];
    if (mtl_buffer == nil)
    {
        return;
    }

    if (copy_bytes > 0)
    {
        if (options == MTLResourceStorageModePrivate)
        {
            // device-private 缓冲 CPU 写不进去，走一次 staging + blit，这也是 Vulkan
            // 侧同样的语义（VulkanUtil 对非 host-visible 内存根本没法 memcpy）。
            id<MTLBuffer> staging =
                [m_Device newBufferWithBytes:data length:static_cast<NSUInteger>(copy_bytes)
                                     options:MTLResourceStorageModeShared];
            if (staging != nil)
            {
                id<MTLCommandBuffer>     copy_command    = [m_CommandQueue commandBuffer];
                id<MTLBlitCommandEncoder> blit_encoder   = [copy_command blitCommandEncoder];
                [blit_encoder copyFromBuffer:staging
                                sourceOffset:0
                                    toBuffer:mtl_buffer
                           destinationOffset:0
                                        size:static_cast<NSUInteger>(copy_bytes)];
                [blit_encoder endEncoding];
                [copy_command commit];
                [copy_command waitUntilCompleted];
                [staging release];
            }
        }
        else
        {
            std::memcpy(mtl_buffer.contents, data, copy_bytes);
        }
    }
    (void)usage;

    auto* metal_buffer = new MetalBuffer();
    metal_buffer->setResource(mtl_buffer);
    [mtl_buffer release];

    auto* memory = new MetalDeviceMemory();
    memory->setBuffer(metal_buffer->getResource());
    if ((properties & RHI_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ||
        options == MTLResourceStorageModeShared)
    {
        memory->setHostPtr(metal_buffer->getResource().contents);
    }

    buffer        = metal_buffer;
    buffer_memory = memory;
#endif
}

void MetalRHI::CopyBuffer(RHIBuffer* srcBuffer,
                          RHIBuffer* dstBuffer,
                          RHIDeviceSize srcOffset,
                          RHIDeviceSize dstOffset,
                          RHIDeviceSize size)
{
#ifdef __APPLE__
    if (srcBuffer == nullptr || dstBuffer == nullptr || size == 0)
    {
        return;
    }
    RHICommandBuffer* command_buffer = BeginSingleTimeCommands();
    if (command_buffer == nullptr)
    {
        return;
    }
    RHIBufferCopy region {};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size      = size;
    CmdCopyBuffer(command_buffer, srcBuffer, dstBuffer, 1, &region);
    EndSingleTimeCommands(command_buffer);
#endif
}

void MetalRHI::CreateImage(uint32_t              image_width,
                           uint32_t              image_height,
                           RHIFormat             format,
                           RHIImageTiling,
                           RHIImageUsageFlags    image_usage_flags,
                           RHIMemoryPropertyFlags,
                           RHIImage*&            image,
                           RHIDeviceMemory*&     memory,
                           RHIImageCreateFlags,
                           uint32_t              array_layers,
                           uint32_t              miplevels)
{
    image  = nullptr;
    memory = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || image_width == 0 || image_height == 0)
    {
        return;
    }

    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = array_layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    descriptor.pixelFormat = ToMetalFormat(format);
    descriptor.width       = static_cast<NSUInteger>(image_width);
    descriptor.height      = static_cast<NSUInteger>(image_height);
    descriptor.depth       = 1;
    descriptor.mipmapLevelCount = static_cast<NSUInteger>(miplevels > 0 ? miplevels : 1);
    descriptor.arrayLength = static_cast<NSUInteger>(array_layers > 0 ? array_layers : 1);
    descriptor.usage       = ToMetalUsage(image_usage_flags);
    descriptor.storageMode = MTLStorageModePrivate;

    id<MTLTexture> texture = [m_Device newTextureWithDescriptor:descriptor];
    [descriptor release];
    if (texture == nil)
    {
        return;
    }

    auto* metal_image = new MetalImage();
    metal_image->setResource(texture);
    [texture release];

    auto* metal_memory = new MetalDeviceMemory();
    metal_memory->setHostPtr(nullptr);

    image  = metal_image;
    memory = metal_memory;
#endif
}

void MetalRHI::CreateImageView(RHIImage*           image,
                               RHIFormat,
                               RHIImageAspectFlags,
                               RHIImageViewType,
                               uint32_t,
                               uint32_t,
                               RHIImageView*&      image_view)
{
    image_view = nullptr;
#ifdef __APPLE__
    if (image == nullptr)
    {
        return;
    }
    // Metal has no separate view object: an RHIImageView simply re-exposes the
    // same MTLTexture (slices/mips are baked into the texture descriptor).
    auto* view = new MetalImageView();
    view->setResource(((MetalImage*)image)->getResource());
    image_view = view;
#endif
}

void MetalRHI::CreateGlobalImage(RHIImage*&      image,
                                 RHIImageView*&  image_view,
                                 void*,
                                 uint32_t        texture_image_width,
                                 uint32_t        texture_image_height,
                                 void*           texture_image_pixels,
                                 RHIFormat       texture_image_format,
                                 uint32_t        miplevels)
{
    image      = nullptr;
    image_view = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || texture_image_width == 0 || texture_image_height == 0)
    {
        return;
    }

    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType      = MTLTextureType2D;
    descriptor.pixelFormat      = ToMetalFormat(texture_image_format);
    descriptor.width            = static_cast<NSUInteger>(texture_image_width);
    descriptor.height           = static_cast<NSUInteger>(texture_image_height);
    descriptor.mipmapLevelCount = static_cast<NSUInteger>(miplevels > 0 ? miplevels : 1);
    descriptor.usage            = MTLTextureUsageShaderRead;
    descriptor.storageMode      = MTLStorageModeManaged;

    id<MTLTexture> texture = [m_Device newTextureWithDescriptor:descriptor];
    [descriptor release];
    if (texture == nil)
    {
        return;
    }

    if (texture_image_pixels != nullptr)
    {
        const NSUInteger bytes_per_row = static_cast<NSUInteger>(texture_image_width) * 4u;
        [texture replaceRegion:MTLRegionMake2D(0, 0, texture_image_width, texture_image_height)
                   mipmapLevel:0
                     withBytes:texture_image_pixels
                   bytesPerRow:bytes_per_row];
    }

    auto* metal_image = new MetalImage();
    metal_image->setResource(texture);
    [texture release];

    auto* view = new MetalImageView();
    view->setResource(metal_image->getResource());

    auto* metal_memory = new MetalDeviceMemory();
    metal_memory->setHostPtr(nullptr);

    image      = metal_image;
    image_view = view;
#else
    (void)texture_image_width;
    (void)texture_image_height;
    (void)texture_image_pixels;
    (void)texture_image_format;
    (void)miplevels;
#endif
}

void MetalRHI::CreateCubeMap(RHIImage*&           image,
                             RHIImageView*&       image_view,
                             void*,
                             uint32_t             texture_image_width,
                             uint32_t             texture_image_height,
                             std::array<void*, 6> texture_image_pixels,
                             RHIFormat            texture_image_format,
                             uint32_t             miplevels)
{
    image      = nullptr;
    image_view = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || texture_image_width == 0 || texture_image_height == 0)
    {
        return;
    }

    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType      = MTLTextureTypeCube;
    descriptor.pixelFormat      = ToMetalFormat(texture_image_format);
    descriptor.width            = static_cast<NSUInteger>(texture_image_width);
    descriptor.height           = static_cast<NSUInteger>(texture_image_height);
    descriptor.mipmapLevelCount = static_cast<NSUInteger>(miplevels > 0 ? miplevels : 1);
    descriptor.usage            = MTLTextureUsageShaderRead;
    descriptor.storageMode      = MTLStorageModeManaged;

    id<MTLTexture> texture = [m_Device newTextureWithDescriptor:descriptor];
    [descriptor release];
    if (texture == nil)
    {
        return;
    }

    const NSUInteger bytes_per_row = static_cast<NSUInteger>(texture_image_width) * 4u;
    for (uint32_t face = 0; face < 6; ++face)
    {
        if (texture_image_pixels[face] == nullptr)
        {
            continue;
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, texture_image_width, texture_image_height)
                   mipmapLevel:0
                         slice:face
                     withBytes:texture_image_pixels[face]
                   bytesPerRow:bytes_per_row
                 bytesPerImage:bytes_per_row * texture_image_height];
    }

    auto* metal_image = new MetalImage();
    metal_image->setResource(texture);
    [texture release];

    auto* view = new MetalImageView();
    view->setResource(metal_image->getResource());

    image      = metal_image;
    image_view = view;
#else
    (void)texture_image_width;
    (void)texture_image_height;
    (void)texture_image_pixels;
    (void)texture_image_format;
    (void)miplevels;
#endif
}

void MetalRHI::CreateCommandPool()
{
    if (m_RhiCommandPool == nullptr)
    {
        m_RhiCommandPool = new MetalCommandPool();
    }
}

bool MetalRHI::CreateCommandPool(const RHICommandPoolCreateInfo*, RHICommandPool*& pCommandPool)
{
    pCommandPool = new MetalCommandPool();
    return true;
}

bool MetalRHI::CreateDescriptorPool(const RHIDescriptorPoolCreateInfo*, RHIDescriptorPool*& pDescriptorPool)
{
    pDescriptorPool = new MetalDescriptorPool();
    return true;
}

bool MetalRHI::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutCreateInfo* pCreateInfo,
                                         RHIDescriptorSetLayout*&                pSetLayout)
{
    pSetLayout = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }
    auto* layout = new MetalDescriptorSetLayout();
    for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i)
    {
        const RHIDescriptorSetLayoutBinding& binding = pCreateInfo->pBindings[i];
        MetalDescriptorBinding               metal_binding {};
        metal_binding.binding    = binding.binding;
        metal_binding.type       = binding.descriptorType;
        metal_binding.stageFlags = static_cast<uint32_t>(binding.stageFlags);
        layout->bindings.push_back(metal_binding);
    }
    pSetLayout = layout;
    return true;
}

bool MetalRHI::CreateFence(const RHIFenceCreateInfo*, RHIFence*& pFence)
{
    pFence = new MetalFence();
    return true;
}

bool MetalRHI::CreateFramebuffer(const RHIFramebufferCreateInfo* pCreateInfo, RHIFramebuffer*& pFramebuffer)
{
    pFramebuffer = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }

    auto* framebuffer = new MetalFramebuffer();
    framebuffer->width  = pCreateInfo->width;
    framebuffer->height = pCreateInfo->height;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        RHIImageView* view = pCreateInfo->pAttachments[i];
        // A null attachment means "the swapchain drawable" (resolved per frame).
        const bool is_swapchain = view == nullptr;
        id<MTLTexture> texture = is_swapchain ? nil : ((MetalImageView*)view)->getResource();
        framebuffer->textures.push_back(texture != nil ? [texture retain] : nil);
        framebuffer->isSwapchain.push_back(is_swapchain);
    }
    pFramebuffer = framebuffer;
    return true;
}

bool MetalRHI::CreateGraphicsPipelines(RHIPipelineCache*,
                                       uint32_t                          createInfoCount,
                                       const RHIGraphicsPipelineCreateInfo* pCreateInfos,
                                       RHIPipeline*&                      pPipelines)
{
    pPipelines = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || pCreateInfos == nullptr || createInfoCount == 0)
    {
        return false;
    }
    // One call creates one pipeline (see CreateComputePipelines below and the
    // Vulkan backend for the reasoning).
    if (createInfoCount != 1)
    {
        LOG_ERROR(ZRender,
                  "MetalRHI::CreateGraphicsPipelines: createInfoCount must be 1, got {} -- call once per pipeline",
                  createInfoCount);
        return false;
    }

    auto* pipeline = new MetalPipeline();
    bool  ok = true;

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];

    for (uint32_t i = 0; i < pCreateInfos[0].stageCount; ++i)
    {
        const RHIPipelineShaderStageCreateInfo& stage = pCreateInfos[0].pStages[i];
        auto* shader = (MetalShader*)stage.module;
        if (shader == nullptr || shader->getFunction() == nil)
        {
            continue;
        }
        if (stage.stage == RHI_SHADER_STAGE_VERTEX_BIT)
        {
            descriptor.vertexFunction = shader->getFunction();
        }
        else if (stage.stage == RHI_SHADER_STAGE_FRAGMENT_BIT)
        {
            descriptor.fragmentFunction = shader->getFunction();
        }
    }
    if (descriptor.vertexFunction == nil)
    {
        LOG_WARNING(ZRender, "MetalRHI::CreateGraphicsPipelines -- no MSL vertex function");
        ok = false;
    }

    // Colour attachments: formats come from the render pass / framebuffer.
    if (pCreateInfos[0].renderPass != nullptr)
    {
        auto* render_pass = (MetalRenderPass*)pCreateInfos[0].renderPass;
        uint32_t color_index = 0;
        for (const MetalAttachmentDescription& attachment : render_pass->attachments)
        {
            const MTLPixelFormat format = ToMetalFormat(attachment.format);
            if (attachment.isDepthStencil || IsDepthFormat(attachment.format))
            {
                descriptor.depthAttachmentPixelFormat = format;
                descriptor.stencilAttachmentPixelFormat =
                    HasStencilComponent(attachment.format) ? format : MTLPixelFormatInvalid;
            }
            else if (color_index < 8)
            {
                descriptor.colorAttachments[color_index].pixelFormat = format;
                ++color_index;
            }
        }
    }
    else
    {
        descriptor.colorAttachments[0].pixelFormat = ToMetalFormat(m_SwapchainImageFormat);
    }

    // Per-attachment blending -- the ZSlate / UI pipelines alpha-blend their quads.
    if (pCreateInfos[0].pColorBlendState != nullptr && pCreateInfos[0].pColorBlendState->pAttachments != nullptr)
    {
        const uint32_t blend_count =
            std::min<uint32_t>(pCreateInfos[0].pColorBlendState->attachmentCount, 8u);
        for (uint32_t i = 0; i < blend_count; ++i)
        {
            const RHIPipelineColorBlendAttachmentState& blend = pCreateInfos[0].pColorBlendState->pAttachments[i];
            MTLRenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
            attachment.blendingEnabled = blend.blendEnable ? YES : NO;
            if (blend.colorWriteMask != 0)
            {
                MTLColorWriteMask mask = 0;
                if ((blend.colorWriteMask & RHI_COLOR_COMPONENT_R_BIT) != 0) { mask |= MTLColorWriteMaskRed; }
                if ((blend.colorWriteMask & RHI_COLOR_COMPONENT_G_BIT) != 0) { mask |= MTLColorWriteMaskGreen; }
                if ((blend.colorWriteMask & RHI_COLOR_COMPONENT_B_BIT) != 0) { mask |= MTLColorWriteMaskBlue; }
                if ((blend.colorWriteMask & RHI_COLOR_COMPONENT_A_BIT) != 0) { mask |= MTLColorWriteMaskAlpha; }
                attachment.writeMask = mask;
            }
            if (!blend.blendEnable)
            {
                continue;
            }
            attachment.sourceRGBBlendFactor        = ToMetalBlendFactor(blend.srcColorBlendFactor);
            attachment.destinationRGBBlendFactor   = ToMetalBlendFactor(blend.dstColorBlendFactor);
            attachment.rgbBlendOperation           = ToMetalBlendOp(blend.colorBlendOp);
            attachment.sourceAlphaBlendFactor      = ToMetalBlendFactor(blend.srcAlphaBlendFactor);
            attachment.destinationAlphaBlendFactor = ToMetalBlendFactor(blend.dstAlphaBlendFactor);
            attachment.alphaBlendOperation         = ToMetalBlendOp(blend.alphaBlendOp);
        }
    }

    // Vertex layout.
    if (pCreateInfos[0].pVertexInputState != nullptr)
    {
        const RHIPipelineVertexInputStateCreateInfo& vertex_input = *pCreateInfos[0].pVertexInputState;
        MTLVertexDescriptor* vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t i = 0; i < vertex_input.vertexBindingDescriptionCount; ++i)
        {
            const RHIVertexInputBindingDescription& binding = vertex_input.pVertexBindingDescriptions[i];
            vertex_descriptor.layouts[binding.binding].stride = static_cast<NSUInteger>(binding.stride);
            vertex_descriptor.layouts[binding.binding].stepFunction =
                binding.inputRate == RHI_VERTEX_INPUT_RATE_INSTANCE ? MTLVertexStepFunctionPerInstance
                                                                    : MTLVertexStepFunctionPerVertex;
        }
        for (uint32_t i = 0; i < vertex_input.vertexAttributeDescriptionCount; ++i)
        {
            const RHIVertexInputAttributeDescription& attribute = vertex_input.pVertexAttributeDescriptions[i];
            vertex_descriptor.attributes[attribute.location].format   = ToMetalVertexFormat(attribute.format);
            vertex_descriptor.attributes[attribute.location].offset   = static_cast<NSUInteger>(attribute.offset);
            vertex_descriptor.attributes[attribute.location].bufferIndex = attribute.binding;
        }
        descriptor.vertexDescriptor = vertex_descriptor;
    }

    if (ok)
    {
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [m_Device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (state == nil)
        {
            LOG_WARNING(ZRender,
                        "MetalRHI::CreateGraphicsPipelines -- {}",
                        error != nil ? error.localizedDescription.UTF8String : "unknown error");
            ok = false;
        }
        else
        {
            pipeline->renderState = [state retain];
            [state release];
        }
    }
    [descriptor release];

    // Depth-stencil state.
    MTLDepthStencilDescriptor* depth_descriptor = [[MTLDepthStencilDescriptor alloc] init];
    if (pCreateInfos[0].pDepthStencilState != nullptr &&
        pCreateInfos[0].pDepthStencilState->depthTestEnable)
    {
        depth_descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
        depth_descriptor.depthWriteEnabled    = pCreateInfos[0].pDepthStencilState->depthWriteEnable ? YES : NO;
    }
    else
    {
        depth_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
        depth_descriptor.depthWriteEnabled    = NO;
    }
    id<MTLDepthStencilState> depth_state = [m_Device newDepthStencilStateWithDescriptor:depth_descriptor];
    pipeline->depthState = depth_state != nil ? [depth_state retain] : nil;
    [depth_state release];
    [depth_descriptor release];

    if (pCreateInfos[0].pRasterizationState != nullptr)
    {
        switch (pCreateInfos[0].pRasterizationState->cullMode)
        {
            case RHI_CULL_MODE_BACK_BIT:  pipeline->cullMode = MTLCullModeBack; break;
            case RHI_CULL_MODE_FRONT_BIT: pipeline->cullMode = MTLCullModeFront; break;
            default:                      pipeline->cullMode = MTLCullModeNone; break;
        }
        // **绕序要反过来映射。** Metal 的 winding 是在**帧缓冲空间**里判定的，而
        // Metal 帧缓冲的 y 轴朝下（原点在左上角），NDC 的 y 轴朝上 —— 从 NDC 到
        // 帧缓冲这一次变换把绕序翻了个面。DX12 的 `FrontCounterClockwise` 和 Vulkan
        // 的 VK_FRONT_FACE_COUNTER_CLOCKWISE 都是按"NDC 里逆时针 = 正面"来的，所以
        // 想让同一个 frontFace 在三个后端上表现一致，Metal 这一侧必须取反。
        //
        // 这不是纸上推导出来的：Nanite 的光栅冒烟测试用一张法线朝 +Y、从上方俯视时
        // 在 NDC 里逆时针的平面验证过 —— 按 CCW 映射它被整片剔掉（0 像素），按 CW
        // 映射才画出来（253 像素）。此前引擎在 Metal 上只画 UI（cullMode 全是
        // NONE），所以这条路径从来没有被走到过。
        pipeline->winding = pCreateInfos[0].pRasterizationState->frontFace == RHI_FRONT_FACE_COUNTER_CLOCKWISE
                                ? MTLWindingClockwise
                                : MTLWindingCounterClockwise;
    }
    if (pCreateInfos[0].pInputAssemblyState != nullptr)
    {
        switch (pCreateInfos[0].pInputAssemblyState->topology)
        {
            case RHI_PRIMITIVE_TOPOLOGY_LINE_LIST:      pipeline->primitiveType = MTLPrimitiveTypeLine; break;
            case RHI_PRIMITIVE_TOPOLOGY_LINE_STRIP:     pipeline->primitiveType = MTLPrimitiveTypeLineStrip; break;
            case RHI_PRIMITIVE_TOPOLOGY_POINT_LIST:     pipeline->primitiveType = MTLPrimitiveTypePoint; break;
            case RHI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: pipeline->primitiveType = MTLPrimitiveTypeTriangleStrip; break;
            default:                                    pipeline->primitiveType = MTLPrimitiveTypeTriangle; break;
        }
    }

    if (!ok)
    {
        delete pipeline;
        return false;
    }
    pPipelines = pipeline;
    return true;
#else
    (void)createInfoCount;
    (void)pCreateInfos;
    return false;
#endif
}

bool MetalRHI::CreateComputePipelines(RHIPipelineCache*,
                                      uint32_t                        createInfoCount,
                                      const RHIComputePipelineCreateInfo* pCreateInfos,
                                      RHIPipeline*&                    pPipelines)
{
    pPipelines = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || pCreateInfos == nullptr || createInfoCount == 0)
    {
        return false;
    }
    // One call creates one pipeline -- the same contract Vulkan and DX12 now
    // enforce. This backend only ever read pCreateInfos[0], so a count > 1 would
    // have silently dropped the rest; reject it instead.
    if (createInfoCount != 1)
    {
        LOG_ERROR(ZRender,
                  "MetalRHI::CreateComputePipelines: createInfoCount must be 1, got {} -- call once per pipeline",
                  createInfoCount);
        return false;
    }
    auto* shader = pCreateInfos[0].pStages != nullptr ? (MetalShader*)pCreateInfos[0].pStages->module : nullptr;
    if (shader == nullptr || shader->getFunction() == nil)
    {
        return false;
    }

    NSError* error = nil;
    id<MTLComputePipelineState> state = [m_Device newComputePipelineStateWithFunction:shader->getFunction() error:&error];
    if (state == nil)
    {
        LOG_WARNING(ZRender,
                    "MetalRHI::CreateComputePipelines -- {}",
                    error != nil ? error.localizedDescription.UTF8String : "unknown error");
        return false;
    }

    auto* pipeline = new MetalPipeline();
    pipeline->computeState = [state retain];
    [state release];
    // CmdDispatch 只能看到 pipeline，看不到 shader，所以在这里抄一份。
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        pipeline->workgroupSize[axis] = shader->workgroupSize[axis];
    }
    pPipelines = pipeline;
    return true;
#else
    (void)createInfoCount;
    (void)pCreateInfos;
    return false;
#endif
}

bool MetalRHI::CreatePipelineLayout(const RHIPipelineLayoutCreateInfo* pCreateInfo,
                                    RHIPipelineLayout*&                pPipelineLayout)
{
    pPipelineLayout = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }
    auto* layout = new MetalPipelineLayout();
    for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; ++i)
    {
        layout->setLayouts.push_back((MetalDescriptorSetLayout*)pCreateInfo->pSetLayouts[i]);
    }
    pPipelineLayout = layout;
    return true;
}

bool MetalRHI::CreateRenderPass(const RHIRenderPassCreateInfo* pCreateInfo, RHIRenderPass*& pRenderPass)
{
    pRenderPass = nullptr;
    if (pCreateInfo == nullptr)
    {
        return false;
    }
    auto* render_pass = new MetalRenderPass();
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        const RHIAttachmentDescription& attachment = pCreateInfo->pAttachments[i];
        MetalAttachmentDescription      metal_attachment {};
        metal_attachment.format        = attachment.format;
        metal_attachment.loadOp        = attachment.loadOp;
        metal_attachment.storeOp       = attachment.storeOp;
        metal_attachment.isDepthStencil = IsDepthFormat(attachment.format);
        render_pass->attachments.push_back(metal_attachment);
    }
    pRenderPass = render_pass;
    return true;
}

bool MetalRHI::CreateSampler(const RHISamplerCreateInfo* pCreateInfo, RHISampler*& pSampler)
{
    pSampler = nullptr;
#ifdef __APPLE__
    if (m_Device == nil || pCreateInfo == nullptr)
    {
        return false;
    }
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.minFilter = pCreateInfo->minFilter == RHI_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear
                                                                       : MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = pCreateInfo->magFilter == RHI_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear
                                                                       : MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = pCreateInfo->mipmapMode == RHI_SAMPLER_MIPMAP_MODE_LINEAR ? MTLSamplerMipFilterLinear
                                                                                     : MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = ToMetalAddressMode(pCreateInfo->addressModeU);
    descriptor.tAddressMode = ToMetalAddressMode(pCreateInfo->addressModeV);
    descriptor.rAddressMode = ToMetalAddressMode(pCreateInfo->addressModeW);
    descriptor.maxAnisotropy = static_cast<NSUInteger>(pCreateInfo->maxAnisotropy > 0.0f ? pCreateInfo->maxAnisotropy : 1.0f);
    descriptor.lodMinClamp   = pCreateInfo->minLod;
    descriptor.lodMaxClamp   = pCreateInfo->maxLod > 0.0f ? pCreateInfo->maxLod : 16.0f;

    id<MTLSamplerState> state = [m_Device newSamplerStateWithDescriptor:descriptor];
    [descriptor release];
    if (state == nil)
    {
        return false;
    }
    auto* sampler = new MetalSampler();
    sampler->setResource(state);
    [state release];
    pSampler = sampler;
    return true;
#else
    (void)pCreateInfo;
    return false;
#endif
}

bool MetalRHI::CreateSemaphore(const RHISemaphoreCreateInfo*, RHISemaphore*& pSemaphore)
{
    pSemaphore = new MetalSemaphore();
    return true;
}

bool MetalRHI::WaitForFencesPFN(uint32_t fenceCount, RHIFence* const* pFence, RHIBool32, uint64_t)
{
    for (uint32_t i = 0; i < fenceCount; ++i)
    {
        auto* fence = (MetalFence*)pFence[i];
        if (fence == nullptr)
        {
            continue;
        }
        id<MTLCommandBuffer> buffer = fence->getCommandBuffer();
        if (buffer != nil)
        {
            [buffer waitUntilCompleted];
        }
        fence->signaled = true;
    }
    return true;
}

bool MetalRHI::ResetFencesPFN(uint32_t fenceCount, RHIFence* const* pFences)
{
    for (uint32_t i = 0; i < fenceCount; ++i)
    {
        auto* fence = (MetalFence*)pFences[i];
        if (fence == nullptr)
        {
            continue;
        }
        fence->signaled = false;
        fence->setCommandBuffer(nil);
    }
    return true;
}

bool MetalRHI::ResetCommandPoolPFN(RHICommandPool*, RHICommandPoolResetFlags) { return true; }

bool MetalRHI::BeginCommandBufferPFN(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo*)
{
    return BeginCommandBuffer(commandBuffer, nullptr);
}

bool MetalRHI::EndCommandBufferPFN(RHICommandBuffer* commandBuffer)
{
    return EndCommandBuffer(commandBuffer);
}

void MetalRHI::CmdBeginRenderPassPFN(RHICommandBuffer*              commandBuffer,
                                     const RHIRenderPassBeginInfo*  pRenderPassBegin,
                                     RHISubpassContents)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->commandBuffer == nil || pRenderPassBegin == nullptr)
    {
        return;
    }
    auto* render_pass  = (MetalRenderPass*)pRenderPassBegin->renderPass;
    auto* framebuffer  = (MetalFramebuffer*)pRenderPassBegin->framebuffer;
    if (framebuffer == nullptr)
    {
        return;
    }

    // 前面如果有 compute（Nanite 的剔除就是 dispatch → draw），必须先关掉 compute
    // encoder 才能开 render encoder。CmdDispatch 不再自己收尾，所以这一步在这里。
    EndComputeEncoder(metal_buffer);

    MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    const size_t attachment_count = framebuffer->textures.size();
    uint32_t     color_index = 0;
    for (size_t i = 0; i < attachment_count; ++i)
    {
        id<MTLTexture> texture = framebuffer->textures[i];
        if (framebuffer->isSwapchain[i] || texture == nil)
        {
            texture = m_CurrentDrawable != nil ? m_CurrentDrawable.texture : nil;
        }
        if (texture == nil)
        {
            continue;
        }

        const bool is_depth = render_pass != nullptr && i < render_pass->attachments.size()
                                  ? render_pass->attachments[i].isDepthStencil
                                  : (texture.pixelFormat == MTLPixelFormatDepth32Float ||
                                     texture.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8 ||
                                     texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8);

        RHIAttachmentLoadOp  load_op  = RHI_ATTACHMENT_LOAD_OP_LOAD;
        RHIAttachmentStoreOp store_op = RHI_ATTACHMENT_STORE_OP_STORE;
        if (render_pass != nullptr && i < render_pass->attachments.size())
        {
            load_op  = render_pass->attachments[i].loadOp;
            store_op = render_pass->attachments[i].storeOp;
        }

        if (is_depth)
        {
            descriptor.depthAttachment.texture     = texture;
            descriptor.depthAttachment.loadAction  = load_op == RHI_ATTACHMENT_LOAD_OP_CLEAR ? MTLLoadActionClear
                                                                                             : MTLLoadActionLoad;
            descriptor.depthAttachment.storeAction = store_op == RHI_ATTACHMENT_STORE_OP_STORE ? MTLStoreActionStore
                                                                                               : MTLStoreActionDontCare;
            if (load_op == RHI_ATTACHMENT_LOAD_OP_CLEAR && pRenderPassBegin->pClearValues != nullptr &&
                i < pRenderPassBegin->clearValueCount)
            {
                descriptor.depthAttachment.clearDepth = pRenderPassBegin->pClearValues[i].depthStencil.depth;
            }
        }
        else if (color_index < 8)
        {
            descriptor.colorAttachments[color_index].texture     = texture;
            descriptor.colorAttachments[color_index].loadAction  = load_op == RHI_ATTACHMENT_LOAD_OP_CLEAR
                                                                       ? MTLLoadActionClear
                                                                       : MTLLoadActionLoad;
            descriptor.colorAttachments[color_index].storeAction = store_op == RHI_ATTACHMENT_STORE_OP_STORE
                                                                       ? MTLStoreActionStore
                                                                       : MTLStoreActionDontCare;
            if (load_op == RHI_ATTACHMENT_LOAD_OP_CLEAR && pRenderPassBegin->pClearValues != nullptr &&
                i < pRenderPassBegin->clearValueCount)
            {
                const RHIClearColorValue& clear = pRenderPassBegin->pClearValues[i].color;
                descriptor.colorAttachments[color_index].clearColor =
                    MTLClearColorMake(clear.float32[0], clear.float32[1], clear.float32[2], clear.float32[3]);
            }
            ++color_index;
        }
    }

    id<MTLRenderCommandEncoder> encoder = [metal_buffer->commandBuffer renderCommandEncoderWithDescriptor:descriptor];
    if (metal_buffer->renderEncoder != nil)
    {
        [metal_buffer->renderEncoder release];
    }
    metal_buffer->renderEncoder = [encoder retain];
    [encoder release];
#endif
}

void MetalRHI::CmdNextSubpassPFN(RHICommandBuffer*, RHISubpassContents)
{
    // Metal has no subpasses: the pass is recorded as one encoder.
}

void MetalRHI::CmdEndRenderPassPFN(RHICommandBuffer* commandBuffer)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil)
    {
        return;
    }
    [metal_buffer->renderEncoder endEncoding];
    [metal_buffer->renderEncoder release];
    metal_buffer->renderEncoder = nil;
#endif
}

void MetalRHI::CmdBindPipelinePFN(RHICommandBuffer*    commandBuffer,
                                  RHIPipelineBindPoint pipelineBindPoint,
                                  RHIPipeline*         pipeline)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* metal_pipeline = (MetalPipeline*)pipeline;
    if (metal_buffer == nullptr || metal_pipeline == nullptr)
    {
        return;
    }
    if (pipelineBindPoint == RHI_PIPELINE_BIND_POINT_COMPUTE)
    {
        metal_buffer->computePipeline = metal_pipeline;
        // 把 encoder 在这里就开起来：调用方接下来的动作是绑描述符再 dispatch，
        // 而绑定需要 encoder 已经存在（见 BeginComputeEncoder 的注释）。
        BeginComputeEncoder(metal_buffer);
        return;
    }
    metal_buffer->graphicsPipeline = metal_pipeline;
    if (metal_buffer->renderEncoder == nil)
    {
        return;
    }
    if (metal_pipeline->renderState != nil)
    {
        [metal_buffer->renderEncoder setRenderPipelineState:metal_pipeline->renderState];
    }
    if (metal_pipeline->depthState != nil)
    {
        [metal_buffer->renderEncoder setDepthStencilState:metal_pipeline->depthState];
    }
    [metal_buffer->renderEncoder setCullMode:metal_pipeline->cullMode];
    [metal_buffer->renderEncoder setFrontFacingWinding:metal_pipeline->winding];
#endif
}

void MetalRHI::CmdSetViewportPFN(RHICommandBuffer*  commandBuffer,
                                 uint32_t           firstViewport,
                                 uint32_t           viewportCount,
                                 const RHIViewport* pViewports)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil || pViewports == nullptr)
    {
        return;
    }
    if (firstViewport != 0 || viewportCount == 0)
    {
        return;
    }
    const MTLViewport viewport = {pViewports[0].x,
                                  pViewports[0].y,
                                  pViewports[0].width,
                                  pViewports[0].height,
                                  pViewports[0].minDepth,
                                  pViewports[0].maxDepth};
    [metal_buffer->renderEncoder setViewport:viewport];
#endif
}

void MetalRHI::CmdSetScissorPFN(RHICommandBuffer* commandBuffer,
                                uint32_t          firstScissor,
                                uint32_t          scissorCount,
                                const RHIRect2D*  pScissors)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil || pScissors == nullptr)
    {
        return;
    }
    if (firstScissor != 0 || scissorCount == 0)
    {
        return;
    }
    const MTLScissorRect rect = {static_cast<NSUInteger>(pScissors[0].offset.x),
                                 static_cast<NSUInteger>(pScissors[0].offset.y),
                                 static_cast<NSUInteger>(pScissors[0].extent.width),
                                 static_cast<NSUInteger>(pScissors[0].extent.height)};
    [metal_buffer->renderEncoder setScissorRect:rect];
#endif
}

void MetalRHI::CmdBindVertexBuffersPFN(RHICommandBuffer*    commandBuffer,
                                       uint32_t             firstBinding,
                                       uint32_t             bindingCount,
                                       RHIBuffer* const*    pBuffers,
                                       const RHIDeviceSize* pOffsets)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil || pBuffers == nullptr)
    {
        return;
    }
    for (uint32_t i = 0; i < bindingCount; ++i)
    {
        auto* buffer = (MetalBuffer*)pBuffers[i];
        if (buffer == nullptr || buffer->getResource() == nil)
        {
            continue;
        }
        const NSUInteger offset = pOffsets != nullptr ? static_cast<NSUInteger>(pOffsets[i]) : 0;
        [metal_buffer->renderEncoder setVertexBuffer:buffer->getResource()
                                              offset:offset
                                             atIndex:firstBinding + i];
    }
#endif
}

void MetalRHI::CmdBindIndexBufferPFN(RHICommandBuffer* commandBuffer,
                                     RHIBuffer*        buffer,
                                     RHIDeviceSize     offset,
                                     RHIIndexType      indexType)
{
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* metal_buffer_resource = (MetalBuffer*)buffer;
    if (metal_buffer == nullptr || metal_buffer_resource == nullptr)
    {
        return;
    }
    if (metal_buffer->indexBuffer != nil)
    {
        [metal_buffer->indexBuffer release];
    }
    metal_buffer->indexBuffer       = [metal_buffer_resource->getResource() retain];
    metal_buffer->indexBufferOffset = offset;
    metal_buffer->indexType         = indexType == RHI_INDEX_TYPE_UINT16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
}

void MetalRHI::CmdBindDescriptorSetsPFN(RHICommandBuffer*           commandBuffer,
                                        RHIPipelineBindPoint        pipelineBindPoint,
                                        RHIPipelineLayout*          layout,
                                        uint32_t                    firstSet,
                                        uint32_t                    descriptorSetCount,
                                        const RHIDescriptorSet* const* pDescriptorSets,
                                        uint32_t                    dynamicOffsetCount,
                                        const uint32_t*             pDynamicOffsets)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || pDescriptorSets == nullptr)
    {
        return;
    }
    (void)layout;
    (void)dynamicOffsetCount;
    (void)pDynamicOffsets;

    const bool compute = pipelineBindPoint == RHI_PIPELINE_BIND_POINT_COMPUTE;
    for (uint32_t s = 0; s < descriptorSetCount; ++s)
    {
        auto* set = (MetalDescriptorSet*)pDescriptorSets[s];
        if (set == nullptr)
        {
            continue;
        }
        // Metal indices are flat; Vulkan descriptors are (set, binding). Must match
        // the slice assignment in CreateShaderModule().
        const uint32_t set_index = firstSet + s;
        for (const MetalDescriptorBinding& binding : set->bindings)
        {
            const bool vertex_stage = (binding.stageFlags & RHI_SHADER_STAGE_VERTEX_BIT) != 0;
            const bool fragment_stage = (binding.stageFlags & RHI_SHADER_STAGE_FRAGMENT_BIT) != 0;
            // Metal keeps vertex buffers and argument buffers in the same index
            // space, so descriptor buffers are offset past the vertex slots.
            const NSUInteger slot          = set_index * k_msl_descriptor_set_stride + binding.binding;
            const NSUInteger buffer_index  = slot + k_msl_descriptor_buffer_offset;
            const NSUInteger texture_index = slot;

            if (binding.buffer != nil)
            {
                if (compute)
                {
                    // encoder 在这里按需开：这一句通常跑在 CmdDispatch 之前，
                    // 不能指望 dispatch 已经把 encoder 准备好了。
                    if (BeginComputeEncoder(metal_buffer) == nil)
                    {
                        continue;
                    }
                    // **必须和 CreateShaderModule() 里 SPIRV-Cross 的
                    // `binding.msl_buffer = slot + k_msl_descriptor_buffer_offset`
                    // 用同一个索引**。以前这里用的是 texture_index（= slot，没有 +8），
                    // 于是 compute 的 storage buffer 全部绑到错误的 [[buffer(n)]] 上：
                    // kernel 读到的不是集群数据，而是别的槽位（通常是空/未定义），
                    // 剔除结果恒为垃圾。
                    [metal_buffer->computeEncoder setBuffer:binding.buffer
                                                     offset:static_cast<NSUInteger>(binding.offset)
                                                    atIndex:buffer_index];
                }
                else if (metal_buffer->renderEncoder != nil)
                {
                    if (vertex_stage || (!fragment_stage))
                    {
                        [metal_buffer->renderEncoder setVertexBuffer:binding.buffer
                                                              offset:static_cast<NSUInteger>(binding.offset)
                                                             atIndex:buffer_index];
                    }
                    if (fragment_stage)
                    {
                        [metal_buffer->renderEncoder setFragmentBuffer:binding.buffer
                                                                offset:static_cast<NSUInteger>(binding.offset)
                                                               atIndex:buffer_index];
                    }
                }
            }
            if (binding.texture != nil && metal_buffer->renderEncoder != nil)
            {
                if (vertex_stage || (!fragment_stage))
                {
                    [metal_buffer->renderEncoder setVertexTexture:binding.texture atIndex:texture_index];
                }
                if (fragment_stage)
                {
                    [metal_buffer->renderEncoder setFragmentTexture:binding.texture atIndex:texture_index];
                }
            }
            if (binding.sampler != nil && metal_buffer->renderEncoder != nil)
            {
                if (vertex_stage || (!fragment_stage))
                {
                    [metal_buffer->renderEncoder setVertexSamplerState:binding.sampler atIndex:texture_index];
                }
                if (fragment_stage)
                {
                    [metal_buffer->renderEncoder setFragmentSamplerState:binding.sampler atIndex:texture_index];
                }
            }
        }
    }
#endif
}

void MetalRHI::CmdDrawIndexedPFN(RHICommandBuffer* commandBuffer,
                                 uint32_t          indexCount,
                                 uint32_t          instanceCount,
                                 uint32_t          firstIndex,
                                 int32_t           vertexOffset,
                                 uint32_t          firstInstance)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil || metal_buffer->indexBuffer == nil)
    {
        return;
    }
    const NSUInteger index_size = metal_buffer->indexType == MTLIndexTypeUInt16 ? 2u : 4u;
    const NSUInteger index_offset = static_cast<NSUInteger>(metal_buffer->indexBufferOffset) +
                                    static_cast<NSUInteger>(firstIndex) * index_size;
    [metal_buffer->renderEncoder drawIndexedPrimitives:metal_buffer->graphicsPipeline != nullptr
                                                           ? metal_buffer->graphicsPipeline->primitiveType
                                                           : MTLPrimitiveTypeTriangle
                                            indexCount:indexCount
                                             indexType:metal_buffer->indexType
                                           indexBuffer:metal_buffer->indexBuffer
                                     indexBufferOffset:index_offset
                                         instanceCount:instanceCount
                                            baseVertex:static_cast<NSInteger>(vertexOffset)
                                          baseInstance:firstInstance];
#endif
}

void MetalRHI::CmdDrawIndexedIndirect(RHICommandBuffer* commandBuffer,
                                      RHIBuffer* buffer,
                                      RHIDeviceSize offset,
                                      uint32_t drawCount,
                                      uint32_t stride)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* metal_indirect_buffer = (MetalBuffer*)buffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil || metal_buffer->indexBuffer == nil ||
        metal_indirect_buffer == nullptr)
    {
        return;
    }

    // Metal has no multi-draw indirect, so a batch becomes a loop of single
    // draws -- still GPU-driven (the counts come from the buffer), just not a
    // single call. MTLDrawIndexedPrimitivesIndirectArguments is byte-identical
    // to RHIDrawIndexedIndirectCommand, so `stride` steps command to command.
    //
    // firstIndex is NOT applied here on purpose. The per-command
    // RHIDrawIndexedIndirectCommand::firstIndex is read by the GPU out of the
    // indirect buffer and lands in MTLDrawIndexedPrimitivesIndirectArguments::
    // indexStart, whose unit (index elements) already matches Vulkan's
    // firstIndex and D3D12's StartIndexLocation. indexBufferOffset below is the
    // pure byte base recorded by CmdBindIndexBufferPFN -- do not add
    // firstIndex * index_size to it the way CmdDrawIndexedPFN does: that
    // variant takes firstIndex as a CPU argument and this one never sees it.
    const MTLPrimitiveType primitive_type =
        metal_buffer->graphicsPipeline != nullptr ? metal_buffer->graphicsPipeline->primitiveType
                                                  : MTLPrimitiveTypeTriangle;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        const NSUInteger command_offset =
            static_cast<NSUInteger>(offset) + static_cast<NSUInteger>(i) * static_cast<NSUInteger>(stride);

        [metal_buffer->renderEncoder drawIndexedPrimitives:primitive_type
                                                 indexType:metal_buffer->indexType
                                               indexBuffer:metal_buffer->indexBuffer
                                         indexBufferOffset:static_cast<NSUInteger>(metal_buffer->indexBufferOffset)
                                            indirectBuffer:metal_indirect_buffer->getResource()
                                      indirectBufferOffset:command_offset];
    }
#endif
}

void MetalRHI::CmdDrawIndirect(RHICommandBuffer* commandBuffer,
                               RHIBuffer* buffer,
                               RHIDeviceSize offset,
                               uint32_t drawCount,
                               uint32_t stride)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* metal_indirect_buffer = (MetalBuffer*)buffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil || metal_indirect_buffer == nullptr)
    {
        return;
    }

    const MTLPrimitiveType primitive_type =
        metal_buffer->graphicsPipeline != nullptr ? metal_buffer->graphicsPipeline->primitiveType
                                                  : MTLPrimitiveTypeTriangle;
    for (uint32_t i = 0; i < drawCount; ++i)
    {
        const NSUInteger command_offset =
            static_cast<NSUInteger>(offset) + static_cast<NSUInteger>(i) * static_cast<NSUInteger>(stride);

        [metal_buffer->renderEncoder drawPrimitives:primitive_type
                                     indirectBuffer:metal_indirect_buffer->getResource()
                               indirectBufferOffset:command_offset];
    }
#endif
}

void MetalRHI::CmdClearAttachmentsPFN(RHICommandBuffer*, uint32_t, const RHIClearAttachment*, uint32_t, const RHIClearRect*)
{
    // Metal has no mid-encoder clear: passes declare their clear values up front
    // (see CmdBeginRenderPassPFN).
}

bool MetalRHI::BeginCommandBuffer(RHICommandBuffer* commandBuffer, const RHICommandBufferBeginInfo*)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || m_CommandQueue == nil)
    {
        return false;
    }
    if (metal_buffer->commandBuffer != nil)
    {
        [metal_buffer->commandBuffer release];
    }
    id<MTLCommandBuffer> command_buffer = [m_CommandQueue commandBuffer];
    metal_buffer->commandBuffer = [command_buffer retain];
    [command_buffer release];
    return metal_buffer->commandBuffer != nil;
#else
    (void)commandBuffer;
    return false;
#endif
}

void MetalRHI::CmdCopyImageToBuffer(RHICommandBuffer*          commandBuffer,
                                    RHIImage*                  srcImage,
                                    RHIImageLayout,
                                    RHIBuffer*                 dstBuffer,
                                    uint32_t                   regionCount,
                                    const RHIBufferImageCopy*  pRegions)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* texture = (MetalImage*)srcImage;
    auto* buffer = (MetalBuffer*)dstBuffer;
    if (metal_buffer == nullptr || texture == nullptr || buffer == nullptr)
    {
        return;
    }
    id<MTLBlitCommandEncoder> encoder = [metal_buffer->commandBuffer blitCommandEncoder];
    for (uint32_t i = 0; i < regionCount; ++i)
    {
        const RHIBufferImageCopy& region = pRegions[i];
        [encoder copyFromTexture:texture->getResource()
                     sourceSlice:region.imageSubresource.baseArrayLayer
                     sourceLevel:region.imageSubresource.mipLevel
                    sourceOrigin:MTLOriginMake(region.imageOffset.x, region.imageOffset.y, region.imageOffset.z)
                      sourceSize:MTLSizeMake(region.imageExtent.width, region.imageExtent.height, region.imageExtent.depth)
                        toBuffer:buffer->getResource()
               destinationOffset:static_cast<NSUInteger>(region.bufferOffset)
          destinationBytesPerRow:region.bufferRowLength > 0 ? region.bufferRowLength : region.imageExtent.width * 4u
        destinationBytesPerImage:0
                         options:MTLBlitOptionNone];
    }
    [encoder endEncoding];
#endif
}

void MetalRHI::CmdCopyImageToImage(RHICommandBuffer*        commandBuffer,
                                   RHIImage*                srcImage,
                                   RHIImageAspectFlagBits,
                                   RHIImage*                dstImage,
                                   RHIImageAspectFlagBits,
                                   uint32_t                 width,
                                   uint32_t                 height)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* source = (MetalImage*)srcImage;
    auto* destination = (MetalImage*)dstImage;
    if (metal_buffer == nullptr || source == nullptr || destination == nullptr)
    {
        return;
    }
    id<MTLBlitCommandEncoder> encoder = [metal_buffer->commandBuffer blitCommandEncoder];
    [encoder copyFromTexture:source->getResource()
                 toTexture:destination->getResource()];
    [encoder endEncoding];
    (void)width;
    (void)height;
#endif
}

void MetalRHI::CmdBlitImage(RHICommandBuffer* commandBuffer,
                            RHIImage*         srcImage,
                            RHIImageLayout,
                            RHIImage*         dstImage,
                            RHIImageLayout,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            uint32_t,
                            RHIFilter)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* source = (MetalImage*)srcImage;
    auto* destination = (MetalImage*)dstImage;
    if (metal_buffer == nullptr || source == nullptr || destination == nullptr)
    {
        return;
    }
    id<MTLBlitCommandEncoder> encoder = [metal_buffer->commandBuffer blitCommandEncoder];
    [encoder copyFromTexture:source->getResource() toTexture:destination->getResource()];
    [encoder endEncoding];
#endif
}

void MetalRHI::CmdCopyBuffer(RHICommandBuffer* commandBuffer,
                             RHIBuffer*        srcBuffer,
                             RHIBuffer*        dstBuffer,
                             uint32_t          regionCount,
                             RHIBufferCopy*    pRegions)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* source = (MetalBuffer*)srcBuffer;
    auto* destination = (MetalBuffer*)dstBuffer;
    if (metal_buffer == nullptr || source == nullptr || destination == nullptr)
    {
        return;
    }
    id<MTLBlitCommandEncoder> encoder = [metal_buffer->commandBuffer blitCommandEncoder];
    for (uint32_t i = 0; i < regionCount; ++i)
    {
        [encoder copyFromBuffer:source->getResource()
                   sourceOffset:static_cast<NSUInteger>(pRegions[i].srcOffset)
                       toBuffer:destination->getResource()
              destinationOffset:static_cast<NSUInteger>(pRegions[i].dstOffset)
                           size:static_cast<NSUInteger>(pRegions[i].size)];
    }
    [encoder endEncoding];
#endif
}

void MetalRHI::CmdDraw(RHICommandBuffer* commandBuffer,
                       uint32_t          vertexCount,
                       uint32_t          instanceCount,
                       uint32_t          firstVertex,
                       uint32_t          firstInstance)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->renderEncoder == nil)
    {
        return;
    }
    [metal_buffer->renderEncoder drawPrimitives:metal_buffer->graphicsPipeline != nullptr
                                                    ? metal_buffer->graphicsPipeline->primitiveType
                                                    : MTLPrimitiveTypeTriangle
                                    vertexStart:firstVertex
                                    vertexCount:vertexCount
                                  instanceCount:instanceCount
                                   baseInstance:firstInstance];
#endif
}

void MetalRHI::CmdDispatch(RHICommandBuffer* commandBuffer,
                           uint32_t          groupCountX,
                           uint32_t          groupCountY,
                           uint32_t          groupCountZ)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr || metal_buffer->commandBuffer == nil)
    {
        return;
    }
    if (BeginComputeEncoder(metal_buffer) == nil)
    {
        return;
    }
    if (metal_buffer->computePipeline != nullptr && metal_buffer->computePipeline->computeState != nil)
    {
        [metal_buffer->computeEncoder setComputePipelineState:metal_buffer->computePipeline->computeState];
    }

    // threadsPerThreadgroup 必须跟 kernel 声明的 local_size 一致 —— 以前硬编码
    // (1,1,1)，于是 `local_size_x = 256` 的 kernel 在 Metal 上只有一个线程干活，
    // 和 Vulkan 同一份 shader 的行为完全不同。
    MTLSize threads_per_group = MTLSizeMake(1, 1, 1);
    if (metal_buffer->computePipeline != nullptr)
    {
        threads_per_group = MTLSizeMake(metal_buffer->computePipeline->workgroupSize[0],
                                        metal_buffer->computePipeline->workgroupSize[1],
                                        metal_buffer->computePipeline->workgroupSize[2]);
    }
    [metal_buffer->computeEncoder dispatchThreadgroups:MTLSizeMake(groupCountX, groupCountY, groupCountZ)
                                 threadsPerThreadgroup:threads_per_group];
    // **不要在这里 endEncoding**：一次 dispatch 就把 encoder 关掉的话，后面再
    // CmdBindDescriptorSets / 再 CmdDispatch 全部静默失效（绑定要求 computeEncoder
    // 非 nil）。encoder 的生命周期交给 EndCommandBuffer / CmdBeginRenderPassPFN /
    // QueueSubmit，它们都已经会收尾。
#endif
}

void MetalRHI::CmdDispatchIndirect(RHICommandBuffer* commandBuffer, RHIBuffer* buffer, RHIDeviceSize offset)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    auto* metal_buffer_resource = (MetalBuffer*)buffer;
    if (metal_buffer == nullptr || metal_buffer_resource == nullptr)
    {
        return;
    }
    if (metal_buffer->computeEncoder == nil)
    {
        id<MTLComputeCommandEncoder> encoder = [metal_buffer->commandBuffer computeCommandEncoder];
        metal_buffer->computeEncoder = [encoder retain];
        [encoder release];
    }
    if (metal_buffer->computePipeline != nullptr && metal_buffer->computePipeline->computeState != nil)
    {
        [metal_buffer->computeEncoder setComputePipelineState:metal_buffer->computePipeline->computeState];
    }

    MTLSize threads_per_group = MTLSizeMake(1, 1, 1);
    if (metal_buffer->computePipeline != nullptr)
    {
        threads_per_group = MTLSizeMake(metal_buffer->computePipeline->workgroupSize[0],
                                        metal_buffer->computePipeline->workgroupSize[1],
                                        metal_buffer->computePipeline->workgroupSize[2]);
    }
    [metal_buffer->computeEncoder
        dispatchThreadgroupsWithIndirectBuffer:metal_buffer_resource->getResource()
                         indirectBufferOffset:static_cast<NSUInteger>(offset)
                        threadsPerThreadgroup:threads_per_group];
#endif
}

void MetalRHI::CmdPipelineBarrier(RHICommandBuffer*,
                                  RHIPipelineStageFlags,
                                  RHIPipelineStageFlags,
                                  RHIDependencyFlags,
                                  uint32_t,
                                  const RHIMemoryBarrier*,
                                  uint32_t,
                                  const RHIBufferMemoryBarrier*,
                                  uint32_t,
                                  const RHIImageMemoryBarrier*)
{
    // Metal tracks hazards implicitly between encoders; no explicit barrier op.
}

bool MetalRHI::EndCommandBuffer(RHICommandBuffer* commandBuffer)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)commandBuffer;
    if (metal_buffer == nullptr)
    {
        return false;
    }
    if (metal_buffer->computeEncoder != nil)
    {
        [metal_buffer->computeEncoder endEncoding];
        [metal_buffer->computeEncoder release];
        metal_buffer->computeEncoder = nil;
    }
    return true;
#else
    (void)commandBuffer;
    return false;
#endif
}

void MetalRHI::UpdateDescriptorSets(uint32_t                     descriptorWriteCount,
                                    const RHIWriteDescriptorSet* pDescriptorWrites,
                                    uint32_t,
                                    const RHICopyDescriptorSet*)
{
    for (uint32_t i = 0; i < descriptorWriteCount; ++i)
    {
        const RHIWriteDescriptorSet& write = pDescriptorWrites[i];
        auto* set = (MetalDescriptorSet*)write.dstSet;
        if (set == nullptr)
        {
            continue;
        }

        for (auto& binding : set->bindings)
        {
            if (binding.binding != write.dstBinding)
            {
                continue;
            }
            binding.releaseResources();
            if (write.pBufferInfo != nullptr)
            {
                auto* buffer = (MetalBuffer*)write.pBufferInfo->buffer;
                binding.buffer = buffer != nullptr ? [buffer->getResource() retain] : nil;
                binding.offset = write.pBufferInfo->offset;
                binding.range  = write.pBufferInfo->range;
            }
            if (write.pImageInfo != nullptr)
            {
                auto* view = (MetalImageView*)write.pImageInfo->imageView;
                auto* sampler = (MetalSampler*)write.pImageInfo->sampler;
                binding.texture = view != nullptr ? [view->getResource() retain] : nil;
                binding.sampler = sampler != nullptr ? [sampler->getResource() retain] : nil;
            }
        }
    }
}

bool MetalRHI::QueueSubmit(RHIQueue* queue, uint32_t submitCount, const RHISubmitInfo* pSubmits, RHIFence* fence)
{
    (void)queue;
#ifdef __APPLE__
    for (uint32_t i = 0; i < submitCount; ++i)
    {
        for (uint32_t b = 0; b < pSubmits[i].commandBufferCount; ++b)
        {
            auto* metal_buffer = (MetalCommandBuffer*)pSubmits[i].pCommandBuffers[b];
            if (metal_buffer == nullptr || metal_buffer->commandBuffer == nil)
            {
                continue;
            }
            [metal_buffer->commandBuffer commit];
            if (fence != nullptr)
            {
                ((MetalFence*)fence)->setCommandBuffer(metal_buffer->commandBuffer);
            }
        }
    }
    return true;
#else
    (void)submitCount;
    (void)pSubmits;
    (void)fence;
    return false;
#endif
}

bool MetalRHI::QueueWaitIdle(RHIQueue*)
{
    // Nothing to wait on: submissions are committed synchronously in QueueSubmit().
    return true;
}

void MetalRHI::ResetCommandPool()
{
    for (uint32_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        if (m_CommandBuffers[i] != nullptr)
        {
            auto* metal_buffer = (MetalCommandBuffer*)m_CommandBuffers[i];
            [metal_buffer->commandBuffer release];
            metal_buffer->commandBuffer = nil;
        }
    }
}

void MetalRHI::WaitForFences()
{
    for (uint32_t i = 0; i < k_max_frames_in_flight; ++i)
    {
        RHIFence* fence = m_RhiIsFrameInFlightFences[i];
        if (fence != nullptr)
        {
            WaitForFencesPFN(1, &fence, RHI_TRUE, UINT64_MAX);
        }
    }
}

void MetalRHI::GetPhysicalDeviceProperties(RHIPhysicalDeviceProperties*) {}
RHICommandBuffer* MetalRHI::GetCurrentCommandBuffer() const { return m_CurrentCommandBuffer; }
RHICommandBuffer* const* MetalRHI::GetCommandBufferList() const { return m_CommandBuffers; }
RHICommandPool* MetalRHI::GetCommandPoor() const { return m_RhiCommandPool; }
RHIDescriptorPool* MetalRHI::GetDescriptorPoor() const { return m_DescriptorPool; }
RHIFence* const* MetalRHI::GetFenceList() const { return m_RhiIsFrameInFlightFences; }
QueueFamilyIndices MetalRHI::GetQueueFamilyIndices() const { return m_QueueIndices; }
RHIQueue* MetalRHI::GetGraphicsQueue() const { return m_GraphicsQueue; }
RHIQueue* MetalRHI::GetComputeQueue() const { return m_ComputeQueue; }
RHISwapChainDesc MetalRHI::GetSwapchainInfo()
{
    RHISwapChainDesc desc {};
    desc.extent         = m_SwapchainExtent;
    desc.image_format   = m_SwapchainImageFormat;
    desc.viewport       = m_Viewports;
    desc.viewport_count = static_cast<uint32_t>(m_ViewportCount);
    desc.scissor        = m_Scissors;
    desc.imageViews     = m_SwapchainImageviews;
    return desc;
}

RHIRenderPass* MetalRHI::GetSwapchainRenderPass()
{
    if (m_SwapchainRenderPass != nullptr)
    {
        return m_SwapchainRenderPass;
    }

    RHIAttachmentDescription attachment {};
    attachment.format  = m_SwapchainImageFormat != RHI_FORMAT_UNDEFINED ? m_SwapchainImageFormat
                                                                        : RHI_FORMAT_B8G8R8A8_UNORM;
    attachment.samples = RHI_SAMPLE_COUNT_1_BIT;
    attachment.loadOp  = RHI_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = RHI_ATTACHMENT_STORE_OP_STORE;

    RHIRenderPassCreateInfo create_info {};
    create_info.sType           = RHI_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 1;
    create_info.pAttachments    = &attachment;
    create_info.subpassCount    = 0;  // Metal has no subpasses.

    CreateRenderPass(&create_info, m_SwapchainRenderPass);
    return m_SwapchainRenderPass;
}

RHIFramebuffer* MetalRHI::GetSwapchainFramebuffer()
{
    const bool stale = m_SwapchainFramebuffer != nullptr && m_SwapchainExtent.width > 0 &&
                       m_SwapchainExtent.height > 0 &&
                       (m_SwapchainExtent.width != ((MetalFramebuffer*)m_SwapchainFramebuffer)->width ||
                        m_SwapchainExtent.height != ((MetalFramebuffer*)m_SwapchainFramebuffer)->height);
    if (m_SwapchainFramebuffer != nullptr && !stale)
    {
        return m_SwapchainFramebuffer;
    }
    if (m_SwapchainFramebuffer != nullptr)
    {
        delete (MetalFramebuffer*)m_SwapchainFramebuffer;
        m_SwapchainFramebuffer = nullptr;
    }
    if (m_SwapchainExtent.width == 0 || m_SwapchainExtent.height == 0)
    {
        return nullptr;
    }

    // A null attachment means "this frame's drawable" (see CreateFramebuffer).
    RHIImageView* attachments[1] = {nullptr};
    RHIFramebufferCreateInfo create_info {};
    create_info.sType           = RHI_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    create_info.renderPass      = GetSwapchainRenderPass();
    create_info.attachmentCount = 1;
    create_info.pAttachments    = attachments;
    create_info.width           = m_SwapchainExtent.width;
    create_info.height          = m_SwapchainExtent.height;
    create_info.layers          = 1;

    CreateFramebuffer(&create_info, m_SwapchainFramebuffer);
    return m_SwapchainFramebuffer;
}

RHIDepthImageDesc MetalRHI::GetDepthImageInfo() const
{
    RHIDepthImageDesc desc {};
    desc.depth_image_format = m_DepthImageFormat;
    desc.depth_image_view   = m_DepthImageView;
    return desc;
}
uint8_t MetalRHI::GetMaxFramesInFlight() const { return k_max_frames_in_flight; }
uint8_t MetalRHI::GetCurrentFrameIndex() const { return m_CurrentFrameIndex; }
void MetalRHI::SetCurrentFrameIndex(uint8_t index) { m_CurrentFrameIndex = index % k_max_frames_in_flight; }

RHICommandBuffer* MetalRHI::BeginSingleTimeCommands()
{
#ifdef __APPLE__
    if (m_CommandQueue == nil)
    {
        return nullptr;
    }
    auto* metal_buffer = new MetalCommandBuffer();
    id<MTLCommandBuffer> command_buffer = [m_CommandQueue commandBuffer];
    metal_buffer->commandBuffer = [command_buffer retain];
    [command_buffer release];
    return metal_buffer;
#else
    return nullptr;
#endif
}

void MetalRHI::EndSingleTimeCommands(RHICommandBuffer* command_buffer)
{
#ifdef __APPLE__
    auto* metal_buffer = (MetalCommandBuffer*)command_buffer;
    if (metal_buffer == nullptr)
    {
        return;
    }
    // renderEncoder 也要关：一个只做 CmdCopyBuffer 的单次命令缓冲不会开它，但一个
    // 顺手画了点什么（或者 pass 没配对 CmdEndRenderPass）的会 —— 留着它 commit 会
    // 直接失败，命令全部丢失。
    if (metal_buffer->renderEncoder != nil)
    {
        [metal_buffer->renderEncoder endEncoding];
    }
    if (metal_buffer->computeEncoder != nil)
    {
        [metal_buffer->computeEncoder endEncoding];
    }
    [metal_buffer->commandBuffer commit];
    // **"单次命令"的契约是同步的**：Vulkan 侧是 vkQueueSubmit + vkQueueWaitIdle，
    // DX12 侧是 ExecuteCommandLists + Signal + WaitForFences。这里以前只 commit
    // 就返回，于是 PickPass 那种"End 之后立刻 MapMemory 读回"的用法读到的是还没
    // 跑完（甚至还没开始跑）的缓冲。Nanite 的 GPU 剔除也是同一个用法。
    [metal_buffer->commandBuffer waitUntilCompleted];
    delete metal_buffer;
#endif
}
// Acquires this frame's drawable. Returns true when the frame must be skipped
// (no window yet, zero-sized window, or no drawable available).
bool MetalRHI::PrepareBeforePass(std::function<void()> passUpdateAfterRecreateSwapchain)
{
#ifdef __APPLE__
    if (m_MetalLayer == nil)
    {
        CreateSwapchain();
    }
    if (m_MetalLayer == nil)
    {
        return true;
    }

    NSView* view = m_SwapchainView != nullptr ? (__bridge NSView*)m_SwapchainView : nil;
    if (view != nil)
    {
        const NSRect backing = [view convertRectToBacking:view.bounds];
        const bool resized = backing.size.width <= 0.0 || backing.size.height <= 0.0 ||
                             std::fabs(backing.size.width - static_cast<double>(m_SwapchainExtent.width)) > 0.5 ||
                             std::fabs(backing.size.height - static_cast<double>(m_SwapchainExtent.height)) > 0.5;
        if (resized)
        {
            if (!SyncLayerMetrics(m_MetalLayer, view, m_SwapchainExtent, m_Viewports, m_Scissors))
            {
                return true;  // minimised / zero-sized window
            }
            if (passUpdateAfterRecreateSwapchain)
            {
                passUpdateAfterRecreateSwapchain();
            }
        }
    }

    dispatch_semaphore_t semaphore = m_Semaphores[m_CurrentFrameIndex];
    if (semaphore != nullptr)
    {
        dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
    }

    id<CAMetalDrawable> drawable = [m_MetalLayer nextDrawable];
    if (drawable == nil)
    {
        if (semaphore != nullptr)
        {
            dispatch_semaphore_signal(semaphore);
        }
        return true;
    }
    m_CurrentDrawable = [drawable retain];

    // The frame's command buffer: everything recorded between PrepareBeforePass()
    // and SubmitRendering() (ZSlate overlay, copies, ...) lands here.
    if (m_CommandBuffers[m_CurrentFrameIndex] == nullptr)
    {
        m_CommandBuffers[m_CurrentFrameIndex] = new MetalCommandBuffer();
    }
    auto* metal_buffer = (MetalCommandBuffer*)m_CommandBuffers[m_CurrentFrameIndex];
    if (metal_buffer->commandBuffer != nil)
    {
        // Last frame's buffer was never submitted (skipped frame): drop it.
        [metal_buffer->commandBuffer release];
        metal_buffer->commandBuffer = nil;
    }
    id<MTLCommandBuffer> command_buffer = [m_CommandQueue commandBuffer];
    metal_buffer->commandBuffer = command_buffer != nil ? [command_buffer retain] : nil;
    if (command_buffer != nil)
    {
        [command_buffer release];
    }
    metal_buffer->renderEncoder  = nil;
    metal_buffer->computeEncoder = nil;
    m_CurrentCommandBuffer       = metal_buffer;
    return metal_buffer->commandBuffer == nil;
#else
    if (passUpdateAfterRecreateSwapchain)
    {
        passUpdateAfterRecreateSwapchain();
    }
    return true;
#endif
}

// Ends any encoder still open for the frame, presents the drawable and commits.
// Drawing happens between PrepareBeforePass() and this call, through the RHI
// Cmd* encoders (see RenderSystem::Tick on Apple).
void MetalRHI::SubmitRendering(std::function<void()> passUpdateAfterRecreateSwapchain)
{
#ifdef __APPLE__
    dispatch_semaphore_t semaphore = m_Semaphores[m_CurrentFrameIndex];
    auto* metal_buffer = (MetalCommandBuffer*)m_CurrentCommandBuffer;
    if (m_CurrentDrawable == nil || metal_buffer == nullptr || metal_buffer->commandBuffer == nil)
    {
        if (metal_buffer != nullptr)
        {
            [metal_buffer->commandBuffer release];
            metal_buffer->commandBuffer = nil;
        }
        m_CurrentCommandBuffer = nullptr;
        if (m_CurrentDrawable != nil)
        {
            [m_CurrentDrawable release];
            m_CurrentDrawable = nil;
        }
        if (semaphore != nullptr)
        {
            dispatch_semaphore_signal(semaphore);
        }
        if (passUpdateAfterRecreateSwapchain)
        {
            passUpdateAfterRecreateSwapchain();
        }
        return;
    }

    id<MTLCommandBuffer> command_buffer = metal_buffer->commandBuffer;

    // A pass left its encoder open (no CmdEndRenderPass): close it so the
    // command buffer can still be committed.
    if (metal_buffer->renderEncoder != nil)
    {
        [metal_buffer->renderEncoder endEncoding];
        [metal_buffer->renderEncoder release];
        metal_buffer->renderEncoder = nil;
    }
    if (metal_buffer->computeEncoder != nil)
    {
        [metal_buffer->computeEncoder endEncoding];
        [metal_buffer->computeEncoder release];
        metal_buffer->computeEncoder = nil;
    }

    [command_buffer presentDrawable:m_CurrentDrawable];
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        if (semaphore != nullptr)
        {
            dispatch_semaphore_signal(semaphore);
        }
    }];
    [command_buffer commit];

    metal_buffer->commandBuffer = nil;
    [command_buffer release];
    m_CurrentCommandBuffer = nullptr;
    [m_CurrentDrawable release];
    m_CurrentDrawable = nil;
    m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % k_max_frames_in_flight;
#else
    if (passUpdateAfterRecreateSwapchain)
    {
        passUpdateAfterRecreateSwapchain();
    }
#endif
}
void MetalRHI::PushEvent(RHICommandBuffer*, const char*, const float*) {}
void MetalRHI::PopEvent(RHICommandBuffer*) {}

void MetalRHI::clear()
{
    // Tear down bindless manager before general cleanup.
    if (m_BindlessTextureManager)
    {
        m_BindlessTextureManager->Shutdown();
        delete m_BindlessTextureManager;
        m_BindlessTextureManager = nullptr;
    }
    m_BindlessSupported           = false;
    m_MaxBindlessSampledImages  = 0;
    m_MaxBindlessStorageBuffers = 0;
}
void MetalRHI::ClearSwapchain()
{
#ifdef __APPLE__
    if (m_CurrentDrawable != nil)
    {
        [m_CurrentDrawable release];
        m_CurrentDrawable = nil;
    }
    NSView* view = m_SwapchainView != nullptr ? (__bridge NSView*)m_SwapchainView : nil;
    if (view != nil && view.layer == m_MetalLayer)
    {
        view.layer = nil;
    }
    if (m_MetalLayer != nil)
    {
        [m_MetalLayer release];
        m_MetalLayer = nil;
    }
    m_SwapchainView  = nullptr;
    m_SwapchainExtent = {0, 0};
#endif
}
void MetalRHI::DestroyDefaultSampler(RHIDefaultSamplerType) {}
void MetalRHI::DestroyMipmappedSampler() {}
void MetalRHI::DestroyShaderModule(RHIShader*) {}
void MetalRHI::DestroySemaphore(RHISemaphore*) {}
void MetalRHI::DestroySampler(RHISampler*) {}
void MetalRHI::DestroyInstance(RHIInstance*) {}
void MetalRHI::DestroyImageView(RHIImageView*) {}
void MetalRHI::DestroyImage(RHIImage*) {}
void MetalRHI::DestroyFramebuffer(RHIFramebuffer*) {}
void MetalRHI::DestroyFence(RHIFence*) {}
void MetalRHI::DestroyDevice() {}
void MetalRHI::DestroyCommandPool(RHICommandPool*) {}
void MetalRHI::DestroyBuffer(RHIBuffer*& buffer)
{
    delete (MetalBuffer*)buffer;
    buffer = nullptr;
}

void MetalRHI::FreeCommandBuffers(RHICommandPool*, uint32_t, RHICommandBuffer*) {}

void MetalRHI::FreeMemory(RHIDeviceMemory*& memory)
{
    delete (MetalDeviceMemory*)memory;
    memory = nullptr;
}

bool MetalRHI::MapMemory(RHIDeviceMemory*   memory,
                         RHIDeviceSize      offset,
                         RHIDeviceSize,
                         RHIMemoryMapFlags,
                         void**             ppData)
{
    if (ppData == nullptr)
    {
        return false;
    }
    *ppData = nullptr;
    auto* metal_memory = (MetalDeviceMemory*)memory;
    if (metal_memory == nullptr)
    {
        return false;
    }
    void* base = metal_memory->getHostPtr();
    if (base == nullptr && metal_memory->getBuffer() != nil)
    {
        base = metal_memory->getBuffer().contents;
    }
    if (base == nullptr)
    {
        return false;
    }
    *ppData = static_cast<uint8_t*>(base) + offset;
    return true;
}
void MetalRHI::UnmapMemory(RHIDeviceMemory*) {}
void MetalRHI::InvalidateMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize) {}
void MetalRHI::FlushMappedMemoryRanges(void*, RHIDeviceMemory*, RHIDeviceSize, RHIDeviceSize) {}

RHISemaphore*& MetalRHI::GetTextureCopySemaphore(uint32_t)
{
    static RHISemaphore* null_semaphore = nullptr;
    return null_semaphore;
}

void MetalRHI::RegisterViewport(const int viewport_id, const RHIViewport& viewport)
{
    if (viewport_id >= 0 && viewport_id < m_ViewportCount)
    {
        m_Viewports[viewport_id] = viewport;
    }
}

void MetalRHI::UpdateViewport(ViewportType viewport_id, const RHIViewport& viewport)
{
    const int index = static_cast<int>(viewport_id);
    if (index >= 0 && index < m_ViewportCount)
    {
        m_Viewports[index] = viewport;
    }
}

RHIViewport* MetalRHI::GetViewport(ViewportType viewport_id)
{
    const int index = static_cast<int>(viewport_id);
    if (index < 0 || index >= m_ViewportCount)
    {
        return nullptr;
    }
    return &m_Viewports[index];
}

void MetalRHI::CreateViewportRenderTexture(const std::string&, uint32_t, uint32_t) {}
void MetalRHI::UpdateViewportRenderTexture(const std::string&, uint32_t, uint32_t) {}
void MetalRHI::DestroyViewportRenderTexture(const std::string&) {}
RHI::ViewportRenderTexture* MetalRHI::GetViewportRenderTexture(const std::string& viewport_id)
{
    auto it = m_ViewportRenderTextures.find(viewport_id);
    return it == m_ViewportRenderTextures.end() ? nullptr : &it->second;
}

// =====================================================================
// Bindless overrides
// =====================================================================

RHIBindlessTextureManager* MetalRHI::getBindlessTextureManager()
{
    return m_BindlessTextureManager;
}

bool MetalRHI::supportsBindlessTextures() const
{
    return m_BindlessSupported;
}

uint32_t MetalRHI::maxBindlessSampledImages() const
{
    return m_MaxBindlessSampledImages;
}

uint32_t MetalRHI::maxBindlessStorageBuffers() const
{
    return m_MaxBindlessStorageBuffers;
}

void MetalRHI::CmdSetBindlessIndexPFN(RHICommandBuffer*    /*commandBuffer*/,
                                      RHIPipelineBindPoint /*pipelineBindPoint*/,
                                      RHIPipelineLayout*   /*layout*/,
                                      uint32_t             packed_index)
{
#ifdef __APPLE__
    if (!m_BindlessSupported || m_BindlessTextureManager == nullptr)
    {
        return;
    }

    // Push the packed 32-bit bindless index through
    // setVertexBytes / setFragmentBytes on the current frame's
    // render command encoder. This mirrors Vulkan's push-constant
    // and DX12's root-constant delivery.
    //
    // The 4-byte payload is: low 16 bits = texture index,
    // high 16 bits = sampler index (see BindlessIndex::Pack).
    const uint32_t frame = m_CurrentFrameIndex;
    if (frame >= k_max_frames_in_flight)
    {
        return;
    }

    id<MTLRenderCommandEncoder> encoder = m_RenderEncoders[frame];
    if (encoder == nil)
    {
        return;
    }

    // Bind the argument buffer at the canonical slot (0) for both
    // vertex and fragment stages, then push the index constant at
    // slot 1.
    const NSUInteger buffer_index = MetalBindlessTextureManager::kBindlessBufferIndex;
    const NSUInteger index_buffer_index = MetalBindlessTextureManager::kBindlessIndexBufferIndex;

    [encoder setVertexBuffer:m_BindlessTextureManager->getArgumentBuffer()
                      offset:0
                     atIndex:buffer_index];
    [encoder setFragmentBuffer:m_BindlessTextureManager->getArgumentBuffer()
                        offset:0
                       atIndex:buffer_index];

    [encoder setVertexBytes:&packed_index
                     length:sizeof(packed_index)
                    atIndex:index_buffer_index];
    [encoder setFragmentBytes:&packed_index
                       length:sizeof(packed_index)
                      atIndex:index_buffer_index];

    // Ensure residency of all allocated textures before any draw.
    m_BindlessTextureManager->ensureResidency(encoder);
#endif
}
