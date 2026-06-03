#pragma once

#include <cstdint>
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// GPU-side timing for ZEngine Insights (DX12 backend).
//
// Records matched begin/end TIMESTAMP queries on the frame's graphics command
// list, resolves them into a per-frame-slot region of a READBACK buffer at frame
// submit, and -- once the slot's GPU work has completed (polled via the frame
// fence) -- maps the results, converts GPU ticks to the Insights trace clock via
// ID3D12CommandQueue::GetClockCalibration, and pushes the spans into the Insights
// "GPU" timeline track. Readback latency is therefore k_max_frames_in_flight.
//
// All recording is gated on InsightsTrace capture being active (sampled once per
// frame in BeginFrame), so a closed Insights window costs nothing on the GPU.
class DX12GpuProfiler
{
public:
    bool EnsureInitialized(ID3D12Device* device, ID3D12CommandQueue* queue, uint8_t max_frames_in_flight);
    void Shutdown();
    bool IsReady() const { return m_Ready; }

    // Frame begin (after the command list is reset/open). Emits the previous
    // results for this slot if their fence has signalled, then arms the slot.
    void BeginFrame(ID3D12GraphicsCommandList* cmd, uint8_t frame_slot);
    // Matched, nestable scope markers recorded onto the BeginFrame command list.
    void BeginScope(const char* name);
    void EndScope();
    // Frame end (right before the command list is closed): resolves the queries.
    void EndFrame(ID3D12GraphicsCommandList* cmd);
    // After the queue Signal: records which fence value gates this slot's readback.
    void MarkSubmitted(uint8_t frame_slot, ID3D12Fence* fence, uint64_t fence_value);

private:
    void Emit(uint8_t frame_slot);

    static constexpr uint32_t kMaxScopesPerFrame = 192;
    static constexpr uint32_t kQueriesPerFrame = kMaxScopesPerFrame * 2;
    static constexpr uint32_t kInvalidQuery = 0xffffffffu;

    struct PendingScope
    {
        uint32_t begin_query {kInvalidQuery};
        uint32_t end_query {kInvalidQuery};
        uint16_t depth {0};
        std::string name;
    };

    struct FrameData
    {
        uint32_t used_queries {0};
        uint16_t depth {0};
        bool armed {false};
        ID3D12Fence* fence {nullptr};
        uint64_t fence_value {0};
        // Clock calibration captured at BeginFrame (for ticks -> trace-ns).
        uint64_t gpu_ref_ticks {0};
        uint64_t cpu_ref_trace_ns {0};
        bool calibrated {false};
        std::vector<PendingScope> scopes;
        std::vector<uint32_t> open;  // stack of indices into `scopes`
    };

    ID3D12Device* m_Device {nullptr};
    ID3D12CommandQueue* m_Queue {nullptr};
    ComPtr<ID3D12QueryHeap> m_QueryHeap;
    ComPtr<ID3D12Resource> m_Readback;
    std::vector<FrameData> m_Frames;
    ID3D12GraphicsCommandList* m_CurrentCmd {nullptr};
    uint8_t m_MaxFrames {0};
    uint8_t m_CurrentSlot {0};
    uint64_t m_GpuFreq {0};
    uint64_t m_QpcFreq {0};
    bool m_CaptureThisFrame {false};
    bool m_Ready {false};
};
