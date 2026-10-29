#pragma once
// ============================================================================
// ZSlateD3D11Renderer — D3D11 GPU backend for ZSlateBatchedRenderer
// ============================================================================
// Create once per window.  Feed it a ZSlateBatchedRenderer each frame; it
// uploads vertices/indices, updates the font atlas texture, and draws.
//
// Usage:
//   ZSlateD3D11Renderer r(hwnd);
//   r.Init();
//   loop { batched.BeginFrame(); ... paint ... batched.EndFrame();
//          r.Render(batched, &fontAtlas); }
//   r.Shutdown();
// ============================================================================

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#undef DrawText  // windows.h #defines DrawText → DrawTextA/W
#endif

namespace ZSlate
{

class ZSlateBatchedRenderer;
class ZSlateFontAtlas;

class ZSlateD3D11Renderer
{
public:
    explicit ZSlateD3D11Renderer(HWND hwnd);
    ~ZSlateD3D11Renderer();

    ZSlateD3D11Renderer(const ZSlateD3D11Renderer&) = delete;
    ZSlateD3D11Renderer& operator=(const ZSlateD3D11Renderer&) = delete;

    bool Init();
    void Shutdown();
    void Resize(uint32_t w, uint32_t h);

    // Draw one frame.  Uploads dirty atlas regions automatically.
    void Render(ZSlateBatchedRenderer& batch, ZSlateFontAtlas* atlas = nullptr);

    uint32_t GetWidth()  const { return m_Width; }
    uint32_t GetHeight() const { return m_Height; }

#ifdef _WIN32
    ID3D11Device*        GetDevice()  { return m_Device; }
    ID3D11DeviceContext* GetContext() { return m_Context; }
#endif

private:
    void CreateWhiteTexture();
    void UploadFontAtlas(ZSlateFontAtlas& atlas);
    void VerifyAtlasOnGPU(ZSlateFontAtlas& atlas);
    bool CompileShaders();

    HWND m_Hwnd;

    ID3D11Device*           m_Device    = nullptr;
    ID3D11DeviceContext*    m_Context   = nullptr;
    IDXGISwapChain*         m_SwapChain = nullptr;
    ID3D11RenderTargetView* m_RTV       = nullptr;

    ID3D11VertexShader*    m_VS = nullptr;
    ID3D11PixelShader*     m_PS = nullptr;
    ID3D11InputLayout*     m_InputLayout = nullptr;
    ID3D11BlendState*      m_BlendState = nullptr;
    ID3D11RasterizerState* m_RasterState = nullptr;
    ID3D11Buffer*          m_PerFrameCB = nullptr;

    ID3D11SamplerState*       m_Sampler = nullptr;
    ID3D11ShaderResourceView* m_WhiteSRV = nullptr;

    // Per-frame buffers (re-created each Render)
    ID3D11Buffer* m_VB = nullptr;
    ID3D11Buffer* m_IB = nullptr;

    // Font atlas texture
    ID3D11Texture2D*          m_FontTex = nullptr;
    ID3D11ShaderResourceView* m_FontSRV = nullptr;
    bool m_AtlasVerified = false;  // one-shot GPU readback self-check

    uint32_t m_Width  = 0;
    uint32_t m_Height = 0;
};

}  // namespace ZSlate
