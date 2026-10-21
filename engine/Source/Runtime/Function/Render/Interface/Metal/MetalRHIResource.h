#pragma once

// =====================================================================
// Metal RHI resource wrapper types
// ---------------------------------------------------------------------
// Thin wrappers around Metal Objective-C types that inherit from the
// cross-backend RHI base classes (RHIImage, RHIImageView, RHISampler,
// etc.). The bindless manager and the rest of the Metal backend
// downcast RHI* pointers to these concrete types to extract the
// underlying id<MTLTexture> / id<MTLSamplerState> / id<MTLBuffer>.
//
// Pattern mirrors vulkan_rhi_resource.h and dx12_rhi_resource.h.
// =====================================================================

#ifdef __APPLE__
    #define Component AppleComponent
    #include <Metal/Metal.h>
    #undef Component
#endif

#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <string>
#include <vector>

// ---- Image ----

// NOTE: the engine is built without ARC, so the wrappers own their Metal
// object: setResource() retains, the destructor releases.

class MetalImage : public RHIImage
{
public:
    ~MetalImage() { [m_Resource release]; }

    void setResource(id<MTLTexture> tex) { m_Resource = [tex retain]; }
    id<MTLTexture> getResource() const { return m_Resource; }

private:
    id<MTLTexture> m_Resource = nil;
};

// ---- Image view ----

class MetalImageView : public RHIImageView
{
public:
    ~MetalImageView() { [m_Resource release]; }

    void setResource(id<MTLTexture> tex) { m_Resource = [tex retain]; }
    id<MTLTexture> getResource() const { return m_Resource; }

private:
    id<MTLTexture> m_Resource = nil;
};

// ---- Sampler ----

class MetalSampler : public RHISampler
{
public:
    ~MetalSampler() { [m_Resource release]; }

    void setResource(id<MTLSamplerState> samp) { m_Resource = [samp retain]; }
    id<MTLSamplerState> getResource() const { return m_Resource; }

private:
    id<MTLSamplerState> m_Resource = nil;
};

// ---- Buffer ----

class MetalBuffer : public RHIBuffer
{
public:
    ~MetalBuffer() { [m_Resource release]; }

    void setResource(id<MTLBuffer> buf) { m_Resource = [buf retain]; }
    id<MTLBuffer> getResource() const { return m_Resource; }

private:
    id<MTLBuffer> m_Resource = nil;
};

// ---- Device memory ----
// Metal has unified memory: allocations are either a MTLBuffer (device visible,
// map-able through its contents) or a host-side malloc used for staging. The
// engine still hands us an RHIDeviceMemory to bind/unmap, so keep both.

class MetalDeviceMemory : public RHIDeviceMemory
{
public:
    ~MetalDeviceMemory()
    {
        // Releases our own reference to the MTLBuffer. m_HostPtr is NOT freed
        // here: for a host-visible allocation it is the buffer's `contents`
        // pointer, i.e. memory owned by the MTLBuffer, not by malloc.
        [m_Buffer release];
    }

    // Borrowed pointer: either nullptr or the MTLBuffer's `contents` (valid for
    // as long as this object keeps its reference to the buffer).
    void* getHostPtr() const { return m_HostPtr; }
    void  setHostPtr(void* p) { m_HostPtr = p; }

    void  setBuffer(id<MTLBuffer> buffer) { m_Buffer = [buffer retain]; }
    id<MTLBuffer> getBuffer() const { return m_Buffer; }

private:
    void*        m_HostPtr = nullptr;
    id<MTLBuffer> m_Buffer  = nil;
};

// ---- Shader ----

class MetalShader : public RHIShader
{
public:
    ~MetalShader()
    {
        [m_Function release];
        [m_Library release];
    }

    void          setFunction(id<MTLFunction> f) { m_Function = [f retain]; }
    id<MTLFunction> getFunction() const { return m_Function; }

    void          setLibrary(id<MTLLibrary> l) { m_Library = [l retain]; }
    id<MTLLibrary> getLibrary() const { return m_Library; }

    uint32_t    stage {0};
    std::string entryPoint;

    // compute kernel 声明的 workgroup 尺寸（GLSL 的 `layout(local_size_x/y/z)`，
    // MSL 的 `[[threads_per_threadgroup(x, y, z)]]`）。
    //
    // Metal 与 Vulkan 不同：`dispatchThreadgroups` 必须在 CPU 侧给出
    // threadsPerThreadgroup，Metal 不会从 shader 里读它。以前这里硬编码 (1,1,1)，
    // 于是 `local_size_x = 256` 的 kernel 在 Metal 上只有一个线程在跑 —— 剔除结果
    // 恒等于"只处理了第 0 个元素"，而且和 Vulkan 的结果不一致。
    uint32_t workgroupSize[3] {1, 1, 1};

private:
    id<MTLFunction> m_Function = nil;
    id<MTLLibrary>  m_Library  = nil;
};

