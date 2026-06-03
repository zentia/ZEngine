// =====================================================================
// MetalBindlessTextureManager implementation
// =====================================================================

#include "CommonPCH/pch.h"
#include "Runtime/Function/Render/Interface/Metal/MetalBindlessTextureManager.h"
#include "Runtime/Function/Render/Interface/Metal/MetalRHIResource.h"

#include <algorithm>
#include <cstring>

// =====================================================================
// initialize
// ---------------------------------------------------------------------
// 1. Check Argument Buffer Tier2 support.
// 2. Create MTLArgumentDescriptor array for textures + samplers.
// 3. Create argument encoder + backing MTLBuffer.
// 4. Create and encode 4 static samplers.
// 5. Create and upload 1x1 white placeholder into slot 0.
// =====================================================================
bool MetalBindlessTextureManager::Initialize(id<MTLDevice> device, uint32_t capacity)
{
    if (device == nil || capacity == 0)
    {
        LOG_WARNING(ZRender,
                    "MetalBindlessTextureManager::Initialize called with invalid args "
                    "(device={}, capacity={})",
                    static_cast<void*>(device),
                    capacity);
        return false;
    }

    // ---- 1. Check Argument Buffer Tier ----
    MTLArgumentBuffersTier tier = [device argumentBuffersSupport];
    if (tier < MTLArgumentBuffersTier2)
    {
        LOG_WARNING(ZRender,
                    "MetalBindlessTextureManager: device only supports Argument Buffer Tier1 "
                    "(max 128 entries per array). Tier2 is required for bindless. "
                    "Bindless support disabled.");
        return false;
    }

    m_Device   = device;
    m_Capacity = capacity;
    m_HighWaterMark = 0;
    m_FreeList.clear();
    m_FreeList.reserve(64);
    m_AllocatedTextures.clear();

    // ---- 2. Argument descriptors ----
    // Texture array at argument index 0.
    MTLArgumentDescriptor* tex_arg = [[MTLArgumentDescriptor alloc] init];
    tex_arg.dataType      = MTLDataTypeTexture;
    // access defaults to MTLArgumentAccessReadOnly; setting it explicitly
    // triggers -Wdeprecated-declarations on macOS 14+.
    tex_arg.index         = kTextureArrayArgIndex;
    tex_arg.arrayLength   = static_cast<NSUInteger>(capacity);
    tex_arg.textureType   = MTLTextureType2D;

    // Sampler array at argument index = capacity (flat index space:
    // texture array occupies ids 0..capacity-1, samplers start after).
    MTLArgumentDescriptor* samp_arg = [[MTLArgumentDescriptor alloc] init];
    samp_arg.dataType      = MTLDataTypeSampler;
    // access defaults to MTLArgumentAccessReadOnly; omit to avoid deprecation.
    samp_arg.index         = static_cast<NSUInteger>(capacity);
    samp_arg.arrayLength   = kNumStaticSamplers;

    NSArray<MTLArgumentDescriptor*>* args = @[tex_arg, samp_arg];

    m_ArgumentEncoder = [device newArgumentEncoderWithArguments:args];
    if (m_ArgumentEncoder == nil)
    {
        LOG_ERROR(ZRender, "MetalBindlessTextureManager: newArgumentEncoderWithArguments failed");
        [tex_arg release];
        [samp_arg release];
        return false;
    }

    // ---- 3. Backing buffer ----
    const NSUInteger buffer_size = [m_ArgumentEncoder encodedLength];
    // Align to 256 bytes (Metal requirement for argument buffer binding).
    const NSUInteger aligned_size = (buffer_size + 255) & ~255;

    m_ArgumentBuffer = [device newBufferWithLength:aligned_size
                                            options:MTLResourceStorageModeShared];
    if (m_ArgumentBuffer == nil)
    {
        LOG_ERROR(ZRender, "MetalBindlessTextureManager: newBufferWithLength failed");
        [tex_arg release];
        [samp_arg release];
        return false;
    }

    // Store layout offsets for incremental encoding.
    // The encoder's -encodedLength covers the whole layout; we query
    // individual argument offsets by probing after setArgumentBuffer.
    [m_ArgumentEncoder setArgumentBuffer:m_ArgumentBuffer offset:0];

    // ---- 4. Static samplers ----
    if (!createAndEncodeStaticSamplers())
    {
        LOG_ERROR(ZRender, "MetalBindlessTextureManager: failed to create static samplers");
        Shutdown();
        [tex_arg release];
        [samp_arg release];
        return false;
    }

    // ---- 5. Slot 0 placeholder ----
    m_HighWaterMark = 1; // reserve slot 0
    if (!CreateAndUploadPlaceholder())
    {
        LOG_ERROR(ZRender, "MetalBindlessTextureManager: failed to create slot-0 placeholder");
        Shutdown();
        [tex_arg release];
        [samp_arg release];
        return false;
    }

    [tex_arg release];
    [samp_arg release];

    LOG_INFO(ZRender,
             "MetalBindlessTextureManager: initialized (capacity = {}, Tier2, "
             "slot 0 = 1x1 white placeholder, {} static samplers)",
             m_Capacity,
             kNumStaticSamplers);
    return true;
}

