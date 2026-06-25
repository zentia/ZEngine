// =============================================================================
// TexPreviewWindow.cpp — ZSlate + D3D11 texture preview (no ZRuntime)
// =============================================================================

#include "TexPreviewWindow.h"
#undef DrawText  // restored by TexPreviewWindow.h, but .cpp needs its own guard

#include "ZSlate/Core/SlateReply.h"
#include "Runtime/Function/Render/Texture/ASTCDecompressor.h"
#include "Runtime/Function/Render/Texture/BC7Decompressor.h"
#include "Runtime/Function/Render/Texture/ETC2Decompressor.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
    #include <commdlg.h>
#endif

// ---- Logging (no Runtime/Macro.h) -------------------------------------------
#define TEX_LOG_ERR(fmt, ...) std::fprintf(stderr, "[TexPreview] " fmt "\n", ##__VA_ARGS__)
#define TEX_LOG_INF(fmt, ...) std::fprintf(stdout, "[TexPreview] " fmt "\n", ##__VA_ARGS__)

namespace
{
    const char* FormatToString(TexPreviewFormat fmt)
    {
        switch (fmt) {
        case TexPreviewFormat::ASTC: return "ASTC";
        case TexPreviewFormat::BC7:  return "BC7";
        case TexPreviewFormat::ETC2: return "ETC2";
        default:                     return "Unknown";
        }
    }
}

// ---- Constructor / Destructor -----------------------------------------------

TexPreviewWindow::TexPreviewWindow()
    : m_ZoomLevel(1.0f), m_MinZoom(0.1f), m_MaxZoom(10.0f)
    , m_IsPanning(false), m_PanX(0.0f), m_PanY(0.0f)
    , m_LastMouseX(0.0f), m_LastMouseY(0.0f)
    , m_NeedsDecompress(true), m_TextureLoaded(false)
    , m_NeedsTextureCreate(true), m_TextureId(nullptr)
{
    Visibility = ZSlate::EVisibility::Visible;
}

TexPreviewWindow::~TexPreviewWindow()
{
#ifdef _WIN32
    ReleaseGPUTexture();
#endif
}

void TexPreviewWindow::ReleaseGPUTexture()
{
#ifdef _WIN32
    if (m_TextureSRV) { m_TextureSRV->Release(); m_TextureSRV = nullptr; }
    if (m_TextureTex) { m_TextureTex->Release(); m_TextureTex = nullptr; }
#endif
    m_TextureId = nullptr;
    m_NeedsTextureCreate = true;
}

// ---- SetTexture --------------------------------------------------------------

void TexPreviewWindow::SetTexture(const std::filesystem::path& texture_path)
{
    m_TexturePath = texture_path;
    m_DetectedFormat = DetectFormat(texture_path);
    m_NeedsDecompress = true;
    m_TextureLoaded = false;
    m_NeedsTextureCreate = true;
    m_PanX = m_PanY = 0.0f;
    m_ZoomLevel = 1.0f;

    ReleaseGPUTexture();

    if (DecompressTexture())
    {
        m_TextureLoaded = true;
        m_NeedsTextureCreate = true;
    }
    else
    {
        TEX_LOG_ERR("failed to decompress texture");
    }
}

// ---- SWidget overrides ------------------------------------------------------

ZSlate::Vector2 TexPreviewWindow::ComputeDesiredSize() const
{
    return ZSlate::Vector2(1024.0f, 768.0f);
}

void TexPreviewWindow::OnPaint(const ZSlate::FPaintContext& ctx,
                                const ZSlate::FGeometry& geom) const
{
    if (!ctx.Renderer) return;
    const ZSlate::UIRect rect = geom.ToRect();

    ctx.Renderer->DrawQuad(rect, ZSlate::UIColor(0.2f, 0.2f, 0.2f, 1.0f));
    DrawCheckerboard(ctx.Renderer, rect);
    DrawPreviewImage(ctx.Renderer, rect);
    DrawInfoOverlay(ctx.Renderer, rect);
}

void TexPreviewWindow::OnMouseMove(const ZSlate::Vector2& screen_pos)
{
    if (m_IsPanning) {
        m_PanX += (screen_pos.x - m_LastMouseX);
        m_PanY += (screen_pos.y - m_LastMouseY);
        m_LastMouseX = screen_pos.x;
        m_LastMouseY = screen_pos.y;
    }
}

