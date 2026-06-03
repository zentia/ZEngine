#include "Runtime/Function/Render/Interface/DX12/Utility/DX12CubemapMipGen.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"

#include <algorithm>
#include <d3dcompiler.h>

namespace
{
    bool CheckDX12(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            LOG_ERROR(ZRender, "{} HRESULT=0x{:08X}", message, static_cast<unsigned int>(result));
            return false;
        }
        return true;
    }

    constexpr const char* kMipGenVertHlsl = R"(
struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint vertex_id : SV_VertexID)
{
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    VSOutput output;
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv       = uv;
    return output;
}
)";

    constexpr const char* kMipGenPsHlsl = R"(
Texture2D<float4> SrcTexture : register(t0);
SamplerState      LinearClamp : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target0
{
    return SrcTexture.Sample(LinearClamp, uv);
}
)";

    uint32_t CubemapSubresourceIndex(uint32_t mip, uint32_t face, uint32_t mip_levels)
    {
        return mip + face * mip_levels;
    }

    uint32_t MipDimension(uint32_t base, uint32_t mip_level)
    {
        uint32_t dim = base;
        for (uint32_t i = 0; i < mip_level; ++i)
        {
            dim = std::max(1u, dim / 2u);
        }
        return dim;
    }

    void TransitionSubresourceOnList(DX12Image* image,
                                     uint32_t subresource,
                                     D3D12_RESOURCE_STATES new_state,
                                     ID3D12GraphicsCommandList* command_list)
    {
        if (image == nullptr || image->getResource() == nullptr || command_list == nullptr)
        {
            return;
        }

        const D3D12_RESOURCE_STATES old_state = image->getState(subresource);
        if (old_state == new_state)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = image->getResource();
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = old_state;
        barrier.Transition.StateAfter = new_state;
        command_list->ResourceBarrier(1, &barrier);
        image->setState(subresource, new_state);
    }
}  // namespace

