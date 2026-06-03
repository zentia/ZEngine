#pragma once

// =====================================================================
// DX12BindlessTextureManager (PR4)
// ---------------------------------------------------------------------
// Concrete DX12 implementation of the cross-backend
// RHIBindlessTextureManager interface. Mirrors the Vulkan one
// (vulkan_bindless_texture_manager.{h,cpp}) but adapts to DX12's
// descriptor model:
//
//   - DX12 has at most one CBV/SRV/UAV heap and one Sampler heap bound
//     at a time. We therefore own a *dedicated* SHADER_VISIBLE
//     CBV/SRV/UAV heap whose entire range serves as the bindless
//     SRV table -- the legacy m_CbvSrvUavHeap on DX12RHI (used by
//     ImGui SRV / etc.) is left untouched so existing call sites
//     keep working.
//
//   - SRVs flow in via D3D12 CopyDescriptorsSimple from the source
//     RHIImageView's CPU handle. The view-side heap is non-shader-
//     visible, so this is the canonical "stage to shader-visible heap"
//     pattern. The copy is performed on the host with no GPU sync
//     required.
//
//   - Samplers are NOT in the SRV heap on DX12 (they live in a
//     separate D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER heap). The Vulkan
//     backend uses COMBINED_IMAGE_SAMPLER which fuses the two; we
//     don't have that affordance here. For PR4 the `sampler`
//     parameter to allocate() / Update() is intentionally ignored on
//     DX12 and a default static sampler is assumed at the shader
//     level. A separate sampler-heap bindless table is a future PR
//     once the engine grows a real sampler-table consumer.
//
// Slot allocation strategy: same as Vulkan -- LIFO free-list backed
// by a monotonic high-water mark, slot 0 reserved as the "default
// white" placeholder, std::mutex for thread safety. Slot indices are
// stable for the lifetime of the manager and identical to the
// shader-visible heap descriptor index, so SM 6.6 shaders can use
// `ResourceDescriptorHeap[idx]` directly.
//
// References (drawn from):
//   - UnrealEngine: FD3D12BindlessDescriptorAllocator
//     (Engine/Source/Runtime/D3D12RHI/Private/D3D12Bindless.cpp)
//   - Microsoft: DirectX-Graphics-Samples / D3D12HelloWorld /
//     ResourceBinding sample.
// We do NOT mirror them line-for-line; only the layout shape (single
// dedicated heap, host copy from non-shader-visible source).
// =====================================================================

#include "Runtime/Function/Render/Interface/RHI.h"

#include <d3d12.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

class DX12BindlessTextureManager final : public RHIBindlessTextureManager
{
public:
    // The SRV heap descriptor index IS the bindless slot index. PR5
    // shaders will read `ResourceDescriptorHeap[NonUniformResourceIndex(
    // index)]` directly. No separate root-signature mapping required
    // (root signature must enable
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED -- handled in PR5).
    DX12BindlessTextureManager() = default;
    ~DX12BindlessTextureManager() override = default;

    // Two-phase init so we never throw out of a constructor and we
    // can fail gracefully (driver lies, OOM on heap creation, ...).
    // Returns false on any DX12 error; the manager is unusable in
    // that case and the owning RHI should not publish it.
    //
    // PR5a: command_queue is required because the manager now also
    // creates and uploads a 1x1 white placeholder texture into
    // slot 0 (so any shader that samples an unbound bindless index
    // sees defined data instead of garbage). The upload runs on a
    // throwaway command allocator + list and synchronises with a
    // fence before returning.
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* command_queue, uint32_t capacity);

    // Releases the heap. Safe to call multiple times.
    void Shutdown();

    // ------- RHIBindlessTextureManager overrides -------------------
    uint32_t capacity() const override { return m_Capacity; }
    uint32_t allocate(RHIImageView* image_view, RHISampler* sampler) override;
    void free(uint32_t index) override;
    void Update(uint32_t index, RHIImageView* image_view, RHISampler* sampler) override;
    // ---------------------------------------------------------------

    // PR-DX1: allocate a raw slot without writing a descriptor.
    // The caller writes their own descriptor (e.g. ImGui font texture
    // SRV, or a CBV for a root-signature UBO) into the heap at the
    // returned CPU handle location. The GPU handle at the same slot is
    // available via GetGpuHandleAt(slot) for descriptor-table references.
    uint32_t AllocateRawSlot();
    void FreeRawSlot(uint32_t index);

    // DX12-specific accessors used by the rest of the DX12 backend
    // when wiring root signatures and binding the descriptor heap on
    // a command list. The heap is SHADER_VISIBLE; bind it via
    // SetDescriptorHeaps(1, &heap).
    ID3D12DescriptorHeap* getDescriptorHeap() const { return m_Heap.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandleAt(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandleAt(uint32_t index) const;

private:
    // Internal write helper. Caller must already hold m_Mutex.
    // Copies a single descriptor from the source RHIImageView's CPU
    // handle into our shader-visible heap at slot `index`.
    void WriteDescriptorLocked(uint32_t index, RHIImageView* image_view);

    // PR5a: builds a 1x1 R8G8B8A8_UNORM white texture, uploads
    // 0xFFFFFFFF to it via a throwaway command list, transitions
    // to PIXEL_SHADER_RESOURCE, creates a non-shader-visible SRV in
    // m_PlaceholderSrvHeap, and copies that SRV into slot 0 of
    // our shader-visible bindless heap. Owns the resources for the
    // manager's lifetime; freed in Shutdown(). Returns false on
    // any failure (caller treats this as a fatal init error -- the
    // contract is broken without slot 0).
    bool CreateAndUploadPlaceholder(ID3D12CommandQueue* command_queue);
    void DestroyPlaceholder();

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_Heap;
    ID3D12Device* m_Device {nullptr};
    UINT m_DescriptorSize {0};
    D3D12_CPU_DESCRIPTOR_HANDLE m_CpuStart {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_GpuStart {};

    // PR5a: slot-0 placeholder ("default white"). Owned by the
    // manager so its lifetime is tied to the bindless table.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_PlaceholderImage;
    // Non-shader-visible 1-descriptor heap that holds the source SRV
    // for the placeholder. Required because WriteDescriptorLocked()
    // expects an RHIImageView with a CPU handle, but the
    // placeholder is owned internally and never wrapped in an
    // RHIImageView. We keep this heap alive so the source descriptor
    // remains valid (DX12 only requires that during the host-side
    // CopyDescriptorsSimple call, but keeping it lets us re-stage
    // if higher layers ever need to refresh).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_PlaceholderSrvHeap;

    uint32_t m_Capacity {0};

    // Slot bookkeeping (guarded by m_Mutex):
    //   - m_HighWaterMark : next never-allocated slot
    //   - m_FreeList       : LIFO of slots returned via free()
    std::mutex m_Mutex;
    uint32_t m_HighWaterMark {0};
    std::vector<uint32_t> m_FreeList;
};