ZSlate::FReply TexPreviewWindow::OnMouseButtonDown(const ZSlate::Vector2& pos, int btn)
{
    if (btn == 2) { m_IsPanning = true; m_LastMouseX = pos.x; m_LastMouseY = pos.y; return ZSlate::FReply::Handled(); }
    return ZSlate::FReply::Unhandled();
}

ZSlate::FReply TexPreviewWindow::OnMouseButtonUp(const ZSlate::Vector2&, int btn)
{
    if (btn == 2 && m_IsPanning) { m_IsPanning = false; return ZSlate::FReply::Handled(); }
    return ZSlate::FReply::Unhandled();
}

ZSlate::FReply TexPreviewWindow::OnMouseWheel(const ZSlate::Vector2&, float delta)
{
    const float old = m_ZoomLevel;
    m_ZoomLevel = std::clamp(m_ZoomLevel * (delta > 0 ? 1.1f : 0.9f), m_MinZoom, m_MaxZoom);
    if (m_ZoomLevel != old) { const float s = m_ZoomLevel / old; m_PanX *= s; m_PanY *= s; }
    return ZSlate::FReply::Handled();
}

// ---- Format detection -------------------------------------------------------

TexPreviewFormat TexPreviewWindow::DetectFormat(const std::filesystem::path& path) const
{
    if (!std::filesystem::exists(path)) return TexPreviewFormat::Unknown;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return TexPreviewFormat::Unknown;

    uint8_t magic[4] = {};
    file.read(reinterpret_cast<char*>(magic), 4);

    const uint32_t m32 = magic[0] | (magic[1] << 8) | (magic[2] << 16) | (magic[3] << 24);
    if (m32 == 0x5CA1AB13 || m32 == 0x00000004) return TexPreviewFormat::ASTC;

    if (m32 == 0x20534444) {
        uint8_t buf[132] = {}; file.seekg(0); file.read(reinterpret_cast<char*>(buf), 132);
        if (file.gcount() >= 132) {
            uint32_t pf = buf[80]|(buf[81]<<8)|(buf[82]<<16)|(buf[83]<<24);
            if (pf & 0x4) {
                uint32_t fc = buf[84]|(buf[85]<<8)|(buf[86]<<16)|(buf[87]<<24);
                if (fc == 0x30315844) {
                    uint32_t fmt = buf[128]|(buf[129]<<8)|(buf[130]<<16)|(buf[131]<<24);
                    if (fmt == 98 || fmt == 99) return TexPreviewFormat::BC7;
                }
            }
        }
    }

    if (magic[0]==0x50 && magic[1]==0x4B && magic[2]==0x4D && magic[3]==0x20) {
        uint8_t buf[16]={}; file.seekg(0); file.read(reinterpret_cast<char*>(buf),16);
        if (file.gcount()>=16 && buf[4]==0x31 && buf[5]==0x30) return TexPreviewFormat::ETC2;
    }

    auto ext = path.extension();
    if (ext == ".astc" || ext == ".ASTC") return TexPreviewFormat::ASTC;
    if (ext == ".bc7"  || ext == ".BC7")  return TexPreviewFormat::BC7;
    if (ext == ".pkm"  || ext == ".PKM")  return TexPreviewFormat::ETC2;
    return TexPreviewFormat::Unknown;
}

// ---- Decompression (ASTC/BC7/ETC2 — same logic, just LOG→TEX_LOG) -----------

