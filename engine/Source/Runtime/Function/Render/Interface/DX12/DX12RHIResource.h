#pragma once

#include "Runtime/Function/Render/Interface/RHIStruct.h"

#include <cstdint>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class DX12Buffer;
class DX12Image;
class DX12ImageView;

enum class DX12DescriptorHeapKind
{
    CbvSrvUav,
    Sampler
};

struct DX12RootParameterBinding
{
    uint32_t set {0};
    uint32_t binding {0};
    DX12DescriptorHeapKind heap_kind {DX12DescriptorHeapKind::CbvSrvUav};
    uint32_t root_parameter_index {0};
};

class DX12DescriptorPool : public RHIDescriptorPool
{
};

class DX12DescriptorSetLayout : public RHIDescriptorSetLayout
{
public:
    void setBindings(const RHIDescriptorSetLayoutBinding* bindings, uint32_t count)
    {
        m_Bindings.assign(bindings, bindings + count);
    }

    const std::vector<RHIDescriptorSetLayoutBinding>& getBindings() const { return m_Bindings; }

private:
    std::vector<RHIDescriptorSetLayoutBinding> m_Bindings;
};

struct DX12DescriptorBufferBinding
{
    DX12Buffer* buffer {nullptr};
    RHIDeviceSize offset {0};
    RHIDeviceSize range {0};
    RHIDescriptorType type {static_cast<RHIDescriptorType>(0)};
};

class DX12DescriptorSet : public RHIDescriptorSet
{
public:
    void setLayout(DX12DescriptorSetLayout* layout) { m_Layout = layout; }
    DX12DescriptorSetLayout* getLayout() const { return m_Layout; }

    void setCbvSrvUavHandle(uint32_t binding, D3D12_GPU_DESCRIPTOR_HANDLE handle)
    {
        m_CbvSrvUavHandles[binding] = handle;
    }

    void setSamplerHandle(uint32_t binding, D3D12_GPU_DESCRIPTOR_HANDLE handle)
    {
        m_SamplerHandles[binding] = handle;
    }

    void setBufferBinding(uint32_t binding, DX12Buffer* buffer, RHIDeviceSize offset, RHIDeviceSize range, RHIDescriptorType type)
    {
        m_BufferBindings[binding] = {buffer, offset, range, type};
    }

    const DX12DescriptorBufferBinding* getBufferBinding(uint32_t binding) const
    {
        const auto iter = m_BufferBindings.find(binding);
        return iter != m_BufferBindings.end() ? &iter->second : nullptr;
    }

    bool getCbvSrvUavHandle(uint32_t binding, D3D12_GPU_DESCRIPTOR_HANDLE& handle) const
    {
        auto iter = m_CbvSrvUavHandles.find(binding);
        if (iter == m_CbvSrvUavHandles.end())
        {
            return false;
        }
        handle = iter->second;
        return true;
    }

    bool getSamplerHandle(uint32_t binding, D3D12_GPU_DESCRIPTOR_HANDLE& handle) const
    {
        auto iter = m_SamplerHandles.find(binding);
        if (iter == m_SamplerHandles.end())
        {
            return false;
        }
        handle = iter->second;
        return true;
    }

private:
    DX12DescriptorSetLayout* m_Layout {nullptr};
    std::map<uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE> m_CbvSrvUavHandles;
    std::map<uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE> m_SamplerHandles;
    std::map<uint32_t, DX12DescriptorBufferBinding> m_BufferBindings;
};

class DX12PipelineLayout : public RHIPipelineLayout
{
public:
    void setRootSignature(ComPtr<ID3D12RootSignature> root_signature,
                          std::vector<DX12RootParameterBinding> bindings)
    {
        m_RootSignature = root_signature;
        m_Bindings = std::move(bindings);
    }

    // PR6 (DX12 bindless wiring): if any descriptor set in this layout
    // declared a bindless binding (i.e. carried
    // RHI_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT in its
    // RHIDescriptorSetLayoutBinding::bindingFlags), the pipeline layout
    // is built with a bindless-aware D3D12 root signature: it ORs in
    // GetBindlessRootSignatureFlags() and reserves exactly one extra
    // 32-bit root constant at b0/space0 (the
    // "bindless-index-root-constant"). The bindless set itself does NOT
    // appear in m_Bindings (no descriptor table entry), so the legacy
    // cmdBindDescriptorSets path naturally skips it -- callers instead
    // push the packed (texture_index | sampler_index<<16) value via
    // RHI::cmdSetBindlessIndex(), which translates to
    // SetGraphicsRoot32BitConstant on this very root parameter.
    void setBindlessInfo(bool uses_bindless, uint32_t bindless_root_constant_index)
    {
        m_UsesBindless = uses_bindless;
        m_BindlessRootConstantIndex = bindless_root_constant_index;
    }

