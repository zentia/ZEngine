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

// ---- Image ----

class MetalImage : public RHIImage
{
public:
    void setResource(id<MTLTexture> tex) { m_Resource = tex; }
    id<MTLTexture> getResource() const { return m_Resource; }

private:
    id<MTLTexture> m_Resource = nil;
};

// ---- Image view ----

class MetalImageView : public RHIImageView
{
public:
    void setResource(id<MTLTexture> tex) { m_Resource = tex; }
    id<MTLTexture> getResource() const { return m_Resource; }

private:
    id<MTLTexture> m_Resource = nil;
};

// ---- Sampler ----

class MetalSampler : public RHISampler
{
public:
    void setResource(id<MTLSamplerState> samp) { m_Resource = samp; }
    id<MTLSamplerState> getResource() const { return m_Resource; }

private:
    id<MTLSamplerState> m_Resource = nil;
};

// ---- Buffer ----

class MetalBuffer : public RHIBuffer
{
public:
    void setResource(id<MTLBuffer> buf) { m_Resource = buf; }
    id<MTLBuffer> getResource() const { return m_Resource; }

private:
    id<MTLBuffer> m_Resource = nil;
};

// ---- Device memory (placeholder -- Metal uses unified memory) ----

class MetalDeviceMemory : public RHIDeviceMemory
{
public:
    void* getHostPtr() const { return m_HostPtr; }
    void setHostPtr(void* p) { m_HostPtr = p; }

private:
    void* m_HostPtr = nullptr;
};
