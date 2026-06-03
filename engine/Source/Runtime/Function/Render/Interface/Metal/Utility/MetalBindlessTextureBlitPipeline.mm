// =====================================================================
// MetalBindlessTextureBlitPipeline implementation
// ---------------------------------------------------------------------
// Metal sibling of VulkanBindlessTextureBlitPipeline and
// BindlessTextureBlitPipeline (DX12). Uses the Metal Tier2
// argument_buffer + texture2d<...> array path for bindless texture
// access.
//
// Key implementation notes:
//
//   1. MSL source is inlined (same strategy as the Vulkan sibling).
//      The on-disk .metal files at metal/utility/shaders/ are the
//      human-edited source of truth and MUST be kept in sync with
//      the literals below.
//
//   2. BINDLESS_CAPACITY is injected as a preprocessor macro via
//      MTLCompileOptions. The shader's array<texture2d<...>, N> must
//      have N >= the runtime capacity, otherwise indexing beyond N is
//      undefined behavior per the MSL spec.
//
//   3. Metal shader compilation uses [device newLibraryWithSource:],
//      not the cross-backend RHI's CreateShaderModuleFromSource (which
//      is currently a stub for Metal).
//
//   4. RecordBlit() binds the argument buffer directly on the encoder
//      and calls ensureResidency() before the draw. This is the
//      Metal-specific step that Vulkan/DX12 don't need (Vulkan handles
//      residency implicitly; DX12 uses the shader-visible heap).
// =====================================================================

#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Interface/Metal/Utility/MetalBindlessTextureBlitPipeline.h"
#include "Runtime/Function/Render/Interface/Metal/MetalRHI.h"
#include "Runtime/Function/Render/Interface/Metal/MetalBindlessTextureManager.h"

#include <sstream>

namespace
{
    // =================================================================
    // Inline MSL source for the bindless blit vertex + fragment shader.
    // -----------------------------------------------------------------
    // MUST stay in sync with the on-disk reference file:
    //   metal/utility/shaders/bindless_blit.metal
    //
    // The BINDLESS_CAPACITY macro is NOT defined here -- it is injected
    // via MTLCompileOptions.preprocessorMacros at compile time so the
    // shader's array size matches the runtime capacity exactly.
    // =================================================================
    constexpr const char* k_bindless_blit_msl = R"msl(
#include <metal_stdlib>
using namespace metal;

struct BindlessTable
{
    array<texture2d<float, access::sample>, BINDLESS_CAPACITY> textures [[id(0)]];
    array<sampler, 4>                                          samplers [[id(BINDLESS_CAPACITY)]];
};

struct BlitVSOutput
{
    float4 position [[position]];
    float2 uv;
};

vertex BlitVSOutput bindless_blit_vert(uint vid [[vertex_id]])
{
    BlitVSOutput out;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    out.uv       = uv;
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return out;
}

fragment float4 bindless_blit_frag(BlitVSOutput          in       [[stage_in]],
                                   device BindlessTable& bindless [[buffer(0)]],
                                   constant uint&        packed_index [[buffer(1)]])
{
    const uint tex_idx  = packed_index & 0xFFFFu;
    const uint samp_idx = (packed_index >> 16u) & 0xFFFFu;
    return bindless.textures[tex_idx].sample(bindless.samplers[samp_idx], in.uv);
}
)msl";

} // namespace