bool TexPreviewWindow::DecompressTexture()
{
    if (m_TexturePath.empty() || !std::filesystem::exists(m_TexturePath))
    { TEX_LOG_ERR("texture path empty or file missing"); return false; }

    std::ifstream file(m_TexturePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { TEX_LOG_ERR("failed to open: %s", m_TexturePath.generic_string().c_str()); return false; }
    const size_t sz = (size_t)file.tellg(); file.seekg(0);
    if (sz < 16) { TEX_LOG_ERR("file too small"); return false; }

    std::vector<uint8_t> data(sz);
    file.read(reinterpret_cast<char*>(data.data()), sz); file.close();

    switch (m_DetectedFormat) {
    case TexPreviewFormat::ASTC: return DecompressASTC(data);
    case TexPreviewFormat::BC7:  return DecompressBC7(data);
    case TexPreviewFormat::ETC2: return DecompressETC2(data);
    default: TEX_LOG_ERR("unrecognized format"); return false;
    }
}

bool TexPreviewWindow::DecompressASTC(const std::vector<uint8_t>& data)
{
    const uint32_t magic = data[0]|(data[1]<<8)|(data[2]<<16)|(data[3]<<24);
    uint8_t bx, by; uint32_t w, h;
    if (magic == 0x5CA1AB13) { bx=data[4]; by=data[5]; w=data[7]|(data[8]<<8)|(data[9]<<16); h=data[10]|(data[11]<<8)|(data[12]<<16); }
    else if (magic == 0x00000004) { bx=data[4]; by=data[5]; w=data[6]|(data[7]<<8); h=data[8]|(data[9]<<8); }
    else { TEX_LOG_ERR("bad ASTC magic 0x%08X", magic); return false; }

    ZEngine::Render::ASTCBlockSize bs;
    if (bx==4&&by==4) bs=ZEngine::Render::ASTCBlockSize::ASTC_4x4;
    else if (bx==5&&by==5) bs=ZEngine::Render::ASTCBlockSize::ASTC_5x5;
    else if (bx==6&&by==6) bs=ZEngine::Render::ASTCBlockSize::ASTC_6x6;
    else if (bx==8&&by==8) bs=ZEngine::Render::ASTCBlockSize::ASTC_8x8;
    else if (bx==10&&by==10) bs=ZEngine::Render::ASTCBlockSize::ASTC_10x10;
    else if (bx==12&&by==12) bs=ZEngine::Render::ASTCBlockSize::ASTC_12x12;
    else { TEX_LOG_ERR("unsupported ASTC block %dx%d", bx, by); return false; }

    if (!ZEngine::Render::ASTCDecompressor::Initialize()) { TEX_LOG_ERR("ASTC init failed"); return false; }
    auto r = ZEngine::Render::ASTCDecompressor::Decompress(data.data()+16, data.size()-16, w, h, bs);
    if (!r.success) { TEX_LOG_ERR("ASTC decompress: %s", r.error_message.c_str()); return false; }

    m_PreviewWidth=r.width; m_PreviewHeight=r.height; m_PreviewPixels=std::move(r.pixels);
    m_NeedsDecompress=false; m_TextureLoaded=true; return true;
}

bool TexPreviewWindow::DecompressBC7(const std::vector<uint8_t>& data)
{
    if (data.size() < 148) { TEX_LOG_ERR("BC7 header too small"); return false; }
    const uint32_t m = data[0]|(data[1]<<8)|(data[2]<<16)|(data[3]<<24);
    if (m != 0x20534444) { TEX_LOG_ERR("bad DDS magic"); return false; }
    const uint32_t pf = data[80]|(data[81]<<8)|(data[82]<<16)|(data[83]<<24);
    if (!(pf & 0x4)) { TEX_LOG_ERR("not FOURCC"); return false; }
    const uint32_t fc = data[84]|(data[85]<<8)|(data[86]<<16)|(data[87]<<24);
    if (fc != 0x30315844) { TEX_LOG_ERR("not DX10"); return false; }
    const uint32_t fmt = data[128]|(data[129]<<8)|(data[130]<<16)|(data[131]<<24);
    if (fmt != 98 && fmt != 99) { TEX_LOG_ERR("not BC7"); return false; }

    const uint32_t w = data[16]|(data[17]<<8)|(data[18]<<16)|(data[19]<<24);
    const uint32_t h = data[12]|(data[13]<<8)|(data[14]<<16)|(data[15]<<24);
    const size_t cs = data.size()-148;
    if (!ZEngine::Render::BC7Decompressor::ValidateSize(w,h,cs)) { TEX_LOG_ERR("BC7 size mismatch"); return false; }

    auto r = ZEngine::Render::BC7Decompressor::Decompress(data.data()+148, cs, w, h);
    if (!r.success) { TEX_LOG_ERR("BC7 decompress: %s", r.error_message.c_str()); return false; }

    m_PreviewWidth=r.width; m_PreviewHeight=r.height; m_PreviewPixels=std::move(r.pixels);
    m_NeedsDecompress=false; m_TextureLoaded=true; return true;
}

bool TexPreviewWindow::DecompressETC2(const std::vector<uint8_t>& data)
{
    if (data.size()<16){TEX_LOG_ERR("ETC2 header too small");return false;}
    if (data[0]!=0x50||data[1]!=0x4B||data[2]!=0x4D||data[3]!=0x20){TEX_LOG_ERR("bad PKM magic");return false;}
    if (!(data[4]==0x31&&data[5]==0x30)){TEX_LOG_ERR("not ETC2");return false;}

    const uint32_t w=(data[12]<<8)|data[13], h=(data[14]<<8)|data[15];
    ZEngine::Render::ETC2Variant v;
    if (data[6]=='r'&&data[7]=='A') v=ZEngine::Render::ETC2Variant::RGBA8;
    else if (data[6]=='r'&&data[7]=='G') v=ZEngine::Render::ETC2Variant::RGBA1;
    else v=ZEngine::Render::ETC2Variant::RGB;
    const size_t cs=data.size()-16;
    if (!ZEngine::Render::ETC2Decompressor::ValidateSize(w,h,cs,v)){TEX_LOG_ERR("ETC2 size mismatch");return false;}

    auto r=ZEngine::Render::ETC2Decompressor::Decompress(data.data()+16, cs, w, h, v);
    if (!r.success){TEX_LOG_ERR("ETC2 decompress: %s",r.error_message.c_str());return false;}

    m_PreviewWidth=r.width; m_PreviewHeight=r.height; m_PreviewPixels=std::move(r.pixels);
    m_NeedsDecompress=false; m_TextureLoaded=true; return true;
}

// ---- GPU Texture (D3D11 directly, no UIGpuResources) ------------------------

bool TexPreviewWindow::CreateGPUTexture()
{
#ifdef _WIN32
    if (!m_TextureLoaded || m_PreviewPixels.empty()) return false;
    if (!m_D3DDevice) { TEX_LOG_ERR("no D3D11 device set"); return false; }

    // Release old texture
    if (m_TextureSRV) { m_TextureSRV->Release(); m_TextureSRV = nullptr; }
    if (m_TextureTex) { m_TextureTex->Release(); m_TextureTex = nullptr; }

    D3D11_TEXTURE2D_DESC td {};
    td.Width  = m_PreviewWidth;
    td.Height = m_PreviewHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd {};
    sd.pSysMem = m_PreviewPixels.data();
    sd.SysMemPitch = m_PreviewWidth * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_D3DDevice->CreateTexture2D(&td, &sd, &tex)) || !tex)
    { TEX_LOG_ERR("CreateTexture2D failed"); return false; }

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = m_D3DDevice->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) { srv->Release(); TEX_LOG_ERR("CreateSRV failed"); return false; }

    m_TextureSRV = srv;
    m_TextureId  = srv;  // ISlateRenderer::DrawTexturedQuad takes void* = SRV*
    m_NeedsTextureCreate = false;

    TEX_LOG_INF("GPU texture created (%ux%u, %s)", m_PreviewWidth, m_PreviewHeight, FormatToString(m_DetectedFormat));
    return true;
