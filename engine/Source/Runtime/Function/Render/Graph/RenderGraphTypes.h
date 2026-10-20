#pragma once

// =====================================================================================
// RenderGraph -- the data model shared by the graph compiler and the RHI executor.
// -------------------------------------------------------------------------------------
// Deliberately RHI-free: the RHI classes below are only FORWARD declared as opaque
// pointers so that the whole compiler (RenderGraphBuilder + RenderGraph) can be unit
// tested headless -- see Test/RenderGraphSmokeTest.cpp. Nothing in this header may
// include Interface/RHI.h; the translation to RHIImageLayout / RHIAccessFlags /
// RHIImageUsageFlags happens once, in RenderGraphResources.cpp.
//
// Model (UE FRDG + Unity RenderGraph hybrid):
//   * Setup phase    -- passes declare the resources they read and write. Nothing is
//                       allocated, nothing is recorded. Cheap enough to run per frame.
//   * Compile phase  -- pure CPU: topological sort, culling of passes whose results
//                       nobody consumes, resource lifetime tracking, memory aliasing
//                       of transient resources, and barrier derivation.
//   * Execute phase  -- the RHI layer hands each pass its resolved physical resources
//                       and records into a command buffer.
// =====================================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class RHIImage;         // opaque: never dereferenced by the graph compiler
class RHIImageView;
class RHIBuffer;
class RHICommandBuffer;

constexpr uint32_t kRGInvalidIndex = 0xFFFFFFFFu;

// -------------------------------------------------------------------------------------
// Formats / usage -- a small RHI-independent mirror of the RHI enums. The executor maps
// these to the real RHI values; adding one here means adding one line to that map.
// -------------------------------------------------------------------------------------

enum class RGFormat : uint32_t
{
    Unknown = 0,
    R8_UNORM,
    R8_UINT,
    R16_UINT,
    R32_UINT,
    R32_SFLOAT,
    R16G16_SFLOAT,
    R32G32_SFLOAT,
    R8G8B8A8_UNORM,
    R8G8B8A8_SRGB,
    B8G8R8A8_UNORM,
    B8G8R8A8_SRGB,
    R16G16B16A16_SFLOAT,
    R32G32B32A32_SFLOAT,
    D32_SFLOAT,
    D24_UNORM_S8_UINT,
};

enum class RGTextureUsage : uint32_t
{
    None = 0,
    RenderTarget = 1u << 0,
    DepthStencil = 1u << 1,
    ShaderRead = 1u << 2,
    UnorderedAccess = 1u << 3,
    TransferSrc = 1u << 4,
    TransferDst = 1u << 5,
    InputAttachment = 1u << 6,
};