// =====================================================================
// initialize
// =====================================================================
bool MetalBindlessTextureBlitPipeline::Initialize(MetalRHI* metal_rhi)
{
    if (m_Ready)
    {
        return true;
    }
    if (metal_rhi == nullptr)
    {
        LOG_ERROR(ZRender, "MetalBindlessTextureBlitPipeline::initialize: metal_rhi is null");
        return false;
    }
    if (!metal_rhi->supportsBindlessTextures())
    {
        LOG_WARNING(ZRender,
                    "MetalBindlessTextureBlitPipeline: MetalRHI does not support bindless "
                    "(Argument Buffer Tier1 only). Initialization skipped.");
        return false;
    }

    RHIBindlessTextureManager* mgr = metal_rhi->getBindlessTextureManager();
    if (mgr == nullptr)
    {
        LOG_ERROR(ZRender,
                  "MetalBindlessTextureBlitPipeline: supportsBindlessTextures() returned true "
                  "but getBindlessTextureManager() returned null -- inconsistent RHI state.");
        return false;
    }

    m_Rhi = metal_rhi;

    // We need the Metal device and the bindless capacity for shader
    // compilation (BINDLESS_CAPACITY preprocessor define).
    MetalBindlessTextureManager* metal_mgr =
        static_cast<MetalBindlessTextureManager*>(mgr);
    id<MTLDevice> device = metal_mgr->getArgumentBuffer() != nil
                               ? [metal_mgr->getArgumentBuffer() device]
                               : nil;
    // The argument buffer doesn't expose its device directly.
    // Grab it from the MetalRHI instead.
    // (MetalRHI stores m_Device as a private member; we access it
    // through the bindless manager's stored device reference.)
    // Since the manager doesn't expose m_Device, we use the
    // underlying device of any Metal object we can get. The argument
    // buffer's device is the right one.
    if (metal_mgr->getArgumentBuffer() != nil)
    {
        device = [metal_mgr->getArgumentBuffer() device];
    }

    if (device == nil)
    {
        LOG_ERROR(ZRender,
                  "MetalBindlessTextureBlitPipeline: could not obtain MTLDevice from "
                  "bindless manager. Initialization failed.");
        m_Rhi = nullptr;
        return false;
    }

    const uint32_t capacity = mgr->capacity();

    if (!compileShader(device, capacity))
    {
        m_Rhi = nullptr;
        return false;
    }

    if (!createPipelineState(device))
    {
#if !__has_feature(objc_arc)
        [m_Library release];
#endif
        m_Library = nil;
        m_Rhi     = nullptr;
        return false;
    }

    m_Ready = true;
    LOG_INFO(ZRender,
             "MetalBindlessTextureBlitPipeline initialized "
             "(pipeline_state={}, capacity={}, Tier2 argument buffer)",
             static_cast<void*>(m_PipelineState),
             capacity);
    return true;
}

// =====================================================================
// shutdown
// =====================================================================
void MetalBindlessTextureBlitPipeline::Shutdown()
{
#if !__has_feature(objc_arc)
    [m_PipelineState release];
    [m_Library release];
#endif
    m_PipelineState = nil;
    m_Library        = nil;
    m_Rhi            = nullptr;
    m_Ready          = false;
}

// =====================================================================
// recordBlit
// =====================================================================
void MetalBindlessTextureBlitPipeline::RecordBlit(id<MTLRenderCommandEncoder> encoder,
                                                   uint32_t                    viewport_width,
                                                   uint32_t                    viewport_height,
                                                   uint32_t                    bindless_texture_index,
                                                   uint32_t                    sampler_index) const
{
    if (!m_Ready || encoder == nil)
    {
        return;
    }
    if (bindless_texture_index == RHIBindlessTextureManager::kInvalidBindlessIndex)
    {
        return;
    }
    if (viewport_width == 0 || viewport_height == 0)
    {
        return;
    }

    MetalBindlessTextureManager* mgr =
        static_cast<MetalBindlessTextureManager*>(m_Rhi->getBindlessTextureManager());
    if (mgr == nullptr)
    {
        return;
    }

    // 1. Bind pipeline state.
    [encoder setRenderPipelineState:m_PipelineState];

    // 2. Set viewport.
    MTLViewport viewport = {0.0, 0.0,
                            static_cast<double>(viewport_width),
                            static_cast<double>(viewport_height),
                            0.0, 1.0};
    [encoder setViewport:viewport];

    // 3. Bind the argument buffer at [[buffer(0)]] for both stages.
    const NSUInteger arg_buffer_index = MetalBindlessTextureManager::kBindlessBufferIndex;
    id<MTLBuffer> arg_buffer = mgr->getArgumentBuffer();
    [encoder setVertexBuffer:arg_buffer offset:0 atIndex:arg_buffer_index];
    [encoder setFragmentBuffer:arg_buffer offset:0 atIndex:arg_buffer_index];

    // 4. Push the packed bindless index at [[buffer(1)]] for both
    //    stages. This mirrors Vulkan push-constants and DX12 root
    //    constants.
    const uint32_t packed = BindlessIndex::Pack(bindless_texture_index, sampler_index);
    const NSUInteger index_buffer_index = MetalBindlessTextureManager::kBindlessIndexBufferIndex;
    [encoder setVertexBytes:&packed
                     length:sizeof(packed)
                    atIndex:index_buffer_index];
    [encoder setFragmentBytes:&packed
                       length:sizeof(packed)
                      atIndex:index_buffer_index];

    // 5. Ensure residency of all allocated textures before any draw.
    //    Metal requires explicit useResource / useResources calls for
    //    resources accessed through argument buffers.
    mgr->ensureResidency(encoder);

    // 6. Draw fullscreen triangle: 3 vertices, no index buffer.
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
}

