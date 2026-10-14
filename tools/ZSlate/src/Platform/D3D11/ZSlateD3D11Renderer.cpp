#include "ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ZSlate
{

namespace
{
    struct FVertex
    {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    struct FViewportConstants
    {
        float Width;
        float Height;
        float Pad0;
        float Pad1;
    };

    const char* kShaderSource = R"(
cbuffer ViewportConstants : register(b0)
{
    float2 ViewportSize;
    float2 Padding;
};

struct VSInput
{
    float2 Position : POSITION;
    float2 TexCoord : TEXCOORD0;
    float4 Color    : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
    float4 Color    : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float2 clip = float2((input.Position.x / ViewportSize.x) * 2.0 - 1.0,
                          1.0 - (input.Position.y / ViewportSize.y) * 2.0);
    output.Position = float4(clip, 0.0, 1.0);
    output.TexCoord = input.TexCoord;
    output.Color    = input.Color;
    return output;
}

Texture2D    FontTexture  : register(t0);
SamplerState FontSampler  : register(s0);

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Solid quads carry a negative U so they skip the texture fetch.
    if (input.TexCoord.x < 0.0)
    {
        return input.Color;
    }
    float coverage = FontTexture.Sample(FontSampler, input.TexCoord).a;
    return float4(input.Color.rgb, input.Color.a * coverage);
}
)";

    void PushQuad(std::vector<FVertex>& out, const UIRect& rect, const UIColor& color,
                  const UIRect& uv)
    {
        const float x0 = rect.x, y0 = rect.y;
        const float x1 = rect.x + rect.w, y1 = rect.y + rect.h;
        const float u0 = uv.x, v0 = uv.y;
        const float u1 = uv.x + uv.w, v1 = uv.y + uv.h;

        FVertex vertices[6] = {
            { x0, y0, u0, v0, color.r, color.g, color.b, color.a },
            { x1, y0, u1, v0, color.r, color.g, color.b, color.a },
            { x1, y1, u1, v1, color.r, color.g, color.b, color.a },
            { x0, y0, u0, v0, color.r, color.g, color.b, color.a },
            { x1, y1, u1, v1, color.r, color.g, color.b, color.a },
            { x0, y1, u0, v1, color.r, color.g, color.b, color.a },
        };
        out.insert(out.end(), vertices, vertices + 6);
    }
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct ZSlateD3D11Renderer::Impl
{
    HWND                      Hwnd        = nullptr;
    ID3D11Device*             Device      = nullptr;
    ID3D11DeviceContext*      Context     = nullptr;
    IDXGISwapChain*           SwapChain   = nullptr;
    ID3D11RenderTargetView*   RenderTarget = nullptr;

    ID3D11VertexShader*       VertexShader = nullptr;
    ID3D11PixelShader*        PixelShader  = nullptr;
    ID3D11InputLayout*        InputLayout  = nullptr;
    ID3D11Buffer*             VertexBuffer = nullptr;
    ID3D11Buffer*             ConstantBuffer = nullptr;
    ID3D11BlendState*         BlendState   = nullptr;
    ID3D11SamplerState*       Sampler      = nullptr;

    ID3D11Texture2D*          FontTexture   = nullptr;
    ID3D11ShaderResourceView* FontTextureView = nullptr;

    ID3D11Texture2D*          WhiteTexture   = nullptr;
    ID3D11ShaderResourceView* WhiteTextureView = nullptr;

    size_t  VertexCountCapacity = 0;
    int     Width  = 1;
    int     Height = 1;
    bool    Initialized = false;
    uint32_t UploadedAtlasVersion = 0;

    void ReleaseTarget()
    {
        if (RenderTarget) { RenderTarget->Release(); RenderTarget = nullptr; }
    }

    bool CreateTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void**>(&backBuffer))))
        {
            return false;
        }

        const HRESULT result = Device->CreateRenderTargetView(backBuffer, nullptr, &RenderTarget);
        backBuffer->Release();
        return SUCCEEDED(result);
    }

    void ReleaseAll()
    {
        ReleaseTarget();
        if (VertexBuffer)     { VertexBuffer->Release();     VertexBuffer = nullptr; }
        if (ConstantBuffer)   { ConstantBuffer->Release();   ConstantBuffer = nullptr; }
        if (InputLayout)      { InputLayout->Release();      InputLayout = nullptr; }
        if (VertexShader)     { VertexShader->Release();     VertexShader = nullptr; }
        if (PixelShader)      { PixelShader->Release();      PixelShader = nullptr; }
        if (BlendState)       { BlendState->Release();       BlendState = nullptr; }
        if (Sampler)          { Sampler->Release();          Sampler = nullptr; }
        if (FontTextureView)  { FontTextureView->Release();  FontTextureView = nullptr; }
        if (FontTexture)      { FontTexture->Release();      FontTexture = nullptr; }
        if (WhiteTextureView) { WhiteTextureView->Release(); WhiteTextureView = nullptr; }
        if (WhiteTexture)     { WhiteTexture->Release();     WhiteTexture = nullptr; }
        if (SwapChain)        { SwapChain->Release();        SwapChain = nullptr; }
        if (Context)          { Context->Release();          Context = nullptr; }
        if (Device)           { Device->Release();           Device = nullptr; }
        Initialized = false;
    }

    bool CreateWhiteTexture()
    {
        const uint32_t white = 0xFFFFFFFFu;

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width  = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = &white;
        data.SysMemPitch = 4;

        if (FAILED(Device->CreateTexture2D(&desc, &data, &WhiteTexture))) return false;
        if (FAILED(Device->CreateShaderResourceView(WhiteTexture, nullptr, &WhiteTextureView)))
        {
            WhiteTexture->Release();
            WhiteTexture = nullptr;
            return false;
        }
        return true;
    }

    bool EnsureFontTexture(ZSlateFontAtlas* atlas)
    {
        if (!atlas) return false;

        const int width  = atlas->GetWidth();
        const int height = atlas->GetHeight();

        if (!FontTexture)
        {
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width  = static_cast<UINT>(width);
            desc.Height = static_cast<UINT>(height);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;

            if (FAILED(Device->CreateTexture2D(&desc, nullptr, &FontTexture))) return false;
            if (FAILED(Device->CreateShaderResourceView(FontTexture, nullptr, &FontTextureView)))
            {
                FontTexture->Release();
                FontTexture = nullptr;
                return false;
            }
            UploadedAtlasVersion = 0;
        }

        if (UploadedAtlasVersion != atlas->GetVersion())
        {
            const UINT pitch = static_cast<UINT>(width) * 4u;
            Context->UpdateSubresource(FontTexture, 0, nullptr, atlas->GetPixels(), pitch, 0);
            UploadedAtlasVersion = atlas->GetVersion();
            atlas->ClearDirty();
        }

        return true;
    }

    bool EnsureVertexBuffer(size_t vertexCount)
    {
        if (VertexCountCapacity >= vertexCount && VertexBuffer) return true;

        const size_t capacity = std::max<size_t>(4096, vertexCount * 2);

        if (VertexBuffer) { VertexBuffer->Release(); VertexBuffer = nullptr; }

        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth      = static_cast<UINT>(capacity * sizeof(FVertex));
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(Device->CreateBuffer(&desc, nullptr, &VertexBuffer))) return false;

        VertexCountCapacity = capacity;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ZSlateD3D11Renderer::ZSlateD3D11Renderer(HWND hwnd)
    : m_Impl(new Impl())
{
    m_Impl->Hwnd = hwnd;
}