inline constexpr RGTextureUsage operator|(RGTextureUsage a, RGTextureUsage b)
{
    return static_cast<RGTextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr RGTextureUsage operator&(RGTextureUsage a, RGTextureUsage b)
{
    return static_cast<RGTextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr bool RGHasUsage(RGTextureUsage usage, RGTextureUsage bit)
{
    return (usage & bit) != RGTextureUsage::None;
}

enum class RGBufferUsage : uint32_t
{
    None = 0,
    Vertex = 1u << 0,
    Index = 1u << 1,
    Uniform = 1u << 2,
    ShaderRead = 1u << 3,
    UnorderedAccess = 1u << 4,
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6,
    Indirect = 1u << 7,
};

inline constexpr RGBufferUsage operator|(RGBufferUsage a, RGBufferUsage b)
{
    return static_cast<RGBufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr RGBufferUsage operator&(RGBufferUsage a, RGBufferUsage b)
{
    return static_cast<RGBufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline constexpr bool RGHasUsage(RGBufferUsage usage, RGBufferUsage bit)
{
    return (usage & bit) != RGBufferUsage::None;
}

/// The state a resource must be in for a pass to use it. The compiler turns a change of
/// this into exactly one barrier; the executor maps it to (layout, access mask, stage).
enum class RGResourceState : uint32_t
{
    Undefined = 0,
    RenderTarget,        // written as a color attachment
    DepthStencilWrite,   // written as a depth/stencil attachment
    DepthStencilRead,    // sampled / tested read-only
    ShaderResource,      // SRV / bindless / sampled in a shader
    UnorderedAccess,     // UAV / storage image
    CopySource,
    CopyDest,
    Present,             // swapchain image, ready for present
    Count
};

const char* RGToString(RGResourceState state);

enum class RGPassType : uint8_t
{
    Render = 0,  // graphics pipeline work
    Compute,     // compute pipeline work
    Copy,        // blit / copy only
};

const char* RGToString(RGPassType type);

// -------------------------------------------------------------------------------------
// Handles
// -------------------------------------------------------------------------------------
// Index + generation. The generation is what makes a stale handle (one captured during
// a previous frame's setup) fail loudly instead of silently pointing at a resource that
// now means something else.

struct RGTextureHandle
{
    uint32_t index {kRGInvalidIndex};
    uint32_t generation {0};

    bool IsValid() const { return index != kRGInvalidIndex; }
};

struct RGBufferHandle
{
    uint32_t index {kRGInvalidIndex};
    uint32_t generation {0};

    bool IsValid() const { return index != kRGInvalidIndex; }
};

inline bool operator==(const RGTextureHandle& a, const RGTextureHandle& b)
{
    return a.index == b.index && a.generation == b.generation;
}

inline bool operator!=(const RGTextureHandle& a, const RGTextureHandle& b) { return !(a == b); }

inline bool operator==(const RGBufferHandle& a, const RGBufferHandle& b)
{
    return a.index == b.index && a.generation == b.generation;
}

inline bool operator!=(const RGBufferHandle& a, const RGBufferHandle& b) { return !(a == b); }

// -------------------------------------------------------------------------------------
// Descriptors
// -------------------------------------------------------------------------------------

struct RGTextureDesc
{
    std::string name;
    uint32_t width {0};
    uint32_t height {0};
    uint32_t depth {1};
    uint32_t mip_levels {1};
    uint32_t array_size {1};
    RGFormat format {RGFormat::Unknown};
    RGTextureUsage usage {RGTextureUsage::None};

    /// True for resources the graph owns: their memory may be aliased with another
    /// transient resource whose lifetime does not overlap (UE's transient allocator).
    /// Imported / external resources are never aliased.
    bool transient {true};

    /// Approximate footprint, used for the memory budget and for aliasing decisions.
    size_t GetByteSize() const;
};

struct RGBufferDesc
{
    std::string name;
    size_t size_bytes {0};
    RGBufferUsage usage {RGBufferUsage::None};
    bool transient {true};

    size_t GetByteSize() const { return size_bytes; }
};

/// A texture that already exists (swapchain image, a pass-owned RT, a scene depth
/// target). The graph never allocates or destroys it and never aliases its memory; it
/// only inserts the barriers needed to reach the state each pass asks for.
struct RGExternalTexture
{
    RHIImage* image {nullptr};
    RHIImageView* view {nullptr};
    RGResourceState state {RGResourceState::Undefined};
};

struct RGExternalBuffer
{
    RHIBuffer* buffer {nullptr};
    RGResourceState state {RGResourceState::Undefined};
};

// -------------------------------------------------------------------------------------
// Callbacks
// -------------------------------------------------------------------------------------

class RenderGraphBuilder;

/// Runs once per pass during Setup. Declares reads / writes; may create resources.
using RGSetupFn = std::function<void(RenderGraphBuilder&)>;

/// Forward declared: defined by RenderGraphResources.h so the compiler side stays
/// RHI-free while the pass body still gets real RHI handles.
class RenderGraphExecuteContext;

/// Runs once per pass during Execute, in the order the compiler chose.
using RGExecuteFn = std::function<void(RenderGraphExecuteContext&)>;

/// Per-pass flags.
enum class RGPassFlags : uint32_t
{
    None = 0,
    /// The pass has a side effect the graph cannot see (writes an external UAV, kicks a
    /// copy to a readback buffer). It is never culled.
    NeverCull = 1u << 0,
};

inline constexpr RGPassFlags operator|(RGPassFlags a, RGPassFlags b)
{
    return static_cast<RGPassFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline constexpr RGPassFlags operator&(RGPassFlags a, RGPassFlags b)
{
    return static_cast<RGPassFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
