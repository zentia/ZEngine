#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"
#include "ZSlate/Core/SlateLog.h"
#include "ZSlate/Renderer/ZSlateBatchedRenderer.h"
#include "ZSlate/Renderer/ZSlateFontAtlas.h"

#ifdef _WIN32
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ZSlate
{

// ---- Embedded HLSL source (compiled at runtime) ----------------------------

namespace
{
    // VS: screen coords → clip space
    const char kVS[] = R"(
struct VSI { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };
struct VSO { float4 sp : SV_POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };
cbuffer CB : register(b0) { float2 vp; }
VSO main(VSI i) { VSO o; o.sp = float4((i.p/vp)*float2(2,-2)+float2(-1,1),0,1); o.uv=i.uv; o.c=i.c; return o; }
)";

    // PS: bilinear-sampled texture × vertex color.a (white texture → solid fill)
    const char kPS[] = R"(
Texture2D tex : register(t0); SamplerState sam : register(s0);
struct PSI { float4 sp : SV_POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };
float4 main(PSI i) : SV_TARGET { float4 t = tex.Sample(sam, i.uv); return float4(i.c.rgb, i.c.a*t.a); }
)";
}

// ---- Constructor / Destructor -----------------------------------------------

ZSlateD3D11Renderer::ZSlateD3D11Renderer(HWND hwnd) : m_Hwnd(hwnd) {}

ZSlateD3D11Renderer::~ZSlateD3D11Renderer() { Shutdown(); }

// ---- Init -------------------------------------------------------------------

