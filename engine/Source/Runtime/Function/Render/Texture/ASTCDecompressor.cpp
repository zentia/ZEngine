// =============================================================================
// ASTCDecompressor.cpp
// -----------------------------------------------------------------------------
// Implementation of ASTC texture decompression using ARM's astcenc library.
//
// References:
//   - ARM astcenc API (astcenc.h)
//   - Unity's TextureDecompression.cpp (DecompressASTC)
// =============================================================================

// Texture2D is in GLOBAL namespace, NOT in ZEngine::Render
// Must include BEFORE entering ZEngine::Render namespace
#include "Runtime/Function/Render/Texture/Texture2D.h"

#include "ASTCDecompressor.h"

// astcenc public API
// Note: The astcenc library is built from engine/3rdparty/astc-encoder/Source/
#include <astcenc.h>

#include <algorithm>
#include <cstring>
#include <functional>  // for std::hash<>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ZEngine::Render
{

// =============================================================================
// Internal helpers
// =============================================================================

namespace
{

// Map ZEngine ASTCBlockSize to block dimensions
void GetBlockDimensions(ASTCBlockSize block_size, unsigned int& block_x, unsigned int& block_y, unsigned int& block_z)
{
    switch (block_size)
    {
        case ASTCBlockSize::ASTC_4x4:
            block_x = 4;  block_y = 4;  block_z = 1;  break;
        case ASTCBlockSize::ASTC_5x5:
            block_x = 5;  block_y = 5;  block_z = 1;  break;
        case ASTCBlockSize::ASTC_6x6:
            block_x = 6;  block_y = 6;  block_z = 1;  break;
        case ASTCBlockSize::ASTC_8x8:
            block_x = 8;  block_y = 8;  block_z = 1;  break;
        case ASTCBlockSize::ASTC_10x10:
            block_x = 10; block_y = 10; block_z = 1;  break;
        case ASTCBlockSize::ASTC_12x12:
            block_x = 12; block_y = 12; block_z = 1;  break;
        default:
            block_x = 4;  block_y = 4;  block_z = 1;  break;
    }
}

// Default swizzle (RGBA)
const astcenc_swizzle kDefaultSwizzle = {
    ASTCENC_SWZ_R,
    ASTCENC_SWZ_G,
    ASTCENC_SWZ_B,
    ASTCENC_SWZ_A
};

}  // anonymous namespace

// =============================================================================
// Thread-local context pool (mirrors Unity's ASTCDecompressorContextPool)
// =============================================================================

class ASTCContextPool
{
public:
    static ASTCContextPool& GetInstance()
    {
        static ASTCContextPool instance;
        return instance;
    }

    // Get or create a decompress-only context
    astcenc_context* GetContext(unsigned int block_x, unsigned int block_y, unsigned int block_z)
    {
        std::thread::id tid = std::this_thread::get_id();
        
        std::lock_guard<std::mutex> lock(m_Mutex);
        
        // Create a key from block dimensions + thread id
        // Note: std::thread::id doesn't have hash() member, use std::hash instead
        std::hash<std::thread::id> hasher;
        uint64_t key = (static_cast<uint64_t>(block_x) << 32) | 
                       (static_cast<uint64_t>(block_y) << 16) | 
                       block_z;
        key ^= (static_cast<uint64_t>(hasher(tid)) << 40);
        
        auto it = m_Contexts.find(key);
        if (it != m_Contexts.end())
        {
            return it->second;
        }

        // Create configuration for decompression
        astcenc_config config;
        astcenc_error status = astcenc_config_init(
            ASTCENC_PRF_LDR_SRGB,  // Profile: LDR sRGB
            block_x,
            block_y,
            block_z,
            ASTCENC_PRE_FASTEST,      // Quality: fastest (not used for decompression, but required)
            ASTCENC_FLG_DECOMPRESS_ONLY,
            &config);

        if (status != ASTCENC_SUCCESS)
        {
            return nullptr;
        }

        // Create new context
        astcenc_context* ctx = nullptr;
        status = astcenc_context_alloc(&config, 1, &ctx, nullptr);
        
        if (status == ASTCENC_SUCCESS)
        {
            m_Contexts[key] = ctx;
            return ctx;
        }
        
        return nullptr;
    }

    void ReleaseAllContexts()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (auto& pair : m_Contexts)
        {
            if (pair.second != nullptr)
            {
                astcenc_context_free(pair.second);
            }
        }
        m_Contexts.clear();
    }

private:
    ASTCContextPool() = default;
    ~ASTCContextPool()
    {
        ReleaseAllContexts();
    }

    std::mutex m_Mutex;
    std::unordered_map<uint64_t, astcenc_context*> m_Contexts;
};

