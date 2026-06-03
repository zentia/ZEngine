#include "Runtime/Function/Render/Interface/DX12/DX12BindlessTextureManager.h"

#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Function/Render/Interface/DX12/DX12HostSync.h"
#include "Runtime/Function/Render/Interface/DX12/DX12RHIResource.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Minimal local CheckDX12 (the DX12RHI translation unit's helper is
    // in an anonymous namespace and not exported). Same shape on
    // purpose so log scrapers can match either.
    bool BindlessCheckDX12(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            LOG_ERROR(ZRender, "{} HRESULT=0x{:08X}", message, static_cast<unsigned int>(result));
            return false;
        }
        return true;
    }
}  // namespace

bool DX12BindlessTextureManager::Initialize(ID3D12Device* device,
                                            ID3D12CommandQueue* command_queue,
                                            uint32_t capacity)
{
    if (!device)
    {
        LOG_ERROR(ZRender, "DX12BindlessTextureManager::initialize: device is null");
        return false;
    }
    if (!command_queue)
    {
        LOG_ERROR(ZRender, "DX12BindlessTextureManager::initialize: command_queue is null");
        return false;
    }
    if (capacity == 0)
    {
        LOG_ERROR(ZRender, "DX12BindlessTextureManager::initialize: capacity is 0");
        return false;
    }

    m_Device = device;
    m_Capacity = capacity;

    // Dedicated SHADER_VISIBLE CBV/SRV/UAV heap. Whole range is the
    // bindless table; descriptor index N = bindless slot N (so SM 6.6
    // shaders can use ResourceDescriptorHeap[N] directly without an
    // additional offset).
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = capacity;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap_desc.NodeMask = 0;

    if (!BindlessCheckDX12(m_Device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_Heap)),
                           "DX12BindlessTextureManager: CreateDescriptorHeap failed"))
    {
        m_Device = nullptr;
        m_Capacity = 0;
        return false;
    }

    m_DescriptorSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_CpuStart = m_Heap->GetCPUDescriptorHandleForHeapStart();
    m_GpuStart = m_Heap->GetGPUDescriptorHandleForHeapStart();

    // Reserve slot 0 for the "default white / missing" placeholder.
    // PR5a: actually populate that slot now -- 1x1 RGBA8 white,
    // owned by the manager. Keeps higher layers free of any
    // bring-up dance and guarantees that uninitialised bindless
    // indices sample defined data.
    m_HighWaterMark = 1;
    m_FreeList.clear();
    m_FreeList.reserve(64);

    if (!CreateAndUploadPlaceholder(command_queue))
    {
        LOG_ERROR(ZRender,
                  "DX12BindlessTextureManager: failed to create slot-0 placeholder; aborting init");
        // Tear down what we've built so the caller can demote
        // bindless support cleanly.
        DestroyPlaceholder();
        m_Heap.Reset();
        m_Device = nullptr;
        m_DescriptorSize = 0;
        m_Capacity = 0;
        m_CpuStart = {};
        m_GpuStart = {};
        m_HighWaterMark = 0;
        return false;
    }

    LOG_INFO(ZRender,
             "DX12BindlessTextureManager: initialized (capacity = {}, descriptor_size = {} B, slot 0 = white placeholder)",
             m_Capacity,
             m_DescriptorSize);
    return true;
}

void DX12BindlessTextureManager::Shutdown()
{
    // PR5a: placeholder owns a Resource + SRV heap; release first
    // for symmetry with creation order. ComPtr handles refcount.
    DestroyPlaceholder();

    // ComPtr handles refcount; explicit reset for symmetry with the
    // Vulkan side and to guarantee deterministic ordering vs. the
    // owning RHI's Device::Release.
    m_Heap.Reset();
    m_Device = nullptr;
    m_DescriptorSize = 0;
    m_Capacity = 0;
    m_CpuStart = {};
    m_GpuStart = {};
    m_HighWaterMark = 0;
    m_FreeList.clear();
}