// ---- Descriptor set layout / descriptor set ----

struct MetalDescriptorBinding
{
    uint32_t          binding {0};
    RHIDescriptorType type {};
    uint32_t          stageFlags {0};

    // Buffer descriptor
    id<MTLBuffer>     buffer {nil};
    RHIDeviceSize     offset {0};
    RHIDeviceSize     range {0};

    // Image / sampler descriptor
    id<MTLTexture>      texture {nil};
    id<MTLSamplerState> sampler {nil};

    void releaseResources()
    {
        [buffer release];
        [texture release];
        [sampler release];
        buffer  = nil;
        texture = nil;
        sampler = nil;
    }
};

class MetalDescriptorSetLayout : public RHIDescriptorSetLayout
{
public:
    std::vector<MetalDescriptorBinding> bindings;
};

class MetalDescriptorPool : public RHIDescriptorPool
{
};

class MetalDescriptorSet : public RHIDescriptorSet
{
public:
    ~MetalDescriptorSet()
    {
        for (auto& binding : bindings)
        {
            binding.releaseResources();
        }
    }

    std::vector<MetalDescriptorBinding> bindings;
    MetalDescriptorSetLayout*           layout {nullptr};
};

// ---- Pipeline layout / pipeline ----

class MetalPipelineLayout : public RHIPipelineLayout
{
public:
    std::vector<MetalDescriptorSetLayout*> setLayouts;
};

class MetalPipeline : public RHIPipeline
{
public:
    ~MetalPipeline()
    {
        [renderState release];
        [computeState release];
        [depthState release];
    }

    id<MTLRenderPipelineState>  renderState {nil};
    id<MTLComputePipelineState> computeState {nil};
    id<MTLDepthStencilState>    depthState {nil};

    MTLPrimitiveType primitiveType {MTLPrimitiveTypeTriangle};
    MTLCullMode      cullMode {MTLCullModeNone};
    MTLWinding       winding {MTLWindingCounterClockwise};

    // compute 管线建好时把 kernel 的 workgroup 尺寸抄一份过来：CmdDispatch 拿得到
    // 的只有 pipeline，拿不到 shader。
    uint32_t workgroupSize[3] {1, 1, 1};
};

// ---- Render pass / framebuffer ----

struct MetalAttachmentDescription
{
    RHIFormat            format {RHI_FORMAT_UNDEFINED};
    RHIAttachmentLoadOp  loadOp {};
    RHIAttachmentStoreOp storeOp {};
    bool                 isDepthStencil {false};
};

class MetalRenderPass : public RHIRenderPass
{
public:
    std::vector<MetalAttachmentDescription> attachments;
};

class MetalFramebuffer : public RHIFramebuffer
{
public:
    ~MetalFramebuffer()
    {
        for (id<MTLTexture> texture : textures)
        {
            [texture release];
        }
    }

    std::vector<id<MTLTexture>> textures;
    uint32_t                    width {0};
    uint32_t                    height {0};
    // true when attachment i is the swapchain drawable (resolved at begin-pass).
    std::vector<bool>           isSwapchain;
};

// ---- Command pool / command buffer ----

class MetalCommandPool : public RHICommandPool
{
};

class MetalCommandBuffer : public RHICommandBuffer
{
public:
    ~MetalCommandBuffer()
    {
        [commandBuffer release];
        [renderEncoder release];
        [computeEncoder release];
        [blitEncoder release];
        [indexBuffer release];
    }

    id<MTLCommandBuffer>        commandBuffer {nil};
    id<MTLRenderCommandEncoder> renderEncoder {nil};
    id<MTLComputeCommandEncoder> computeEncoder {nil};
    id<MTLBlitCommandEncoder>   blitEncoder {nil};

    // Index buffer state (Metal binds it at draw time, not bind time).
    id<MTLBuffer>   indexBuffer {nil};
    RHIDeviceSize   indexBufferOffset {0};
    MTLIndexType    indexType {MTLIndexTypeUInt32};

    MetalPipeline*  graphicsPipeline {nullptr};
    MetalPipeline*  computePipeline {nullptr};
};

// ---- Fence / semaphore / queue ----

class MetalFence : public RHIFence
{
public:
    ~MetalFence() { [commandBuffer release]; }

    void           setCommandBuffer(id<MTLCommandBuffer> cb) { commandBuffer = [cb retain]; }
    id<MTLCommandBuffer> getCommandBuffer() const { return commandBuffer; }

    bool signaled {false};

private:
    id<MTLCommandBuffer> commandBuffer {nil};
};

class MetalSemaphore : public RHISemaphore
{
};

class MetalQueue : public RHIQueue
{
};