ZSlateD3D11Renderer::~ZSlateD3D11Renderer()
{
    Shutdown();
    delete m_Impl;
    m_Impl = nullptr;
}

bool ZSlateD3D11Renderer::Init()
{
    if (m_Impl->Initialized || !m_Impl->Hwnd) return false;

    RECT clientRect {};
    GetClientRect(m_Impl->Hwnd, &clientRect);
    m_Impl->Width  = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    m_Impl->Height = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));

    DXGI_SWAP_CHAIN_DESC swapChainDesc {};
    swapChainDesc.BufferCount  = 2;
    swapChainDesc.BufferDesc.Width  = static_cast<UINT>(m_Impl->Width);
    swapChainDesc.BufferDesc.Height = static_cast<UINT>(m_Impl->Height);
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator   = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = m_Impl->Hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    swapChainDesc.Flags = 0;

    D3D_FEATURE_LEVEL featureLevel {};
    const HRESULT createResult = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &swapChainDesc, &m_Impl->SwapChain, &m_Impl->Device,
        &featureLevel, &m_Impl->Context);

    if (FAILED(createResult) || !m_Impl->Device || !m_Impl->SwapChain)
    {
        m_Impl->ReleaseAll();
        return false;
    }

    if (!m_Impl->CreateTarget())
    {
        m_Impl->ReleaseAll();
        return false;
    }

    // ---- shaders ------------------------------------------------------------
    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob  = nullptr;
    ID3DBlob* errorBlob  = nullptr;

    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr,
                          nullptr, nullptr, "VSMain", "vs_4_0", 0, 0,
                          &vertexBlob, &errorBlob)))
    {
        if (errorBlob) errorBlob->Release();
        m_Impl->ReleaseAll();
        return false;
    }
    if (errorBlob) { errorBlob->Release(); errorBlob = nullptr; }

    if (FAILED(D3DCompile(kShaderSource, std::strlen(kShaderSource), nullptr,
                          nullptr, nullptr, "PSMain", "ps_4_0", 0, 0,
                          &pixelBlob, &errorBlob)))
    {
        if (errorBlob) errorBlob->Release();
        if (vertexBlob) vertexBlob->Release();
        m_Impl->ReleaseAll();
        return false;
    }
    if (errorBlob) { errorBlob->Release(); errorBlob = nullptr; }

    if (FAILED(m_Impl->Device->CreateVertexShader(vertexBlob->GetBufferPointer(),
                                                  vertexBlob->GetBufferSize(),
                                                  nullptr, &m_Impl->VertexShader)) ||
        FAILED(m_Impl->Device->CreatePixelShader(pixelBlob->GetBufferPointer(),
                                                 pixelBlob->GetBufferSize(),
                                                 nullptr, &m_Impl->PixelShader)))
    {
        vertexBlob->Release();
        pixelBlob->Release();
        m_Impl->ReleaseAll();
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layoutDesc[3] = {};
    layoutDesc[0].SemanticName = "POSITION";
    layoutDesc[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    layoutDesc[0].AlignedByteOffset = offsetof(FVertex, x);
    layoutDesc[1].SemanticName = "TEXCOORD";
    layoutDesc[1].SemanticIndex = 0;
    layoutDesc[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    layoutDesc[1].AlignedByteOffset = offsetof(FVertex, u);
    layoutDesc[2].SemanticName = "COLOR";
    layoutDesc[2].SemanticIndex = 0;
    layoutDesc[2].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    layoutDesc[2].AlignedByteOffset = offsetof(FVertex, r);

    const HRESULT layoutResult = m_Impl->Device->CreateInputLayout(
        layoutDesc, 3, vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
        &m_Impl->InputLayout);

    vertexBlob->Release();
    pixelBlob->Release();

    if (FAILED(layoutResult))
    {
        m_Impl->ReleaseAll();
        return false;
    }

    // ---- constant buffer ----------------------------------------------------
    D3D11_BUFFER_DESC constantDesc {};
    constantDesc.ByteWidth = sizeof(FViewportConstants);
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_Impl->Device->CreateBuffer(&constantDesc, nullptr, &m_Impl->ConstantBuffer)))
    {
        m_Impl->ReleaseAll();
        return false;
    }

    // ---- blend state --------------------------------------------------------
    D3D11_BLEND_DESC blendDesc {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_Impl->Device->CreateBlendState(&blendDesc, &m_Impl->BlendState)))
    {
        m_Impl->ReleaseAll();
        return false;
    }

    // ---- sampler ------------------------------------------------------------
    D3D11_SAMPLER_DESC samplerDesc {};
    samplerDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(m_Impl->Device->CreateSamplerState(&samplerDesc, &m_Impl->Sampler)))
    {
        m_Impl->ReleaseAll();
        return false;
    }

    if (!m_Impl->CreateWhiteTexture())
    {
        m_Impl->ReleaseAll();
        return false;
    }

    m_Impl->Initialized = true;
    return true;
}

