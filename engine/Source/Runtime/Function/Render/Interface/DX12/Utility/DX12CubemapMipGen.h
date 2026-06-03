#pragma once

#include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// GPU linear downsample for cubemap array textures (one face + mip at a time).
// Used during IBL cubemap upload after mip 0 is copied via staging buffers.
class DX12CubemapMipGenerator
{
public:
    bool EnsureInitialized(ID3D12Device* device, DXGI_FORMAT format);
    bool GenerateMipChain(ID3D12GraphicsCommandList* command_list, DX12Image* image);
    void Shutdown();

private:
    bool CompilePipeline(ID3D12Device* device, DXGI_FORMAT format);
    bool CreateDescriptorHeaps(ID3D12Device* device);

    ComPtr<ID3D12RootSignature> m_RootSignature;
    ComPtr<ID3D12PipelineState> m_PipelineState;
    ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    DXGI_FORMAT m_Format {DXGI_FORMAT_UNKNOWN};
    UINT m_SrvDescriptorSize {0};
    UINT m_RtvDescriptorSize {0};
    bool m_Ready {false};
};