bool ZSlateD3D11Renderer::Init()
{
    #ifndef _WIN32
    std::fprintf(stderr, "ZSlateD3D11Renderer: Windows only\n");
    return false;
    #else

    RECT rc; GetClientRect(m_Hwnd, &rc);
    m_Width = rc.right - rc.left; m_Height = rc.bottom - rc.top;
    if (m_Width == 0) m_Width = 800;
    if (m_Height == 0) m_Height = 600;

    DXGI_SWAP_CHAIN_DESC sd {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width  = m_Width;
    sd.BufferDesc.Height = m_Height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_Hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    D3D_FEATURE_LEVEL fl{};
    D3D_DRIVER_TYPE driverUsed = D3D_DRIVER_TYPE_UNKNOWN;
    const D3D_FEATURE_LEVEL required[] = {D3D_FEATURE_LEVEL_11_0};
    auto create = [&](D3D_DRIVER_TYPE driver) {
        driverUsed = driver;
        return D3D11CreateDeviceAndSwapChain(
            nullptr, driver, nullptr, D3D11_CREATE_DEVICE_SINGLETHREADED,
            required, 1, D3D11_SDK_VERSION, &sd, &m_SwapChain, &m_Device, &fl, &m_Context);
    };
    HRESULT deviceResult = create(D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(deviceResult))
    {
        Shutdown();
        deviceResult = create(D3D_DRIVER_TYPE_WARP);
    }
    if (FAILED(deviceResult))
    {
        Shutdown();
        ZSlate::Log(ZSlate::ELogLevel::Error, "[ZSlateD3D11] hardware and WARP init failed: 0x%08lx",
                    static_cast<unsigned long>(deviceResult));
        return false;
    }

    // Which GPU the UI actually rendered on: when a user reports broken output,
    // "Intel UHD 620, FL 0xb100, WARP fallback: no" in their log beats asking
    // them to run dxdiag. Goes through the sink so a WINDOWS-subsystem host
    // (no stderr) still records it.
    {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(m_Device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice)
        {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
            {
                DXGI_ADAPTER_DESC desc {};
                if (SUCCEEDED(adapter->GetDesc(&desc)))
                {
                    char name[128] = {};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                    ZSlate::Log(ZSlate::ELogLevel::Info, "[ZSlateD3D11] adapter '%s', feature level 0x%04x%s",
                                name, static_cast<unsigned>(fl),
                                driverUsed == D3D_DRIVER_TYPE_WARP ? ", WARP fallback" : "");
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }
    }

    ID3D11Texture2D* bb = nullptr;
    m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (bb) { m_Device->CreateRenderTargetView(bb, nullptr, &m_RTV); bb->Release(); }

    if (!CompileShaders()) return false;

    // Blend: premultiplied alpha
    D3D11_BLEND_DESC bd {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_Device->CreateBlendState(&bd, &m_BlendState);

    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE;
    m_Device->CreateRasterizerState(&rd, &m_RasterState);

    D3D11_BUFFER_DESC cbd {};
    cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_Device->CreateBuffer(&cbd, nullptr, &m_PerFrameCB);

    // Point sampler — crisp glyph edges (bilinear smudges font atlas edges)
    D3D11_SAMPLER_DESC samp {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_Device->CreateSamplerState(&samp, &m_Sampler);

    CreateWhiteTexture();
    ZSlate::Log(ZSlate::ELogLevel::Info, "[ZSlateD3D11] init ok (%ux%u)", m_Width, m_Height);
    return true;
    #endif
}

void ZSlateD3D11Renderer::CreateWhiteTexture()
{
    #ifndef _WIN32
    return;
    #else
    uint8_t white[4] = {255, 255, 255, 255};
    D3D11_SUBRESOURCE_DATA sd {white, 4, 4};
    D3D11_TEXTURE2D_DESC td {};
    td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr;
    m_Device->CreateTexture2D(&td, &sd, &tex);
    if (tex) { m_Device->CreateShaderResourceView(tex, nullptr, &m_WhiteSRV); tex->Release(); }
    #endif
}

bool ZSlateD3D11Renderer::CompileShaders()
{
    #ifndef _WIN32
    return false;
    #else
    UINT fl = D3DCOMPILE_ENABLE_STRICTNESS;
    #ifdef _DEBUG
    fl |= D3DCOMPILE_DEBUG;
    #endif
    ID3DBlob *blob = nullptr, *err = nullptr;

    if (FAILED(D3DCompile(kVS, sizeof(kVS)-1, "vs", nullptr, nullptr, "main", "vs_5_0", fl, 0, &blob, &err)))
    { if (err) std::fprintf(stderr, "VS err: %s\n", (char*)err->GetBufferPointer()); return false; }
    m_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_VS);

    D3D11_INPUT_ELEMENT_DESC lay[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
    };
    m_Device->CreateInputLayout(lay, 3, blob->GetBufferPointer(), blob->GetBufferSize(), &m_InputLayout);
    blob->Release();

    if (FAILED(D3DCompile(kPS, sizeof(kPS)-1, "ps", nullptr, nullptr, "main", "ps_5_0", fl, 0, &blob, &err)))
    { if (err) std::fprintf(stderr, "PS err: %s\n", (char*)err->GetBufferPointer()); return false; }
    m_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &m_PS);
    blob->Release();
    return true;
    #endif
}

// ---- Resize -----------------------------------------------------------------

void ZSlateD3D11Renderer::Resize(uint32_t w, uint32_t h)
{
    if (w == m_Width && h == m_Height) return;
    m_Width = w; m_Height = h;
    #ifdef _WIN32
    if (m_RTV) { m_RTV->Release(); m_RTV = nullptr; }
    m_Context->OMSetRenderTargets(0, nullptr, nullptr);
    m_SwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* bb = nullptr;
    m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    if (bb) { m_Device->CreateRenderTargetView(bb, nullptr, &m_RTV); bb->Release(); }
    #endif
}

// ---- Font Atlas Upload ------------------------------------------------------

void ZSlateD3D11Renderer::UploadFontAtlas(ZSlateFontAtlas& atlas)
{
    #ifndef _WIN32
    (void)atlas; return;
    #else
    if (!atlas.IsDirty() || !atlas.IsLoaded()) return;

    uint32_t aw = atlas.GetWidth(), ah = atlas.GetHeight();
    if (!m_FontTex)
    {
        D3D11_TEXTURE2D_DESC td {};
        td.Width = aw; td.Height = ah; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        m_Device->CreateTexture2D(&td, nullptr, &m_FontTex);
        m_Device->CreateShaderResourceView(m_FontTex, nullptr, &m_FontSRV);
    }

    // DEFAULT + UpdateSubresource instead of DYNAMIC + Map/WRITE_DISCARD.
    // UpdateSubresource is the one CPU-to-GPU path every driver implements
    // identically -- pitch is passed explicitly and the runtime stages
    // internally -- whereas DYNAMIC Map hands back a driver-chosen pitch and
    // was the prime suspect for a user report where every glyph sampled
    // garbage while positions and UVs were provably correct.
    m_Context->UpdateSubresource(m_FontTex, 0, nullptr, atlas.GetPixels(),
                                 aw * 4u, 0u);
    atlas.ClearDirty();

    if (!m_AtlasVerified)
    {
        m_AtlasVerified = true;
        VerifyAtlasOnGPU(atlas);
    }
    #endif
}

// One-shot GPU readback: copy the uploaded atlas back through a staging
// texture and compare it against the CPU buffer. On a machine we cannot
// reproduce on, this is what separates "the upload is corrupt" (driver /
// upload path) from "the sampling is corrupt" (our UVs or vertex data): the
// verdict is computed on the user's GPU and lands in their log. Costs one
// 16 MB GPU->CPU copy once per process.
void ZSlateD3D11Renderer::VerifyAtlasOnGPU(ZSlateFontAtlas& atlas)
{
    if (!m_FontTex) return;
    D3D11_TEXTURE2D_DESC td {};
    td.Width = atlas.GetWidth(); td.Height = atlas.GetHeight();
    td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(m_Device->CreateTexture2D(&td, nullptr, &staging)) || !staging)
    {
        ZSlate::Log(ZSlate::ELogLevel::Warning, "[ZSlateD3D11] atlas self-check: cannot create staging texture");
        return;
    }
    m_Context->CopyResource(staging, m_FontTex);
    D3D11_MAPPED_SUBRESOURCE m {};
    const HRESULT hr = m_Context->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr))
    {
        ZSlate::Log(ZSlate::ELogLevel::Warning, "[ZSlateD3D11] atlas self-check: Map failed 0x%08lx",
                    static_cast<unsigned long>(hr));
        staging->Release();
        return;
    }
    const uint8_t* src = atlas.GetPixels();
    const size_t row_bytes = static_cast<size_t>(atlas.GetWidth()) * 4u;
    uint32_t bad_rows = 0;
    for (uint32_t row = 0; row < atlas.GetHeight(); ++row)
    {
        const uint8_t* gpu = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(row) * m.RowPitch;
        if (std::memcmp(gpu, src + static_cast<size_t>(row) * row_bytes, row_bytes) != 0)
        {
            if (bad_rows < 8)
                ZSlate::Log(ZSlate::ELogLevel::Error,
                            "[ZSlateD3D11] atlas self-check: row %u differs (GPU copy is corrupt)", row);
            ++bad_rows;
        }
    }
    m_Context->Unmap(staging, 0);
    staging->Release();
    if (bad_rows == 0)
        ZSlate::Log(ZSlate::ELogLevel::Info, "[ZSlateD3D11] atlas self-check: OK, all %u rows match the CPU atlas",
                    atlas.GetHeight());
    else
        ZSlate::Log(ZSlate::ELogLevel::Error, "[ZSlateD3D11] atlas self-check: %u/%u rows differ",
                    bad_rows, atlas.GetHeight());
}

