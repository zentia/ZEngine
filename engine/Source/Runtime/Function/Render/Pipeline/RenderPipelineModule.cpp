#include "Runtime/Function/Render/Pipeline/RenderPipelineModule.h"

#include "Runtime/Function/Render/RenderSwapContext.h"

void RenderPipelineModule::ConsumeParticleSwapData(RenderSwapData& swap_data, RenderSwapContext& swap_context)
{
    if (swap_data.m_ParticleSubmitRequest.has_value())
    {
        swap_context.ResetPartilceBatchSwapData();
    }
    if (swap_data.m_EmitterTickRequest.has_value())
    {
        swap_context.ResetEmitterTickSwapData();
    }
    if (swap_data.m_EmitterTransformRequest.has_value())
    {
        swap_context.ResetEmitterTransformSwapData();
    }
}