// =====================================================================
// shutdown
// =====================================================================
void MetalBindlessTextureManager::Shutdown()
{
    DestroyPlaceholder();

    for (uint32_t i = 0; i < kNumStaticSamplers; ++i)
    {
#if !__has_feature(objc_arc)
        [m_StaticSamplers[i] release];
#endif
        m_StaticSamplers[i] = nil;
    }

#if !__has_feature(objc_arc)
    [m_ArgumentEncoder release];
    [m_ArgumentBuffer release];
#endif
    m_ArgumentEncoder = nil;
    m_ArgumentBuffer  = nil;

    m_AllocatedTextures.clear();
    m_Device          = nil;
    m_Capacity        = 0;
    m_HighWaterMark = 0;
    m_FreeList.clear();
}

// =====================================================================
// allocate / free / update
// =====================================================================
uint32_t MetalBindlessTextureManager::allocate(RHIImageView* image_view, RHISampler* sampler)
{
    if (m_ArgumentBuffer == nil || image_view == nullptr)
    {
        return kInvalidBindlessIndex;
    }

    // sampler is intentionally ignored on Metal (we use static samplers
    // from the argument buffer, same as DX12's root-signature static
    // sampler convention).
    (void)sampler;

    id<MTLTexture> mtl_tex = static_cast<MetalImageView*>(image_view)->getResource();
    if (mtl_tex == nil)
    {
        return kInvalidBindlessIndex;
    }

    uint32_t slot = kInvalidBindlessIndex;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_FreeList.empty())
        {
            slot = m_FreeList.back();
            m_FreeList.pop_back();
        }
        else if (m_HighWaterMark < m_Capacity)
        {
            slot = m_HighWaterMark++;
        }
        else
        {
            return kInvalidBindlessIndex;
        }

        writeTextureLocked(slot, mtl_tex);
        m_AllocatedTextures.push_back(mtl_tex);
    }
    return slot;
}

void MetalBindlessTextureManager::free(uint32_t index)
{
    if (index == kInvalidBindlessIndex || index >= m_Capacity || index == 0)
    {
        // Slot 0 is the reserved placeholder and must not be freed.
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    // We deliberately do NOT remove the texture from m_AllocatedTextures
    // here. Calling useResource on a valid but unused texture is harmless
    // (just a minor over-residency cost). A future optimization can
    // maintain a parallel slot-index->texture map and remove entries
    // precisely; for the expected working set size this is not a
    // bottleneck.
    m_FreeList.push_back(index);
}

void MetalBindlessTextureManager::Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler)
{
    (void)sampler;
    if (m_ArgumentBuffer == nil || index >= m_Capacity || image_view == nullptr)
    {
        return;
    }

    id<MTLTexture> mtl_tex = static_cast<MetalImageView*>(image_view)->getResource();
    if (mtl_tex == nil)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_HighWaterMark)
    {
        return;
    }
    writeTextureLocked(index, mtl_tex);
}