// ---- Render -----------------------------------------------------------------

void ZSlateD3D11Renderer::Render(ZSlateBatchedRenderer& batch, ZSlateFontAtlas* atlas)
{
    #ifndef _WIN32
    (void)batch; (void)atlas; return;
    #else
    if (!m_Device) return;

    if (atlas) UploadFontAtlas(*atlas);

    const auto& v = batch.GetVertices();
    const auto& i = batch.GetIndices();
    const auto& c = batch.GetCommands();

    if (m_VB) { m_VB->Release(); m_VB = nullptr; }
    { D3D11_BUFFER_DESC b {}; b.ByteWidth=(UINT)(v.size()*sizeof(ZSBVertex));
      b.Usage=D3D11_USAGE_DYNAMIC; b.BindFlags=D3D11_BIND_VERTEX_BUFFER; b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
      D3D11_SUBRESOURCE_DATA s {}; s.pSysMem=v.data();
      m_Device->CreateBuffer(&b, v.empty()?nullptr:&s, &m_VB); }
    if (m_IB) { m_IB->Release(); m_IB = nullptr; }
    { D3D11_BUFFER_DESC b {}; b.ByteWidth=(UINT)(i.size()*sizeof(uint16_t));
      b.Usage=D3D11_USAGE_DYNAMIC; b.BindFlags=D3D11_BIND_INDEX_BUFFER; b.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
      D3D11_SUBRESOURCE_DATA s {}; s.pSysMem=i.data();
      m_Device->CreateBuffer(&b, i.empty()?nullptr:&s, &m_IB); }

    float clr[4] = {0.08f, 0.10f, 0.18f, 1.0f};
    m_Context->ClearRenderTargetView(m_RTV, clr);
    m_Context->OMSetRenderTargets(1, &m_RTV, nullptr);
    m_Context->RSSetState(m_RasterState);

    D3D11_VIEWPORT vp {}; vp.Width=(FLOAT)m_Width; vp.Height=(FLOAT)m_Height; vp.MaxDepth=1.0f;
    m_Context->RSSetViewports(1, &vp);

    D3D11_MAPPED_SUBRESOURCE m;
    m_Context->Map(m_PerFrameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    ((float*)m.pData)[0]=(float)m_Width; ((float*)m.pData)[1]=(float)m_Height;
    m_Context->Unmap(m_PerFrameCB, 0);
    m_Context->VSSetConstantBuffers(0, 1, &m_PerFrameCB);

    m_Context->OMSetBlendState(m_BlendState, nullptr, 0xFFFFFFFF);
    m_Context->VSSetShader(m_VS, nullptr, 0);
    m_Context->PSSetShader(m_PS, nullptr, 0);
    m_Context->PSSetSamplers(0, 1, &m_Sampler);
    m_Context->IASetInputLayout(m_InputLayout);
    m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    UINT stride = sizeof(ZSBVertex), offset = 0;
    m_Context->IASetVertexBuffers(0, 1, &m_VB, &stride, &offset);
    m_Context->IASetIndexBuffer(m_IB, DXGI_FORMAT_R16_UINT, 0);

    for (const auto& cmd : c)
    {
        ID3D11ShaderResourceView* srv = m_WhiteSRV;
        if (cmd.TextureId == ZSlateBatchedRenderer::kFontAtlasTextureId && m_FontSRV)
            srv = m_FontSRV;
        else if (cmd.TextureId != nullptr && cmd.TextureId != ZSlateBatchedRenderer::kFontAtlasTextureId)
            srv = static_cast<ID3D11ShaderResourceView*>(cmd.TextureId);  // custom textures (e.g. TexPreview)
        m_Context->PSSetShaderResources(0, 1, &srv);

        if (cmd.HasClip) {
            D3D11_RECT sc = {(LONG)(std::max)(cmd.ClipRect.x,0.0f),(LONG)(std::max)(cmd.ClipRect.y,0.0f),
                             (LONG)(std::max)(cmd.ClipRect.x+cmd.ClipRect.w,0.0f),
                             (LONG)(std::max)(cmd.ClipRect.y+cmd.ClipRect.h,0.0f)};
            m_Context->RSSetScissorRects(1, &sc);
        } else {
            D3D11_RECT full={0,0,(LONG)m_Width,(LONG)m_Height}; m_Context->RSSetScissorRects(1, &full);
        }
        m_Context->DrawIndexed(cmd.IndexCount, cmd.IndexOffset, 0);
    }

    m_SwapChain->Present(1, 0);
    #endif
}

// ---- Shutdown ---------------------------------------------------------------

void ZSlateD3D11Renderer::Shutdown()
{
    #ifdef _WIN32
    if (m_VB) m_VB->Release(); if (m_IB) m_IB->Release();
    if (m_PerFrameCB) m_PerFrameCB->Release();
    if (m_BlendState) m_BlendState->Release();
    if (m_RasterState) m_RasterState->Release();
    if (m_InputLayout) m_InputLayout->Release();
    if (m_VS) m_VS->Release(); if (m_PS) m_PS->Release();
    if (m_WhiteSRV) m_WhiteSRV->Release();
    if (m_FontSRV) m_FontSRV->Release(); if (m_FontTex) m_FontTex->Release();
    if (m_Sampler) m_Sampler->Release();
    if (m_RTV) m_RTV->Release();
    if (m_SwapChain) m_SwapChain->Release();
    if (m_Context) m_Context->Release();
    if (m_Device) m_Device->Release();

    m_Device = nullptr; m_Context = nullptr; m_SwapChain = nullptr; m_RTV = nullptr;
    m_VS = nullptr; m_PS = nullptr; m_InputLayout = nullptr;
    m_BlendState = nullptr; m_RasterState = nullptr; m_PerFrameCB = nullptr;
    m_Sampler = nullptr; m_WhiteSRV = nullptr;
    m_FontSRV = nullptr; m_FontTex = nullptr;
    m_VB = nullptr; m_IB = nullptr;
    #endif
}

}  // namespace ZSlate
