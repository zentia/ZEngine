#include "Runtime/Function/Render/Interface/DX12/Utility/DX12GpuProfiler.h"

#include "Runtime/Profiler/InsightsTrace.h"

#include <windows.h>

bool DX12GpuProfiler::EnsureInitialized(ID3D12Device* device, ID3D12CommandQueue* queue, uint8_t max_frames_in_flight)
{
    if (m_Ready)
        return true;
    if (device == nullptr || queue == nullptr || max_frames_in_flight == 0)
        return false;

    m_Device = device;
    m_Queue = queue;
    m_MaxFrames = max_frames_in_flight;

    D3D12_QUERY_HEAP_DESC heap_desc {};
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap_desc.Count = static_cast<UINT>(m_MaxFrames) * kQueriesPerFrame;
    heap_desc.NodeMask = 0;
    if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&m_QueryHeap))))
        return false;

    const uint64_t buffer_bytes = static_cast<uint64_t>(m_MaxFrames) * kQueriesPerFrame * sizeof(uint64_t);
    D3D12_HEAP_PROPERTIES heap_props {};
    heap_props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC res_desc {};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    res_desc.Width = buffer_bytes;
    res_desc.Height = 1;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.Format = DXGI_FORMAT_UNKNOWN;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &res_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_Readback))))
        return false;

    if (FAILED(queue->GetTimestampFrequency(&m_GpuFreq)) || m_GpuFreq == 0)
        return false;

    LARGE_INTEGER qpc_freq {};
    QueryPerformanceFrequency(&qpc_freq);
    m_QpcFreq = static_cast<uint64_t>(qpc_freq.QuadPart);

    m_Frames.assign(m_MaxFrames, FrameData {});
    m_Ready = true;
    return true;
}

void DX12GpuProfiler::Shutdown()
{
    m_QueryHeap.Reset();
    m_Readback.Reset();
    m_Frames.clear();
    m_Ready = false;
    m_Device = nullptr;
    m_Queue = nullptr;
    m_CurrentCmd = nullptr;
}

void DX12GpuProfiler::BeginFrame(ID3D12GraphicsCommandList* cmd, uint8_t frame_slot)
{
    if (!m_Ready || frame_slot >= m_MaxFrames)
        return;

    m_CurrentCmd = cmd;
    m_CurrentSlot = frame_slot;
    FrameData& f = m_Frames[frame_slot];

    // Drain the previous use of this slot once its GPU work has completed.
    if (f.armed && f.fence != nullptr && f.fence->GetCompletedValue() >= f.fence_value)
    {
        Emit(frame_slot);
        f.armed = false;
    }

    f.used_queries = 0;
    f.depth = 0;
    f.scopes.clear();
    f.open.clear();
    f.calibrated = false;

    m_CaptureThisFrame = ZEngine::Insights::InsightsTrace::Get().IsCapturing();
    if (!m_CaptureThisFrame)
        return;

    // Calibrate the GPU clock against the Insights trace clock for THIS frame.
    uint64_t gpu_ticks = 0;
    uint64_t cpu_qpc = 0;
    if (SUCCEEDED(m_Queue->GetClockCalibration(&gpu_ticks, &cpu_qpc)) && m_QpcFreq != 0)
    {
        LARGE_INTEGER qpc_now {};
        QueryPerformanceCounter(&qpc_now);
        const uint64_t steady_now = ZEngine::Insights::NowNs();
        const double dqpc = static_cast<double>(static_cast<int64_t>(cpu_qpc) - qpc_now.QuadPart);
        const double dns = dqpc * 1.0e9 / static_cast<double>(m_QpcFreq);
        f.gpu_ref_ticks = gpu_ticks;
        f.cpu_ref_trace_ns = static_cast<uint64_t>(static_cast<double>(steady_now) + dns);
        f.calibrated = true;
    }
}

void DX12GpuProfiler::BeginScope(const char* name)
{
    if (!m_Ready || !m_CaptureThisFrame || m_CurrentCmd == nullptr)
        return;
    FrameData& f = m_Frames[m_CurrentSlot];

    PendingScope ps;
    ps.depth = f.depth;
    ps.name = (name != nullptr) ? name : "";
    if (f.used_queries < kQueriesPerFrame)
    {
        const uint32_t q = static_cast<uint32_t>(m_CurrentSlot) * kQueriesPerFrame + f.used_queries++;
        m_CurrentCmd->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
        ps.begin_query = q;
    }
    f.scopes.push_back(std::move(ps));
    f.open.push_back(static_cast<uint32_t>(f.scopes.size() - 1));
    ++f.depth;
}