void ZSlateD3D11Renderer::Shutdown()
{
    if (m_Impl) m_Impl->ReleaseAll();
}

bool ZSlateD3D11Renderer::IsInitialized() const
{
    return m_Impl && m_Impl->Initialized;
}

void ZSlateD3D11Renderer::Resize(int width, int height)
{
    if (!m_Impl->Initialized) return;

    const int w = std::max(1, width);
    const int h = std::max(1, height);
    if (w == m_Impl->Width && h == m_Impl->Height) return;

    m_Impl->Width  = w;
    m_Impl->Height = h;

    m_Impl->ReleaseTarget();
    m_Impl->SwapChain->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h),
                                     DXGI_FORMAT_UNKNOWN, 0);
    m_Impl->CreateTarget();
}

void ZSlateD3D11Renderer::Render(ZSlateBatchedRenderer& renderer, ZSlateFontAtlas* fontAtlas)
{
    if (!m_Impl->Initialized || !m_Impl->RenderTarget) return;

    const std::vector<FDrawCommand>& commands = renderer.GetCommands();

    // ---- build the vertex stream -------------------------------------------
    std::vector<FVertex> vertices;
    vertices.reserve(commands.size() * 6);

    size_t glyphStart = 0;
    const UIRect solidUV(-1.0f, -1.0f, 0.0f, 0.0f);

    for (const FDrawCommand& command : commands)
    {
        if (command.Type == EDrawCommandType::Quad)
        {
            PushQuad(vertices, command.Rect, command.Color, solidUV);
        }
    }
    glyphStart = vertices.size();

    for (const FDrawCommand& command : commands)
    {
        if (command.Type == EDrawCommandType::Glyph)
        {
            PushQuad(vertices, command.Rect, command.Color, command.UVRect);
        }
    }

    // ---- font texture -------------------------------------------------------
    const bool hasFont = m_Impl->EnsureFontTexture(fontAtlas);

    // ---- viewport constants -------------------------------------------------
    {
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (SUCCEEDED(m_Impl->Context->Map(m_Impl->ConstantBuffer, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            auto* constants = static_cast<FViewportConstants*>(mapped.pData);
            constants->Width  = static_cast<float>(m_Impl->Width);
            constants->Height = static_cast<float>(m_Impl->Height);
            constants->Pad0   = 0.0f;
            constants->Pad1   = 0.0f;
            m_Impl->Context->Unmap(m_Impl->ConstantBuffer, 0);
        }
    }

    // ---- upload vertices ----------------------------------------------------
    if (!vertices.empty())
    {
        if (!m_Impl->EnsureVertexBuffer(vertices.size())) return;

        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (SUCCEEDED(m_Impl->Context->Map(m_Impl->VertexBuffer, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(FVertex));
            m_Impl->Context->Unmap(m_Impl->VertexBuffer, 0);
        }
    }

    // ---- draw ---------------------------------------------------------------
    D3D11_VIEWPORT viewport {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width    = static_cast<float>(m_Impl->Width);
    viewport.Height   = static_cast<float>(m_Impl->Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    const float clearColor[4] = { 0.067f, 0.075f, 0.090f, 1.0f };

    m_Impl->Context->OMSetRenderTargets(1, &m_Impl->RenderTarget, nullptr);
    m_Impl->Context->RSSetViewports(1, &viewport);
    m_Impl->Context->ClearRenderTargetView(m_Impl->RenderTarget, clearColor);

    m_Impl->Context->OMSetBlendState(m_Impl->BlendState, nullptr, 0xFFFFFFFF);
    m_Impl->Context->VSSetShader(m_Impl->VertexShader, nullptr, 0);
    m_Impl->Context->PSSetShader(m_Impl->PixelShader, nullptr, 0);
    m_Impl->Context->IASetInputLayout(m_Impl->InputLayout);
    m_Impl->Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_Impl->Context->VSSetConstantBuffers(0, 1, &m_Impl->ConstantBuffer);
    m_Impl->Context->PSSetSamplers(0, 1, &m_Impl->Sampler);

    const UINT stride = static_cast<UINT>(sizeof(FVertex));
    const UINT offset = 0;
    m_Impl->Context->IASetVertexBuffers(0, 1, &m_Impl->VertexBuffer, &stride, &offset);

    if (glyphStart > 0)
    {
        m_Impl->Context->PSSetShaderResources(0, 1, &m_Impl->WhiteTextureView);
        m_Impl->Context->Draw(static_cast<UINT>(glyphStart), 0);
    }

    if (vertices.size() > glyphStart && hasFont)
    {
        ID3D11ShaderResourceView* view = m_Impl->FontTextureView
            ? m_Impl->FontTextureView : m_Impl->WhiteTextureView;
        m_Impl->Context->PSSetShaderResources(0, 1, &view);
        m_Impl->Context->Draw(static_cast<UINT>(vertices.size() - glyphStart),
                              static_cast<UINT>(glyphStart));
    }

    m_Impl->SwapChain->Present(1, 0);
}

} // namespace ZSlate