    ID3D12RootSignature* getRootSignature() const { return m_RootSignature.Get(); }
    const std::vector<DX12RootParameterBinding>& getBindings() const { return m_Bindings; }
    bool usesBindless() const { return m_UsesBindless; }
    uint32_t getBindlessRootConstantParameterIndex() const { return m_BindlessRootConstantIndex; }

private:
    ComPtr<ID3D12RootSignature> m_RootSignature;
    std::vector<DX12RootParameterBinding> m_Bindings;
    bool m_UsesBindless {false};
    uint32_t m_BindlessRootConstantIndex {0};
};

// Owned copy of one RHISubpassDescription (Vulkan create info uses stack pointers).
struct DX12SubpassDescriptionStorage
{
    RHISubpassDescription desc {};
    std::vector<RHIAttachmentReference> input_attachments;
    std::vector<RHIAttachmentReference> color_attachments;
    std::vector<RHIAttachmentReference> resolve_attachments;
    std::vector<uint32_t> preserve_attachments;
    bool has_depth_stencil {false};
    RHIAttachmentReference depth_stencil {};
};

class DX12RenderPass : public RHIRenderPass
{
public:
    // Legacy entry: attachments only (single implicit subpass).
    void setAttachments(const RHIAttachmentDescription* attachments, uint32_t count);

    // DX-B0: deep-copy the full Vulkan-style create info (subpasses + dependencies).
    bool setCreateInfo(const RHIRenderPassCreateInfo* create_info);

    const std::vector<RHIAttachmentDescription>& getAttachments() const { return m_Attachments; }
    uint32_t getSubpassCount() const
    {
        return static_cast<uint32_t>(m_Subpasses.size());
    }
    const RHISubpassDescription* getSubpass(uint32_t index) const;
    const std::vector<RHISubpassDependency>& getDependencies() const { return m_Dependencies; }

private:
    void rebuildImplicitSubpassFromAttachments();

    std::vector<RHIAttachmentDescription> m_Attachments;
    std::vector<DX12SubpassDescriptionStorage> m_Subpasses;
    std::vector<RHISubpassDependency> m_Dependencies;
};

class DX12ImageView;

class DX12Framebuffer : public RHIFramebuffer
{
public:
    void SetSize(uint32_t width, uint32_t height, uint32_t layers)
    {
        m_Width = width;
        m_Height = height;
        m_Layers = layers;
    }

    void setRenderPass(DX12RenderPass* render_pass) { m_RenderPass = render_pass; }
    DX12RenderPass* GetRenderPass() const { return m_RenderPass; }

    void addRtv(D3D12_CPU_DESCRIPTOR_HANDLE handle, DX12ImageView* image_view)
    {
        m_Rtvs.push_back(handle);
        m_RtvViews.push_back(image_view);
    }
    void setDsv(D3D12_CPU_DESCRIPTOR_HANDLE handle, DX12ImageView* image_view)
    {
        m_Dsv = handle;
        m_DsvView = image_view;
        m_HasDsv = true;
    }

    const std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& getRtvs() const { return m_Rtvs; }
    const std::vector<DX12ImageView*>& getRtvViews() const { return m_RtvViews; }
    bool hasDsv() const { return m_HasDsv; }
    D3D12_CPU_DESCRIPTOR_HANDLE getDsv() const { return m_Dsv; }
    DX12ImageView* getDsvView() const { return m_DsvView; }
    uint32_t getWidth() const { return m_Width; }
    uint32_t getHeight() const { return m_Height; }

    // DX-B0: attachment-indexed views (matches RHIRenderPass attachment order).
    void setAttachmentView(uint32_t attachment_index, DX12ImageView* view);
    DX12ImageView* getAttachmentView(uint32_t attachment_index) const;

private:
    DX12RenderPass* m_RenderPass {nullptr};
    std::vector<DX12ImageView*> m_AttachmentViews;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> m_Rtvs;
    std::vector<DX12ImageView*> m_RtvViews;
    D3D12_CPU_DESCRIPTOR_HANDLE m_Dsv {};
    DX12ImageView* m_DsvView {nullptr};
    bool m_HasDsv {false};
    uint32_t m_Width {0};
    uint32_t m_Height {0};
    uint32_t m_Layers {1};
};