#else
    return false;
#endif
}

// ---- Drawing helpers --------------------------------------------------------

void TexPreviewWindow::DrawCheckerboard(ZSlate::ISlateRenderer* r,
                                         const ZSlate::UIRect& rect) const
{
    // Draw a checkerboard pattern for transparency visualization
    const float cellSize = 16.0f;
    const float mid = (rect.x + rect.w * 0.5f);
    const float top = rect.y;
    const float bot = rect.y + rect.h;

    // Clip drawing to the rect to avoid overdrawing the entire window
    for (float cy = rect.y; cy < bot; cy += cellSize)
    {
        float ch = std::min(cellSize, bot - cy);
        int rowIdx = int((cy - rect.y) / cellSize);
        for (float cx = rect.x; cx < rect.Right(); cx += cellSize)
        {
            float cw = std::min(cellSize, rect.Right() - cx);
            int colIdx = int((cx - rect.x) / cellSize);
            bool even = (rowIdx + colIdx) % 2 == 0;
            r->DrawQuad(ZSlate::UIRect(cx, cy, cw, ch),
                       even ? ZSlate::UIColor(0.22f, 0.22f, 0.22f, 1.0f)
                            : ZSlate::UIColor(0.17f, 0.17f, 0.17f, 1.0f));
        }
    }
}