void DX12GpuProfiler::EndScope()
{
    if (!m_Ready || !m_CaptureThisFrame || m_CurrentCmd == nullptr)
        return;
    FrameData& f = m_Frames[m_CurrentSlot];
    if (f.open.empty())
        return;
    const uint32_t idx = f.open.back();
    f.open.pop_back();
    if (f.depth > 0)
        --f.depth;
    PendingScope& ps = f.scopes[idx];
    if (ps.begin_query == kInvalidQuery)
        return;
    if (f.used_queries < kQueriesPerFrame)
    {
        const uint32_t q = static_cast<uint32_t>(m_CurrentSlot) * kQueriesPerFrame + f.used_queries++;
        m_CurrentCmd->EndQuery(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
        ps.end_query = q;
    }
}

void DX12GpuProfiler::EndFrame(ID3D12GraphicsCommandList* cmd)
{
    if (!m_Ready || !m_CaptureThisFrame || cmd == nullptr)
        return;
    FrameData& f = m_Frames[m_CurrentSlot];
    if (f.used_queries == 0)
    {
        f.armed = false;
        return;
    }
    const uint32_t base = static_cast<uint32_t>(m_CurrentSlot) * kQueriesPerFrame;
    cmd->ResolveQueryData(m_QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, f.used_queries, m_Readback.Get(),
                          static_cast<uint64_t>(base) * sizeof(uint64_t));
    f.armed = true;
}

void DX12GpuProfiler::MarkSubmitted(uint8_t frame_slot, ID3D12Fence* fence, uint64_t fence_value)
{
    if (!m_Ready || frame_slot >= m_MaxFrames)
        return;
    FrameData& f = m_Frames[frame_slot];
    if (!f.armed)
        return;
    f.fence = fence;
    f.fence_value = fence_value;
}

void DX12GpuProfiler::Emit(uint8_t frame_slot)
{
    FrameData& f = m_Frames[frame_slot];
    if (f.scopes.empty() || !f.calibrated || m_Readback == nullptr)
        return;

    const uint32_t base = static_cast<uint32_t>(frame_slot) * kQueriesPerFrame;
    D3D12_RANGE read_range {};
    read_range.Begin = static_cast<SIZE_T>(base) * sizeof(uint64_t);
    read_range.End = static_cast<SIZE_T>(base + f.used_queries) * sizeof(uint64_t);
    void* mapped = nullptr;
    if (FAILED(m_Readback->Map(0, &read_range, &mapped)) || mapped == nullptr)
        return;

    const uint64_t* ticks = static_cast<const uint64_t*>(mapped);
    ZEngine::Insights::InsightsTrace& trace = ZEngine::Insights::InsightsTrace::Get();
    for (const PendingScope& ps : f.scopes)
    {
        if (ps.begin_query == kInvalidQuery || ps.end_query == kInvalidQuery)
            continue;
        const uint64_t t0 = ticks[ps.begin_query];
        const uint64_t t1 = ticks[ps.end_query];
        if (t1 <= t0)
            continue;
        const double d0 = static_cast<double>(static_cast<int64_t>(t0) - static_cast<int64_t>(f.gpu_ref_ticks));
        const double d1 = static_cast<double>(static_cast<int64_t>(t1) - static_cast<int64_t>(f.gpu_ref_ticks));
        const uint64_t start_ns =
            static_cast<uint64_t>(static_cast<double>(f.cpu_ref_trace_ns) + d0 * 1.0e9 / static_cast<double>(m_GpuFreq));
        const uint64_t end_ns =
            static_cast<uint64_t>(static_cast<double>(f.cpu_ref_trace_ns) + d1 * 1.0e9 / static_cast<double>(m_GpuFreq));
        trace.PushExternalEvent("GPU", ps.name.c_str(), ps.depth, start_ns, end_ns);
    }

    const D3D12_RANGE written {0, 0};  // we did not write to the readback resource
    m_Readback->Unmap(0, &written);
    f.scopes.clear();
    f.open.clear();
}