class DX12Pipeline : public RHIPipeline
{
public:
    void setPipelineState(ComPtr<ID3D12PipelineState> pipeline_state) { m_PipelineState = pipeline_state; }
    ID3D12PipelineState* getPipelineState() const { return m_PipelineState.Get(); }

    void setPipelineLayout(DX12PipelineLayout* layout) { m_Layout = layout; }
    DX12PipelineLayout* getPipelineLayout() const { return m_Layout; }

    void setPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) { m_PrimitiveTopology = topology; }
    D3D12_PRIMITIVE_TOPOLOGY getPrimitiveTopology() const { return m_PrimitiveTopology; }

    void setVertexStrides(std::vector<uint32_t> strides) { m_VertexStrides = std::move(strides); }
    uint32_t getVertexStride(uint32_t binding) const
    {
        return binding < m_VertexStrides.size() ? m_VertexStrides[binding] : 0;
    }

private:
    ComPtr<ID3D12PipelineState> m_PipelineState;
    DX12PipelineLayout* m_Layout {nullptr};
    D3D12_PRIMITIVE_TOPOLOGY m_PrimitiveTopology {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
    std::vector<uint32_t> m_VertexStrides;
};

class DX12Buffer : public RHIBuffer
{
public:
    void setResource(ComPtr<ID3D12Resource> resource, RHIDeviceSize size)
    {
        m_Resource = resource;
        m_Size = size;
    }

    ID3D12Resource* getResource() const { return m_Resource.Get(); }
    RHIDeviceSize getSize() const { return m_Size; }

private:
    ComPtr<ID3D12Resource> m_Resource;
    RHIDeviceSize m_Size {0};
};

class DX12DeviceMemory : public RHIDeviceMemory
{
public:
    void setResource(ComPtr<ID3D12Resource> resource, D3D12_HEAP_TYPE heap_type)
    {
        m_Resource = resource;
        m_HeapType = heap_type;
    }

    ID3D12Resource* getResource() const { return m_Resource.Get(); }
    D3D12_HEAP_TYPE getHeapType() const { return m_HeapType; }
    void* getMappedData() const { return m_MappedData; }
    void setMappedData(void* data) { m_MappedData = data; }

private:
    ComPtr<ID3D12Resource> m_Resource;
    D3D12_HEAP_TYPE m_HeapType {D3D12_HEAP_TYPE_DEFAULT};
    void* m_MappedData {nullptr};
};

class DX12Image : public RHIImage
{
public:
    void setResource(ComPtr<ID3D12Resource> resource,
                     DXGI_FORMAT format,
                     uint32_t width,
                     uint32_t height,
                     uint32_t array_layers,
                     uint32_t mip_levels,
                     D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON)
    {
        m_Resource = resource;
        m_Format = format;
        m_Width = width;
        m_Height = height;
        m_ArrayLayers = array_layers;
        m_MipLevels = mip_levels;
        m_States.assign(static_cast<size_t>(array_layers) * static_cast<size_t>(mip_levels), initial_state);
    }

    ID3D12Resource* getResource() const { return m_Resource.Get(); }
    DXGI_FORMAT getFormat() const { return m_Format; }
    uint32_t getWidth() const { return m_Width; }
    uint32_t getHeight() const { return m_Height; }
    uint32_t getArrayLayers() const { return m_ArrayLayers; }
    uint32_t getMipLevels() const { return m_MipLevels; }
    uint32_t getSubresourceCount() const { return static_cast<uint32_t>(m_States.size()); }

    D3D12_RESOURCE_STATES getState(uint32_t subresource) const
    {
        return subresource < m_States.size() ? m_States[subresource] : D3D12_RESOURCE_STATE_COMMON;
    }

    void setState(uint32_t subresource, D3D12_RESOURCE_STATES state)
    {
        if (subresource < m_States.size())
        {
            m_States[subresource] = state;
        }
    }

    void setAllStates(D3D12_RESOURCE_STATES state)
    {
        for (D3D12_RESOURCE_STATES& current_state : m_States)
        {
            current_state = state;
        }
    }

private:
    ComPtr<ID3D12Resource> m_Resource;
    DXGI_FORMAT m_Format {DXGI_FORMAT_UNKNOWN};
    uint32_t m_Width {0};
    uint32_t m_Height {0};
    uint32_t m_ArrayLayers {1};
    uint32_t m_MipLevels {1};
    std::vector<D3D12_RESOURCE_STATES> m_States;
};