// =====================================================================
// writeTextureLocked
// =====================================================================
void MetalBindlessTextureManager::writeTextureLocked(uint32_t index, id<MTLTexture> texture)
{
    // Re-associate the encoder with the buffer (this does NOT clear
    // existing data -- only writes to slots we target).
    [m_ArgumentEncoder setArgumentBuffer:m_ArgumentBuffer offset:0];
    [m_ArgumentEncoder setTexture:texture
                           atIndex:static_cast<NSUInteger>(index)];
}

// =====================================================================
// ensureResidency
// ---------------------------------------------------------------------
// Metal requires explicit useResource / useResources calls for
// resources accessed through argument buffers. We make all currently-
// allocated textures resident in one batch call. This is conservative
// (not all draws need all textures) but simple and correct.
// =====================================================================
void MetalBindlessTextureManager::ensureResidency(id<MTLRenderCommandEncoder> encoder) const
{
    if (encoder == nil || m_ArgumentBuffer == nil)
    {
        return;
    }

    // The argument buffer itself must be made resident too.
    [encoder useResource:m_ArgumentBuffer
                  usage:MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];

    // Make the placeholder texture resident (slot 0 is always valid).
    if (m_PlaceholderTexture != nil)
    {
        [encoder useResource:m_PlaceholderTexture
                      usage:MTLResourceUsageRead
                      stages:MTLRenderStageVertex | MTLRenderStageFragment];
    }

    // Batch all allocated textures.
    if (!m_AllocatedTextures.empty())
    {
        // Collect non-nil entries.
        NSMutableArray<id<MTLTexture>>* textures =
            [NSMutableArray arrayWithCapacity:m_AllocatedTextures.size()];
        for (id<MTLTexture> tex : m_AllocatedTextures)
        {
            if (tex != nil)
            {
                [textures addObject:tex];
            }
        }
        if ([textures count] > 0)
        {
            [encoder useResources:textures
                           usage:MTLResourceUsageRead
                           stages:MTLRenderStageVertex | MTLRenderStageFragment];
        }
    }

    // Static samplers are encoded in the argument buffer; they are
    // implicitly resident when the argument buffer is resident.
}

void MetalBindlessTextureManager::ensureResidency(id<MTLComputeCommandEncoder> encoder) const
{
    if (encoder == nil || m_ArgumentBuffer == nil)
    {
        return;
    }

    [encoder useResource:m_ArgumentBuffer
                  usage:MTLResourceUsageRead];

    if (m_PlaceholderTexture != nil)
    {
        [encoder useResource:m_PlaceholderTexture
                      usage:MTLResourceUsageRead];
    }

    if (!m_AllocatedTextures.empty())
    {
        NSMutableArray<id<MTLTexture>>* textures =
            [NSMutableArray arrayWithCapacity:m_AllocatedTextures.size()];
        for (id<MTLTexture> tex : m_AllocatedTextures)
        {
            if (tex != nil)
            {
                [textures addObject:tex];
            }
        }
        if ([textures count] > 0)
        {
            [encoder useResources:textures
                           usage:MTLResourceUsageRead];
        }
    }
}

