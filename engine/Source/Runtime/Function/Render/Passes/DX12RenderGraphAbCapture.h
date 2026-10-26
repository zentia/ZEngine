#pragma once

// =====================================================================================
// DX12RenderGraphAbCapture -- in-frame A/B capture for the render-graph paths of RP1
// (ZENGINE_RG_RP1) and RP2 (ZENGINE_RG_RP2).
// -------------------------------------------------------------------------------------
// Why in-frame: comparing two separate runs (graph on vs off) is dominated by noise that
// has nothing to do with the graph -- time-of-day, particles, Spine dt, MegaLights
// temporal history, camera drift. So on ONE frame DX12MainCameraPass renders each stage
// twice with identical inputs:
//
//     RP1 hand-written  -> copy backup_odd            "rp1.handwritten.backup_odd"
//     RP1 graph         -> copy backup_odd            "rp1.graph.backup_odd"
//     RP2 hand-written  -> copy backup_even, backbuffer
//     RP2 graph         -> copy backup_even, backbuffer
//
// (RP2 reads RP1's output; both RP2 runs read the SAME backup_odd, the graph one.) With a
// correct graph path the pairs are bit-identical; scripts/rg_ab_compare.py diffs them.
//
// Switch: ZENGINE_RG_AB_CAPTURE=<n> captures the n-th DX12MainCameraPass::Draw (n >= 1;
// give the scene a few frames to settle, e.g. 120). Output directory:
// ZENGINE_RG_AB_DIR (default "rg_ab_capture" in the working directory). Independent of
// ZENGINE_RG_RP1 / ZENGINE_RG_RP2 -- the capture forces each path itself and restores
// the switches afterwards. Off (env unset) = this object does nothing at all.
//
// Readback is asynchronous and never stalls the GPU: the copies are recorded into the
// capture frame's command list, and the files are written on a later frame once that
// frame slot comes round again -- DesktopRenderPipelineModule calls WaitForFences() on the
// current slot before every frame, so the slot's previous work (our copies) is complete.
// =====================================================================================

#include "Runtime/Function/Render/Passes/RenderGraphAbCaptureFormat.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cstdint>
#include <string>
#include <vector>

class RHI;
class RHIImage;
class RHIBuffer;
class RHIDeviceMemory;

class DX12RenderGraphAbCapture
{
public:
    /// Reads the environment. Cheap; does nothing unless ZENGINE_RG_AB_CAPTURE is set.
    void Initialize(RHI* rhi);

    /// Frees any readback still in flight. Waits for the GPU first if something is
    /// pending (only possible when shutting down within a few frames of the capture).
    void Shutdown();

    bool IsEnabled() const { return m_Enabled; }

    /// Call once at the top of every frame's Draw. Writes out a finished capture, and
    /// returns true when THIS frame is the one to capture (at most once per process).
    bool BeginFrame();

    /// Records a GPU copy of `image` into a readback buffer and, afterwards, transitions
    /// the image to `restore_layout` so whatever runs next finds it where it expects.
    void CopyImage(const char* name, RHIImage* image, RHIFormat format, uint32_t width, uint32_t height,
                   RHIImageLayout restore_layout);

    /// Which path a stage actually took on the capture frame (see ManifestStage).
    void RecordStage(const char* stage, bool attempted, bool graph_used);
    void AddNote(std::string note) { m_Notes.push_back(std::move(note)); }

private:
    struct Pending
    {
        RenderGraphAbCapture::ManifestImage info;
        RHIBuffer* buffer {nullptr};
        RHIDeviceMemory* memory {nullptr};
        uint64_t row_pitch {0};
        uint32_t bytes_per_pixel {0};
    };

    void Resolve();
    void ReleasePending();
    void WriteManifest();

    RHI* m_Rhi {nullptr};
    bool m_Enabled {false};
    uint64_t m_CaptureFrame {0};
    uint64_t m_Frame {0};

    bool m_Captured {false};      // the capture frame has been recorded
    bool m_Done {false};          // files written (or given up)
    uint64_t m_CapturedOnFrame {0};
    uint8_t m_CaptureSlot {0};

    std::string m_Dir;
    std::vector<Pending> m_Pending;
    std::vector<RenderGraphAbCapture::ManifestImage> m_Images;  // failed-at-record entries
    std::vector<RenderGraphAbCapture::ManifestStage> m_Stages;
    std::vector<std::string> m_Notes;
};