bool DX12CubemapMipGenerator::EnsureInitialized(ID3D12Device* device, DXGI_FORMAT format)
{
    if (m_Ready && m_Format == format)
    {
        return true;
    }
    if (m_Ready)
    {
        Shutdown();
    }
    if (device == nullptr || format == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    if (!CompilePipeline(device, format))
    {
        return false;
    }
    if (!CreateDescriptorHeaps(device))
    {
        Shutdown();
        return false;
    }

    m_Format = format;
    m_Ready = true;
    LOG_INFO(ZRender, "DX12CubemapMipGenerator: ready (format={})", static_cast<uint32_t>(format));
    return true;
}

void DX12CubemapMipGenerator::Shutdown()
{
    m_PipelineState.Reset();
    m_RootSignature.Reset();
    m_SrvHeap.Reset();
    m_RtvHeap.Reset();
    m_Format = DXGI_FORMAT_UNKNOWN;
    m_SrvDescriptorSize = 0;
    m_RtvDescriptorSize = 0;
    m_Ready = false;
}

bool DX12CubemapMipGenerator::CompilePipeline(ID3D12Device* device, DXGI_FORMAT format)
{
    DX12ShaderCompiler compiler;
    const DX12ShaderCompileResult vs_result =
        compiler.CompileFromSource(kMipGenVertHlsl,
                                   ShaderStage::Vertex,
                                   "dx12_cubemap_mipgen.vert",
                                   {},
                                   {},
                                   "main",
                                   "vs_6_0",
                                   "");
    if (!vs_result.success)
    {
        LOG_ERROR(ZRender, "DX12CubemapMipGenerator: VS compile failed: {}", vs_result.error_message);
        return false;
    }

    const DX12ShaderCompileResult ps_result =
        compiler.CompileFromSource(kMipGenPsHlsl,
                                   ShaderStage::Fragment,
                                   "dx12_cubemap_mipgen.frag",
                                   {},
                                   {},
                                   "main",
                                   "ps_6_0",
                                   "");
    if (!ps_result.success)
    {
        LOG_ERROR(ZRender, "DX12CubemapMipGenerator: PS compile failed: {}", ps_result.error_message);
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srv_range = {};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_parameter = {};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_parameter.DescriptorTable.NumDescriptorRanges = 1;
    root_parameter.DescriptorTable.pDescriptorRanges = &srv_range;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_parameter;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature_blob;
    ComPtr<ID3DBlob> signature_error;
    if (FAILED(D3D12SerializeRootSignature(&root_desc,
                                           D3D_ROOT_SIGNATURE_VERSION_1,
                                           signature_blob.GetAddressOf(),
                                           signature_error.GetAddressOf())))
    {
        if (signature_error)
        {
            LOG_ERROR(ZRender,
                      "DX12CubemapMipGenerator: SerializeRootSignature failed: {}",
                      static_cast<const char*>(signature_error->GetBufferPointer()));
        }
        return false;
    }

    if (!CheckDX12(device->CreateRootSignature(0,
                                               signature_blob->GetBufferPointer(),
                                               signature_blob->GetBufferSize(),
                                               IID_PPV_ARGS(&m_RootSignature)),
                   "DX12CubemapMipGenerator: CreateRootSignature failed"))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = m_RootSignature.Get();
    pso_desc.VS = {vs_result.dxil_code.data(), vs_result.dxil_code.size()};
    pso_desc.PS = {ps_result.dxil_code.data(), ps_result.dxil_code.size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.MultisampleEnable = FALSE;
    pso_desc.RasterizerState.AntialiasedLineEnable = FALSE;
    pso_desc.RasterizerState.ForcedSampleCount = 0;
    pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        pso_desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = format;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleDesc.Quality = 0;

    if (!CheckDX12(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&m_PipelineState)),
                   "DX12CubemapMipGenerator: CreateGraphicsPipelineState failed"))
    {
        m_RootSignature.Reset();
        return false;
    }
    return true;
}

bool DX12CubemapMipGenerator::CreateDescriptorHeaps(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.NumDescriptors = 1;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!CheckDX12(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&m_SrvHeap)),
                   "DX12CubemapMipGenerator: CreateDescriptorHeap(SRV) failed"))
    {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.NumDescriptors = 1;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (!CheckDX12(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&m_RtvHeap)),
                   "DX12CubemapMipGenerator: CreateDescriptorHeap(RTV) failed"))
    {
        m_SrvHeap.Reset();
        return false;
    }

    m_SrvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_RtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool DX12CubemapMipGenerator::GenerateMipChain(ID3D12GraphicsCommandList* command_list, DX12Image* image)
{
    if (!m_Ready || command_list == nullptr || image == nullptr || image->getResource() == nullptr)
    {
        return false;
    }

    const uint32_t mip_levels = image->getMipLevels();
    const uint32_t array_layers = image->getArrayLayers();
    if (mip_levels <= 1 || array_layers != 6)
    {
        return true;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(image->getResource()->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        return false;
    }

    const DXGI_FORMAT format = image->getFormat();
    if (!EnsureInitialized(device.Get(), format))
    {
        return false;
    }

    const uint32_t base_width = image->getWidth();
    const uint32_t base_height = image->getHeight();

    ID3D12DescriptorHeap* heaps[] = {m_SrvHeap.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetGraphicsRootSignature(m_RootSignature.Get());
    command_list->SetPipelineState(m_PipelineState.Get());
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32_t mip = 1; mip < mip_levels; ++mip)
    {
        const uint32_t dst_width = MipDimension(base_width, mip);
        const uint32_t dst_height = MipDimension(base_height, mip);

        for (uint32_t face = 0; face < 6; ++face)
        {
            const uint32_t src_subresource = CubemapSubresourceIndex(mip - 1, face, mip_levels);
            const uint32_t dst_subresource = CubemapSubresourceIndex(mip, face, mip_levels);

            TransitionSubresourceOnList(image,
                                        src_subresource,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                        command_list);
            TransitionSubresourceOnList(image,
                                        dst_subresource,
                                        D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        command_list);

            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = format;
            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Texture2DArray.MostDetailedMip = mip - 1;
            srv_desc.Texture2DArray.MipLevels = 1;
            srv_desc.Texture2DArray.FirstArraySlice = face;
            srv_desc.Texture2DArray.ArraySize = 1;
            srv_desc.Texture2DArray.PlaneSlice = 0;
            srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(image->getResource(), &srv_desc, srv_cpu);

            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
            rtv_desc.Format = format;
            rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv_desc.Texture2DArray.MipSlice = mip;
            rtv_desc.Texture2DArray.FirstArraySlice = face;
            rtv_desc.Texture2DArray.ArraySize = 1;
            rtv_desc.Texture2DArray.PlaneSlice = 0;
            device->CreateRenderTargetView(image->getResource(), &rtv_desc, rtv_cpu);

            D3D12_VIEWPORT viewport = {};
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width = static_cast<float>(dst_width);
            viewport.Height = static_cast<float>(dst_height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;

            D3D12_RECT scissor = {0, 0, static_cast<LONG>(dst_width), static_cast<LONG>(dst_height)};

            command_list->OMSetRenderTargets(1, &rtv_cpu, FALSE, nullptr);
            command_list->RSSetViewports(1, &viewport);
            command_list->RSSetScissorRects(1, &scissor);
            command_list->SetGraphicsRootDescriptorTable(0, srv_gpu);
            command_list->DrawInstanced(3, 1, 0, 0);

            TransitionSubresourceOnList(image,
                                        dst_subresource,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                        command_list);
        }
    }

    return true;
}
