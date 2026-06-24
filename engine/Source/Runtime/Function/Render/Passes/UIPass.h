#pragma once

#include "Runtime/Function/Render/RenderPass.h"
#include "Runtime/UI/Render/UIGpuResources.h"
#include "Runtime/UI/Render/UIRenderBatch.h"

#include <cstdint>
#include <vector>

class WindowUI;

struct UIPassInitInfo : RenderPassInitInfo
{
    RHIRenderPass* render_pass;
};

class UIPass : public RenderPass
{
public:
    void Initialize(const RenderPassInitInfo* init_info) override final;
    void InitializeUIRenderBackend(WindowUI* window_ui) override final;
    void Draw() override final;

    // 检查 UI 渲染管线是否已就绪
    bool IsPipelineReady() const { return m_PipelineReady; }

private:
    void SetupPipeline();
    void DestroyGpuBuffers();
    void EnsureGpuBuffers(size_t vertex_count, size_t index_count);
    void UploadBatch(const UIRenderBatch& batch, float display_width, float display_height);

    WindowUI* m_WindowUi {nullptr};
    UIGpuResources m_GpuResources;
    bool m_PipelineReady {false};

    RHIBuffer* m_VertexBuffer {nullptr};
    RHIDeviceMemory* m_VertexMemory {nullptr};
    RHIBuffer* m_IndexBuffer {nullptr};
    RHIDeviceMemory* m_IndexMemory {nullptr};
    size_t m_VertexCapacity {0};
    size_t m_IndexCapacity {0};

    std::vector<UIVertex> m_CpuVertices;
};