uint32_t DX12BindlessTextureManager::allocate(RHIImageView* image_view, RHISampler* sampler)
{
    // Sampler is intentionally ignored on DX12 -- see the class
    // comment. A future PR will add a parallel sampler-bindless table
    // backed by a D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER heap.
    (void)sampler;

    if (!image_view)
    {
        LOG_WARNING(ZRender, "DX12BindlessTextureManager::allocate called with null image_view");
        return kInvalidBindlessIndex;
    }
    if (!m_Heap)
    {
        LOG_WARNING(ZRender, "DX12BindlessTextureManager::allocate called before Initialize() succeeded");
        return kInvalidBindlessIndex;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32_t slot = kInvalidBindlessIndex;
    if (!m_FreeList.empty())
    {
        slot = m_FreeList.back();
        m_FreeList.pop_back();
    }
    else if (m_HighWaterMark < m_Capacity)
    {
        slot = m_HighWaterMark++;
    }
    else
    {
        LOG_ERROR(ZRender,
                  "DX12BindlessTextureManager::allocate: table full (capacity = {})",
                  m_Capacity);
        return kInvalidBindlessIndex;
    }

    WriteDescriptorLocked(slot, image_view);
    return slot;
}

void DX12BindlessTextureManager::free(uint32_t index)
{
    FreeRawSlot(index);
}

uint32_t DX12BindlessTextureManager::AllocateRawSlot()
{
    if (!m_Heap)
    {
        LOG_WARNING(ZRender, "DX12BindlessTextureManager::AllocateRawSlot called before Initialize() succeeded");
        return kInvalidBindlessIndex;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);

    uint32_t slot = kInvalidBindlessIndex;
    if (!m_FreeList.empty())
    {
        slot = m_FreeList.back();
        m_FreeList.pop_back();
    }
    else if (m_HighWaterMark < m_Capacity)
    {
        slot = m_HighWaterMark++;
    }
    else
    {
        LOG_ERROR(ZRender,
                  "DX12BindlessTextureManager::allocateRawSlot: table full (capacity = {})",
                  m_Capacity);
        return kInvalidBindlessIndex;
    }

    // Caller will write the descriptor themselves.
    return slot;
}

void DX12BindlessTextureManager::FreeRawSlot(uint32_t index)
{
    if (index == kInvalidBindlessIndex)
    {
        return;
    }
    if (index == 0)
    {
        // Slot 0 is the reserved placeholder; refusing to free it
        // matches Vulkan's contract and guarantees uninitialised
        // bindless indices always sample a defined slot.
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_HighWaterMark)
    {
        LOG_WARNING(ZRender,
                    "DX12BindlessTextureManager::freeRawSlot: index {} out of range (high_water = {})",
                    index,
                    m_HighWaterMark);
        return;
    }
    // Note: we do NOT clear the descriptor at this slot. SM 6.6
    // bindless shaders are expected to honour their own validity
    // bookkeeping; rewriting on next allocate() is sufficient.
    m_FreeList.push_back(index);
}

void DX12BindlessTextureManager::Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler)
{
    (void)sampler;
    if (index == kInvalidBindlessIndex)
    {
        return;
    }
    if (!image_view)
    {
        LOG_WARNING(ZRender, "DX12BindlessTextureManager::update: null image_view, index = {}", index);
        return;
    }
    if (!m_Heap)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (index >= m_HighWaterMark)
    {
        LOG_WARNING(ZRender,
                    "DX12BindlessTextureManager::update: index {} not allocated (high_water = {})",
                    index,
                    m_HighWaterMark);
        return;
    }
    WriteDescriptorLocked(index, image_view);
}