void TexPreviewWindow::DrawPreviewImage(ZSlate::ISlateRenderer* renderer,
                                         const ZSlate::UIRect& rect) const
{
    if (!m_TextureLoaded) return;

    if (m_NeedsTextureCreate)
        const_cast<TexPreviewWindow*>(this)->CreateGPUTexture();

    if (!m_TextureId) return;

    const float iw = (float)m_PreviewWidth  * m_ZoomLevel;
    const float ih = (float)m_PreviewHeight * m_ZoomLevel;
    const float ix = rect.x + (rect.w - iw) / 2.0f + m_PanX;
    const float iy = rect.y + (rect.h - ih) / 2.0f + m_PanY;

    renderer->PushClipRect(rect);
    if (iw > 0.0f && ih > 0.0f)
        renderer->DrawTexturedQuad(ZSlate::UIRect(ix, iy, iw, ih), m_TextureId, ZSlate::Colors::White);
    renderer->PopClipRect();
}

void TexPreviewWindow::DrawInfoOverlay(ZSlate::ISlateRenderer* renderer,
                                        const ZSlate::UIRect& rect) const
{
    if (!m_TextureLoaded) return;

    char buf[192];
    std::snprintf(buf, sizeof(buf), "Format: %s  |  Size: %ux%u  |  Zoom: %.0f%%  |  Pan: %.0f, %.0f",
                  FormatToString(m_DetectedFormat), m_PreviewWidth, m_PreviewHeight,
                  m_ZoomLevel * 100.0f, m_PanX, m_PanY);

    renderer->DrawQuad(ZSlate::UIRect(rect.x, rect.y, rect.w, 28.0f),
                       ZSlate::UIColor(0.1f, 0.1f, 0.1f, 0.85f));
    renderer->DrawText(ZSlate::UIRect(rect.x + 8.0f, rect.y, rect.w - 16.0f, 28.0f),
                       buf, 13.0f, ZSlate::UIColor(0.9f, 0.9f, 0.9f, 1.0f),
                       ZSlate::TextAnchor::MiddleLeft, ZSlate::TextWrapMode::NoWrap);
}

// ---- Zoom / Action stubs ----------------------------------------------------

void TexPreviewWindow::OnZoomIn()  { m_ZoomLevel = std::min(m_ZoomLevel * 1.2f, m_MaxZoom); }
void TexPreviewWindow::OnZoomOut() { m_ZoomLevel = std::max(m_ZoomLevel / 1.2f, m_MinZoom); }
void TexPreviewWindow::OnFitToWindow() { m_ZoomLevel = 1.0f; m_PanX = m_PanY = 0.0f; }
void TexPreviewWindow::OnSaveAsPNG() {}
void TexPreviewWindow::OnReloadTexture()
{
    m_NeedsDecompress = true;
    if (DecompressTexture()) { m_TextureLoaded = true; m_NeedsTextureCreate = true; ReleaseGPUTexture(); }
}

void TexPreviewWindow::OnKeyChar(unsigned int c)
{
#ifdef _WIN32
    if (c == 'O' || c == 'o') {
        OPENFILENAMEA ofn {}; char path[MAX_PATH] {};
        ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = "Texture Files (*.astc;*.bc7;*.pkm)\0*.astc;*.bc7;*.pkm\0All (*.*)\0*.*\0";
        ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) SetTexture(std::filesystem::path(path));
        return;
    }
#endif
    if (c == 'R' || c == 'r') { OnReloadTexture(); return; }
    if (c == 'F' || c == 'f') { OnFitToWindow(); return; }
    if (c == '+') { OnZoomIn(); return; }
    if (c == '-') { OnZoomOut(); return; }
    if (c == 'S' || c == 's') { OnSaveAsPNG(); return; }
}