// =====================================================================
// createAndEncodeStaticSamplers
// ---------------------------------------------------------------------
// 4 static samplers matching DX12's bindless convention:
//   0: linear-wrap    (LinearMinMag, Repeat)
//   1: linear-Clamp   (LinearMinMag, ClampToEdge)
//   2: point-wrap     (NearestMinMag, Repeat)
//   3: point-Clamp    (NearestMinMag, ClampToEdge)
// =====================================================================
bool MetalBindlessTextureManager::createAndEncodeStaticSamplers()
{
    struct SamplerDef
    {
        MTLSamplerMinMagFilter minMag;
        MTLSamplerAddressMode  address;
    };
    const SamplerDef defs[kNumStaticSamplers] = {
        {MTLSamplerMinMagFilterLinear,  MTLSamplerAddressModeRepeat},     // 0: linear-wrap
        {MTLSamplerMinMagFilterLinear,  MTLSamplerAddressModeClampToEdge}, // 1: linear-clamp
        {MTLSamplerMinMagFilterNearest, MTLSamplerAddressModeRepeat},     // 2: point-wrap
        {MTLSamplerMinMagFilterNearest, MTLSamplerAddressModeClampToEdge}, // 3: point-clamp
    };

    for (uint32_t i = 0; i < kNumStaticSamplers; ++i)
    {
        MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
        desc.minFilter             = defs[i].minMag;
        desc.magFilter             = defs[i].minMag;
        desc.mipFilter             = MTLSamplerMipFilterLinear;
        desc.sAddressMode          = defs[i].address;
        desc.tAddressMode          = defs[i].address;
        desc.rAddressMode          = defs[i].address;
        desc.normalizedCoordinates = YES;

        m_StaticSamplers[i] = [m_Device newSamplerStateWithDescriptor:desc];
        [desc release];

        if (m_StaticSamplers[i] == nil)
        {
            LOG_ERROR(ZRender,
                      "MetalBindlessTextureManager: failed to create static sampler {}",
                      i);
            return false;
        }
    }

    // Encode the samplers into the argument buffer.
    [m_ArgumentEncoder setArgumentBuffer:m_ArgumentBuffer offset:0];
    for (uint32_t i = 0; i < kNumStaticSamplers; ++i)
    {
        [m_ArgumentEncoder setSamplerState:m_StaticSamplers[i]
                                    atIndex:static_cast<NSUInteger>(i)];
    }

    return true;
}

// =====================================================================
// createAndUploadPlaceholder
// ---------------------------------------------------------------------
// Self-contained: creates a 1x1 RGBA8Unorm texture, writes 0xFFFFFFFF
// (opaque white) into it via a blit command, then encodes it at
// slot 0 of the argument buffer.
//
// Metal's texture upload is simpler than Vulkan/DX12 because we can
// use newTextureWithDescriptor:iosurface: or, for small textures,
// write directly via replaceRegion.
// =====================================================================
bool MetalBindlessTextureManager::CreateAndUploadPlaceholder()
{
    // ---- 1. Create 1x1 RGBA8 texture ----
    MTLTextureDescriptor* tex_desc = [[MTLTextureDescriptor alloc] init];
    tex_desc.textureType     = MTLTextureType2D;
    tex_desc.pixelFormat     = MTLPixelFormatRGBA8Unorm;
    tex_desc.width           = 1;
    tex_desc.height          = 1;
    tex_desc.depth           = 1;
    tex_desc.mipmapLevelCount = 1;
    tex_desc.sampleCount     = 1;
    tex_desc.arrayLength     = 1;
    tex_desc.resourceOptions = MTLResourceStorageModeShared;
    tex_desc.usage           = MTLTextureUsageShaderRead;

    m_PlaceholderTexture = [m_Device newTextureWithDescriptor:tex_desc];
    [tex_desc release];

    if (m_PlaceholderTexture == nil)
    {
        LOG_ERROR(ZRender, "MetalBindlessTextureManager: placeholder newTextureWithDescriptor failed");
        return false;
    }

    // ---- 2. Upload 1x1 white texel ----
    const uint32_t white = 0xFFFFFFFFu; // R=G=B=A=0xFF
    const MTLRegion region = MTLRegionMake2D(0, 0, 1, 1);
    [m_PlaceholderTexture replaceRegion:region
                             mipmapLevel:0
                               withBytes:&white
                             bytesPerRow:4];

    // ---- 3. Encode at slot 0 ----
    writeTextureLocked(0, m_PlaceholderTexture);

    return true;
}

void MetalBindlessTextureManager::DestroyPlaceholder()
{
#if !__has_feature(objc_arc)
    [m_PlaceholderTexture release];
#endif
    m_PlaceholderTexture = nil;
}