D3D12_GPU_DESCRIPTOR_HANDLE DX12BindlessTextureManager::GetGpuHandleAt(uint32_t index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_GpuStart;
    handle.ptr += static_cast<UINT64>(index) * m_DescriptorSize;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DX12BindlessTextureManager::GetCpuHandleAt(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_CpuStart;
    handle.ptr += static_cast<SIZE_T>(index) * m_DescriptorSize;
    return handle;
}

void DX12BindlessTextureManager::WriteDescriptorLocked(uint32_t index, RHIImageView* image_view)
{
    // Source SRV must already exist on a non-shader-visible heap
    // (the engine's standard view creation path puts it on
    // DX12RHI::m_CbvSrvUavHeap). We perform a host-side single-
    // descriptor copy to land it in our shader-visible heap. This
    // is the canonical DX12 "stage to shader heap" pattern; it is
    // synchronous and requires no GPU sync.
    auto* dx12_view = static_cast<DX12ImageView*>(image_view);
    const D3D12_CPU_DESCRIPTOR_HANDLE src = dx12_view->getCpuHandle();
    if (src.ptr == 0)
    {
        LOG_WARNING(ZRender,
                    "DX12BindlessTextureManager: image_view has no SRV CPU handle; slot {} left stale",
                    index);
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_CpuStart;
    dst.ptr += static_cast<SIZE_T>(index) * m_DescriptorSize;

    m_Device->CopyDescriptorsSimple(
        /*NumDescriptors=*/1,
        /*DestDescriptorRangeStart=*/dst,
        /*SrcDescriptorRangeStart=*/src,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// =====================================================================
// PR5a: slot-0 default-white placeholder
// ---------------------------------------------------------------------
// Self-contained DX12 upload path -- no dependency on the engine's
// higher-level texture pipeline -- so the bindless manager can come
// up before any of those systems exist. The cost is ~70 lines of
// plain D3D12 boilerplate.
//
// Steps:
//   1. Create a 1x1 R8G8B8A8_UNORM committed default-heap texture
//      (the placeholder image).
//   2. Create a small upload-heap committed buffer sized to
//      GetCopyableFootprints(...). Map and write 0xFFFFFFFF.
//   3. Open a one-shot command list:
//        copy buffer -> texture (subresource 0),
//        barrier COPY_DEST -> PIXEL_SHADER_RESOURCE.
//   4. Submit on the supplied command queue, signal/wait a fence.
//   5. Tear down upload-heap buffer + the throwaway allocator/list/
//      fence (placeholder image and its SRV heap stay alive).
//   6. Create a non-shader-visible 1-descriptor SRV heap and write
//      the placeholder SRV into it.
//   7. CopyDescriptorsSimple from the staging heap into our
//      shader-visible bindless heap at slot 0.
// =====================================================================

bool DX12BindlessTextureManager::CreateAndUploadPlaceholder(ID3D12CommandQueue* command_queue)
{
    using Microsoft::WRL::ComPtr;

    // ---- 1. 1x1 default-heap placeholder texture --------------------
    D3D12_HEAP_PROPERTIES default_heap_props = {};
    default_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    default_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    default_heap_props.CreationNodeMask = 1;
    default_heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Alignment = 0;
    tex_desc.Width = 1;
    tex_desc.Height = 1;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (!BindlessCheckDX12(m_Device->CreateCommittedResource(
                               &default_heap_props,
                               D3D12_HEAP_FLAG_NONE,
                               &tex_desc,
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               nullptr,
                               IID_PPV_ARGS(&m_PlaceholderImage)),
                           "Bindless placeholder: CreateCommittedResource(texture) failed"))
    {
        return false;
    }

    // ---- 2. Upload-heap staging buffer ------------------------------
    UINT64 upload_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    m_Device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &num_rows, &row_size, &upload_size);

    D3D12_HEAP_PROPERTIES upload_heap_props = {};
    upload_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload_heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    upload_heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    upload_heap_props.CreationNodeMask = 1;
    upload_heap_props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Alignment = 0;
    buf_desc.Width = upload_size;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.Format = DXGI_FORMAT_UNKNOWN;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.SampleDesc.Quality = 0;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buf_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> upload_buffer;
    if (!BindlessCheckDX12(m_Device->CreateCommittedResource(
                               &upload_heap_props,
                               D3D12_HEAP_FLAG_NONE,
                               &buf_desc,
                               D3D12_RESOURCE_STATE_GENERIC_READ,
                               nullptr,
                               IID_PPV_ARGS(&upload_buffer)),
                           "Bindless placeholder: CreateCommittedResource(upload) failed"))
    {
        return false;
    }

    // Map and write one opaque-white texel at the row-major offset
    // dictated by the footprint (Offset is normally 0 for a single
    // 1x1 subresource, but use it for safety).
    void* mapped = nullptr;
    D3D12_RANGE no_read = {0, 0};
    if (!BindlessCheckDX12(upload_buffer->Map(0, &no_read, &mapped),
                           "Bindless placeholder: upload Map failed"))
    {
        return false;
    }
    const uint32_t white = 0xFFFFFFFFu;  // R=G=B=A=0xFF
    std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset, &white, sizeof(white));
    upload_buffer->Unmap(0, nullptr);

    // ---- 3. One-shot command allocator + command list --------------
    ComPtr<ID3D12CommandAllocator> upload_alloc;
    if (!BindlessCheckDX12(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&upload_alloc)),
                           "Bindless placeholder: CreateCommandAllocator failed"))
    {
        return false;
    }

    ComPtr<ID3D12GraphicsCommandList> upload_list;
    if (!BindlessCheckDX12(m_Device->CreateCommandList(0,
                                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       upload_alloc.Get(),
                                                       nullptr,
                                                       IID_PPV_ARGS(&upload_list)),
                           "Bindless placeholder: CreateCommandList failed"))
    {
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = m_PlaceholderImage.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = upload_buffer.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = footprint;

    upload_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // COPY_DEST -> PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = m_PlaceholderImage.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    upload_list->ResourceBarrier(1, &barrier);

    if (!BindlessCheckDX12(upload_list->Close(), "Bindless placeholder: command list Close failed"))
    {
        return false;
    }

    // ---- 4. Submit + fence wait ------------------------------------
    ID3D12CommandList* lists[] = {upload_list.Get()};
    command_queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> upload_fence;
    if (!BindlessCheckDX12(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&upload_fence)),
                           "Bindless placeholder: CreateFence failed"))
    {
        return false;
    }

    HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr)
    {
        LOG_ERROR(ZRender, "Bindless placeholder: CreateEvent failed");
        return false;
    }

    if (!BindlessCheckDX12(command_queue->Signal(upload_fence.Get(), 1),
                           "Bindless placeholder: Signal failed"))
    {
        CloseHandle(fence_event);
        return false;
    }
    if (upload_fence->GetCompletedValue() < 1)
    {
        if (!BindlessCheckDX12(upload_fence->SetEventOnCompletion(1, fence_event),
                               "Bindless placeholder: SetEventOnCompletion failed"))
        {
            CloseHandle(fence_event);
            return false;
        }
        constexpr DWORD k_fence_timeout_ms = 30000;
        if (!ZEngine::DX12HostSync::WaitForFenceValue(upload_fence.Get(),
                                                      1,
                                                      fence_event,
                                                      k_fence_timeout_ms,
                                                      "Bindless placeholder"))
        {
            CloseHandle(fence_event);
            return false;
        }
    }
    CloseHandle(fence_event);

    // ---- 5. Source SRV in a 1-slot non-shader-visible heap ---------
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 1;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // CPU-only
    srv_heap_desc.NodeMask = 0;
    if (!BindlessCheckDX12(m_Device->CreateDescriptorHeap(&srv_heap_desc,
                                                          IID_PPV_ARGS(&m_PlaceholderSrvHeap)),
                           "Bindless placeholder: CreateDescriptorHeap(staging) failed"))
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.PlaneSlice = 0;
    srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;

    const D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu =
        m_PlaceholderSrvHeap->GetCPUDescriptorHandleForHeapStart();
    m_Device->CreateShaderResourceView(m_PlaceholderImage.Get(), &srv_desc, staging_cpu);

    // ---- 6. Copy staging SRV into bindless heap slot 0 -------------
    D3D12_CPU_DESCRIPTOR_HANDLE bindless_slot0 = m_CpuStart;  // slot 0 == start
    m_Device->CopyDescriptorsSimple(1, bindless_slot0, staging_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

void DX12BindlessTextureManager::DestroyPlaceholder()
{
    // ComPtr handles refcount; explicit reset for ordering
    // determinism (image must not outlive the device).
    m_PlaceholderSrvHeap.Reset();
    m_PlaceholderImage.Reset();
}