// =============================================================================
// Public API implementation
// =============================================================================

bool ASTCDecompressor::Initialize()
{
    // astcenc doesn't need global initialization
    // Contexts are created on-demand per thread
    return true;
}

void ASTCDecompressor::Shutdown()
{
    ASTCContextPool::GetInstance().ReleaseAllContexts();
}

ASTCDecompressResult ASTCDecompressor::Decompress(
    const uint8_t* compressed_data,
    size_t data_size,
    uint32_t width,
    uint32_t height,
    ASTCBlockSize block_size)
{
    ASTCDecompressResult result;
    result.success = false;

    if (compressed_data == nullptr || data_size == 0 || width == 0 || height == 0)
    {
        result.error_message = "Invalid input parameters";
        return result;
    }

    // Get block dimensions
    unsigned int block_x, block_y, block_z;
    GetBlockDimensions(block_size, block_x, block_y, block_z);

    // Get or create context
    astcenc_context* ctx = ASTCContextPool::GetInstance().GetContext(block_x, block_y, block_z);
    if (ctx == nullptr)
    {
        result.error_message = "Failed to create astcenc context";
        return result;
    }

    // Prepare output buffer (RGBA8)
    result.width = width;
    result.height = height;
    result.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    // Prepare output image descriptor
    // Note: astcenc_image.data is an array of pointers to 2D slices
    // For a 2D image (dim_z = 1), data[0] points to the entire image
    std::vector<void*> data_ptrs;
    data_ptrs.push_back(result.pixels.data());

    astcenc_image output_image;
    output_image.dim_x = width;
    output_image.dim_y = height;
    output_image.dim_z = 1;
    output_image.data_type = ASTCENC_TYPE_U8;
    output_image.data = data_ptrs.data();

    // Decompress
    astcenc_error status = astcenc_decompress_image(
        ctx,
        compressed_data,
        data_size,
        &output_image,
        &kDefaultSwizzle,
        0);  // thread_index

    if (status != ASTCENC_SUCCESS)
    {
        result.error_message = std::string("astcenc_decompress_image failed: ") + astcenc_get_error_string(status);
        result.pixels.clear();
        return result;
    }

    result.success = true;
    return result;
}

// =============================================================================
// DecompressTexture implementation
// =============================================================================

ASTCDecompressResult ASTCDecompressor::DecompressTexture(const ::Texture2D* texture)
{
    ASTCDecompressResult result;
    result.success = false;

    if (texture == nullptr || !texture->IsValid())
    {
        result.error_message = "Invalid texture";
        return result;
    }

    // TODO: Check m_Format to confirm it's ASTC
    // For now, assume ASTC 4x4 (most common)
    ASTCBlockSize block_size = ASTCBlockSize::ASTC_4x4;
    
    // Get compressed data from m_Pixels (mip 0)
    ::Texture2D::MipSpan mip0 = texture->GetMipSpan(0);
    if (mip0.data == nullptr || mip0.size == 0)
    {
        result.error_message = "No compressed data in texture";
        return result;
    }

    // Decompress
    result = Decompress(
        mip0.data,
        mip0.size,
        texture->m_Width,
        texture->m_Height,
        block_size);

    return result;
}

}  // namespace ZEngine::Render