// =====================================================================
// compileShader
// =====================================================================
bool MetalBindlessTextureBlitPipeline::compileShader(id<MTLDevice> device,
                                                      uint32_t      capacity)
{
    // Inject BINDLESS_CAPACITY as a preprocessor macro so the MSL
    // array<texture2d<...>, N> size matches the runtime capacity.
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    options.languageVersion = MTLLanguageVersion2_0; // Required for argument buffers

    // Build preprocessor macros dictionary.
    // MSL preprocessor macros are passed as NSDictionary<NSString*, NSString*>.
    NSMutableDictionary<NSString*, NSString*>* macros =
        [[NSMutableDictionary alloc] init];
    macros[@"BINDLESS_CAPACITY"] =
        [NSString stringWithFormat:@"%u", capacity];
    options.preprocessorMacros = macros;

    NSString* source =
        [NSString stringWithUTF8String:k_bindless_blit_msl];

    NSError* error = nil;
    m_Library = [device newLibraryWithSource:source
                                     options:options
                                       error:&error];

    [options release];
    [macros release];

    if (m_Library == nil || error != nil)
    {
        NSString* errMsg = error != nil ? [error localizedDescription] : @"unknown";
        LOG_ERROR(ZRender,
                  "MetalBindlessTextureBlitPipeline: failed to compile MSL bindless blit "
                  "shader (capacity={}): {}",
                  capacity,
                  [errMsg UTF8String]);
#if !__has_feature(objc_arc)
        if (m_Library != nil) { [m_Library release]; m_Library = nil; }
#endif
        m_Library = nil;
        return false;
    }

    return true;
}

// =====================================================================
// createPipelineState
// =====================================================================
bool MetalBindlessTextureBlitPipeline::createPipelineState(id<MTLDevice> device)
{
    // Vertex function.
    id<MTLFunction> vertex_fn =
        [m_Library newFunctionWithName:@"bindless_blit_vert"];
    if (vertex_fn == nil)
    {
        LOG_ERROR(ZRender,
                  "MetalBindlessTextureBlitPipeline: failed to find vertex function "
                  "'bindless_blit_vert' in compiled library");
        return false;
    }

    // Fragment function.
    id<MTLFunction> fragment_fn =
        [m_Library newFunctionWithName:@"bindless_blit_frag"];
    if (fragment_fn == nil)
    {
        LOG_ERROR(ZRender,
                  "MetalBindlessTextureBlitPipeline: failed to find fragment function "
                  "'bindless_blit_frag' in compiled library");
#if !__has_feature(objc_arc)
        [vertex_fn release];
#endif
        return false;
    }

    // Pipeline descriptor.
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction   = vertex_fn;
    desc.fragmentFunction = fragment_fn;

    // Color attachment 0: RGBA8Unorm, no blend.
    MTLRenderPipelineColorAttachmentDescriptor* color0 =
        desc.colorAttachments[0];
    color0.pixelFormat = MTLPixelFormatRGBA8Unorm;
    color0.blendingEnabled = NO;
    color0.writeMask = MTLColorWriteMaskAll;

    // No depth attachment.
    desc.depthAttachmentPixelFormat = MTLPixelFormatInvalid;

    NSError* error = nil;
    m_PipelineState = [device newRenderPipelineStateWithDescriptor:desc
                                                              error:&error];

#if !__has_feature(objc_arc)
    [vertex_fn release];
    [fragment_fn release];
    [desc release];
#endif

    if (m_PipelineState == nil || error != nil)
    {
        NSString* errMsg = error != nil ? [error localizedDescription] : @"unknown";
        LOG_ERROR(ZRender,
                  "MetalBindlessTextureBlitPipeline: newRenderPipelineStateWithDescriptor "
                  "failed: {}",
                  [errMsg UTF8String]);
        m_PipelineState = nil;
        return false;
    }

    return true;
}
