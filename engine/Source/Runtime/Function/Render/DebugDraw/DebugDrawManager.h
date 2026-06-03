#pragma once

#include "DebugDrawBuffer.h"
#include "DebugDrawContext.h"
#include "DebugDrawFont.h"
#include "DebugDrawPipeline.h"
#include "Runtime/Core/Base/EngineSystem.h"
#include "Runtime/Function/Render/Interface/RHI.h"
#include "Runtime/Function/Render/RenderResource.h"

class DebugDrawManager : public IEngineSystem
{
public:
    std::string GetName() const override { return "DebugDrawManager"; }
    std::vector<std::type_index> GetDependencies() const override { return {GET_SYSTEM_TYPE(RenderSystem)}; }
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Rendering; }

    DebugDrawManager() {};
    bool Initialize() override;
    void SetupPipelines();
    void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource);
    void Destory();
    void Shutdown() override;
    void Tick(float delta_time);
    void UpdateAfterRecreateSwapchain();
    DebugDrawGroup* TryGetOrCreateDebugDrawGroup(const std::string& name);

    void Draw(uint32_t current_swapchain_image_index);
    ~DebugDrawManager() { Destory(); }

private:
    void UpdateSceneGrid();
    void SwapDataToRender();
    void DrawDebugObject(DebugDrawGroup& debug_draw_group,
                         const Matrix4x4& proj_view_matrix,
                         uint32_t current_swapchain_image_index,
                         const RHIViewport& viewport,
                         const RHIRect2D& scissor);
    void PrepareDrawBuffer(DebugDrawGroup& debug_draw_group, const Matrix4x4& proj_view_matrix);
    void DrawPointLineTriangleBox(uint32_t current_swapchain_image_index);
    void DrawWireFrameObject(DebugDrawGroup& debug_draw_group, uint32_t current_swapchain_image_index);

    std::mutex m_Mutex;
    RHI* m_Rhi = nullptr;
    bool m_RenderEnabled = false;
    DebugDrawPipeline* m_DebugDrawPipeline[DebugDrawPipelineType::_debug_draw_pipeline_type_count] = {};

    DebugDrawAllocator* m_BufferAllocator = nullptr;

    DebugDrawContext m_DebugDrawContext;

    DebugDrawGroup m_DebugDrawGroupForRender;
    DebugDrawGroup m_SceneGridGroupForRender;

    DebugDrawFont* m_Font = nullptr;

    Matrix4x4 m_ProjViewMatrix;
    Matrix4x4 m_SceneProjViewMatrix;

    size_t m_PointStartOffset;
    size_t m_PointEndOffset;
    size_t m_LineStartOffset;
    size_t m_LineEndOffset;
    size_t m_TriangleStartOffset;
    size_t m_TriangleEndOffset;
    size_t m_NoDepthTestPointStartOffset;
    size_t m_NoDepthTestPointEndOffset;
    size_t m_NoDepthTestLineStartOffset;
    size_t m_NoDepthTestLineEndOffset;
    size_t m_NoDepthTestTriangleStartOffset;
    size_t m_NoDepthTestTriangleEndOffset;
    size_t m_TextStartOffset;
    size_t m_TextEndOffset;
};