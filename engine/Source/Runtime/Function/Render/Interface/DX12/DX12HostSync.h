#pragma once

// DX12 host-side helpers (Windows only; this TU is not built on other OSes).
#include "Runtime/Core/Log/LogSystem.h"

#include <Windows.h>
#include <d3d12.h>

namespace ZEngine::DX12HostSync
{
    // Pump pending Win32 messages while blocked on D3D12 GPU work. Without this,
    // DXGI / D3D debug-layer callbacks during startup can stall the command queue
    // forever when the splash screen is not processing messages.
    inline void PumpWindowsMessages()
    {
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Wait until fence reaches |value|, processing window messages while blocked.
    // Returns false on timeout (caller should demote bindless / abort upload).
    inline bool WaitForFenceValue(ID3D12Fence* fence,
                                  UINT64 value,
                                  HANDLE fence_event,
                                  DWORD timeout_ms,
                                  const char* context)
    {
        if (!fence || fence_event == nullptr)
        {
            return false;
        }

        if (fence->GetCompletedValue() >= value)
        {
            return true;
        }

        const DWORD start_tick = GetTickCount();
        for (;;)
        {
            if (fence->GetCompletedValue() >= value)
            {
                return true;
            }

            DWORD slice_ms = 16;
            if (timeout_ms != INFINITE)
            {
                const DWORD elapsed = GetTickCount() - start_tick;
                if (elapsed >= timeout_ms)
                {
                    LOG_ERROR(ZRender,
                              "{}: GPU fence wait timed out after {} ms (completed={}, expected={})",
                              context ? context : "DX12",
                              timeout_ms,
                              fence->GetCompletedValue(),
                              value);
                    return false;
                }
                const DWORD remaining = timeout_ms - elapsed;
                slice_ms = (remaining < slice_ms) ? remaining : slice_ms;
            }

            const DWORD wait_result =
                MsgWaitForMultipleObjects(1, &fence_event, FALSE, slice_ms, QS_ALLINPUT);
            if (wait_result == WAIT_OBJECT_0)
            {
                return fence->GetCompletedValue() >= value;
            }
            if (wait_result == WAIT_OBJECT_0 + 1)
            {
                PumpWindowsMessages();
                continue;
            }
            // WAIT_TIMEOUT: loop and re-check completed value.
        }
    }
}  // namespace ZEngine::DX12HostSync