class DX12ImageView : public RHIImageView
{
public:
    void setResource(DX12Image* image,
                     DXGI_FORMAT format,
                     D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {},
                     D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = {},
                     RHIImageViewType view_type = RHI_IMAGE_VIEW_TYPE_2D,
                     uint32_t array_count = 1,
                     uint32_t mip_levels = 1)
    {
        m_Image = image;
        m_Format = format;
        m_CpuHandle = cpu_handle;
        m_GpuHandle = gpu_handle;
        m_ViewType = view_type;
        m_ArrayCount = array_count;
        m_MipLevels = mip_levels;
    }

    DX12Image* getImage() const { return m_Image; }
    DXGI_FORMAT getFormat() const { return m_Format; }
    D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandle() const { return m_CpuHandle; }
    D3D12_GPU_DESCRIPTOR_HANDLE getGpuHandle() const { return m_GpuHandle; }
    RHIImageViewType getViewType() const { return m_ViewType; }
    uint32_t getArrayCount() const { return m_ArrayCount; }
    uint32_t getMipLevels() const { return m_MipLevels; }

    void setRenderTargetHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        m_RtvHandle = handle;
        m_HasRtv = true;
    }

    void setIsSwapchainView(bool is_swapchain) { m_IsSwapchainView = is_swapchain; }
    bool isSwapchainView() const { return m_IsSwapchainView; }

    void setSwapchainBackBufferIndex(uint32_t index) { m_SwapchainBackBufferIndex = index; }
    uint32_t getSwapchainBackBufferIndex() const { return m_SwapchainBackBufferIndex; }

    void setDepthStencilHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        m_DsvHandle = handle;
        m_HasDsv = true;
    }

    bool hasRenderTargetHandle() const { return m_HasRtv; }
    bool hasDepthStencilHandle() const { return m_HasDsv; }
    D3D12_CPU_DESCRIPTOR_HANDLE getRenderTargetHandle() const { return m_RtvHandle; }
    D3D12_CPU_DESCRIPTOR_HANDLE getDepthStencilHandle() const { return m_DsvHandle; }

private:
    DX12Image* m_Image {nullptr};
    DXGI_FORMAT m_Format {DXGI_FORMAT_UNKNOWN};
    D3D12_CPU_DESCRIPTOR_HANDLE m_CpuHandle {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_GpuHandle {};
    RHIImageViewType m_ViewType {RHI_IMAGE_VIEW_TYPE_2D};
    uint32_t m_ArrayCount {1};
    uint32_t m_MipLevels {1};
    D3D12_CPU_DESCRIPTOR_HANDLE m_RtvHandle {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_DsvHandle {};
    bool m_HasRtv {false};
    bool m_HasDsv {false};
    bool m_IsSwapchainView {false};
    uint32_t m_SwapchainBackBufferIndex {UINT32_MAX};
};

class DX12Sampler : public RHISampler
{
public:
    void setDesc(const D3D12_SAMPLER_DESC& desc,
                 D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {},
                 D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = {})
    {
        m_Desc = desc;
        m_CpuHandle = cpu_handle;
        m_GpuHandle = gpu_handle;
    }
    const D3D12_SAMPLER_DESC& getDesc() const { return m_Desc; }
    D3D12_CPU_DESCRIPTOR_HANDLE getCpuHandle() const { return m_CpuHandle; }
    D3D12_GPU_DESCRIPTOR_HANDLE getGpuHandle() const { return m_GpuHandle; }

private:
    D3D12_SAMPLER_DESC m_Desc {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_CpuHandle {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_GpuHandle {};
};

class DX12Shader : public RHIShader
{
public:
    void setResource(ComPtr<ID3DBlob> blob) { m_ShaderBlob = blob; }

    ComPtr<ID3DBlob> getResource() const { return m_ShaderBlob; }

    const void* getBufferPointer() const { return m_ShaderBlob ? m_ShaderBlob->GetBufferPointer() : nullptr; }

    SIZE_T getBufferSize() const { return m_ShaderBlob ? m_ShaderBlob->GetBufferSize() : 0; }

private:
    ComPtr<ID3DBlob> m_ShaderBlob;
};
