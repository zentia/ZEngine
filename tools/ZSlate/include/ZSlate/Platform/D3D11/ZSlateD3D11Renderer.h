#pragma once

// ============================================================================
// ZSlate/Platform/D3D11/ZSlateD3D11Renderer.h
//
// The D3D11 backend: owns the swap chain and turns the batched draw list into
// GPU work.
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../Core/ZSlateTypes.h"
#include "../../Renderer/ZSlateBatchedRenderer.h"
#include "../../Renderer/ZSlateFontAtlas.h"

namespace ZSlate
{

class ZSlateD3D11Renderer
{
public:
    explicit ZSlateD3D11Renderer(HWND hwnd);
    ~ZSlateD3D11Renderer();

    ZSlateD3D11Renderer(const ZSlateD3D11Renderer&)            = delete;
    ZSlateD3D11Renderer& operator=(const ZSlateD3D11Renderer&) = delete;

    bool Init();
    void Shutdown();
    bool IsInitialized() const;

    void Resize(int width, int height);

    // Submits one frame. `fontAtlas` may be null (text then renders as boxes).
    void Render(ZSlateBatchedRenderer& renderer, ZSlateFontAtlas* fontAtlas);

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

} // namespace ZSlate
