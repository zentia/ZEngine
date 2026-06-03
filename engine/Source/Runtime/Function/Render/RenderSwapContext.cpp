#include "Runtime/Function/Render/RenderSwapContext.h"

#include <utility>

void GameObjectResourceDesc::Add(GameObjectDesc& desc)
{
    m_GameObjectDescs.push_back(desc);
}

bool GameObjectResourceDesc::IsEmpty() const
{
    return m_GameObjectDescs.empty();
}

GameObjectDesc& GameObjectResourceDesc::GetNextProcessObject()
{
    return m_GameObjectDescs.front();
}

void GameObjectResourceDesc::pop()
{
    m_GameObjectDescs.pop_front();
}

void ParticleSubmitRequest::Add(ParticleEmitterDesc& desc)
{
    m_EmitterDescs.push_back(desc);
}

unsigned int ParticleSubmitRequest::GetEmitterCount() const
{
    return m_EmitterDescs.size();
}

const ParticleEmitterDesc& ParticleSubmitRequest::GetEmitterDesc(unsigned int index)
{
    return m_EmitterDescs[index];
}

void EmitterTransformRequest::Add(ParticleEmitterTransformDesc& desc)
{
    m_TransformDescs.push_back(desc);
}

unsigned int EmitterTransformRequest::GetEmitterCount() const
{
    return m_TransformDescs.size();
}

const ParticleEmitterTransformDesc& EmitterTransformRequest::GetNextEmitterTransformDesc(unsigned int index)
{
    return m_TransformDescs[index];
}

RenderSwapData& RenderSwapContext::GetLogicSwapData()
{
    return m_SwapData[m_LogicSwapDataIndex];
}

RenderSwapData& RenderSwapContext::GetRenderSwapData()
{
    return m_SwapData[m_RenderSwapDataIndex];
}

void RenderSwapContext::SwapLogicRenderData()
{
    if (IsReadyToSwap())
    {
        swap();
    }
}

bool RenderSwapContext::IsReadyToSwap() const
{
    return !(m_SwapData[m_RenderSwapDataIndex].m_LevelResourceDesc.has_value() ||
             m_SwapData[m_RenderSwapDataIndex].m_GameObjectResourceDesc.has_value() ||
             m_SwapData[m_RenderSwapDataIndex].m_GameObjectToDelete.has_value() ||
             m_SwapData[m_RenderSwapDataIndex].m_CameraSwapData.has_value() ||
             m_SwapData[m_RenderSwapDataIndex].m_ParticleSubmitRequest.has_value() ||
             m_SwapData[m_RenderSwapDataIndex].m_EmitterTickRequest.has_value() ||
             m_SwapData[m_RenderSwapDataIndex].m_EmitterTransformRequest.has_value());
}

void RenderSwapContext::ResetLevelRsourceSwapData()
{
    m_SwapData[m_RenderSwapDataIndex].m_LevelResourceDesc.reset();
}

void RenderSwapContext::ResetGameObjectResourceSwapData()
{
    m_SwapData[m_RenderSwapDataIndex].m_GameObjectResourceDesc.reset();
}

void RenderSwapContext::ResetGameObjectToDelete()
{
    m_SwapData[m_RenderSwapDataIndex].m_GameObjectToDelete.reset();
}

void RenderSwapContext::ResetPartilceBatchSwapData()
{
    m_SwapData[m_RenderSwapDataIndex].m_ParticleSubmitRequest.reset();
}

void RenderSwapContext::ResetCameraSwapData()
{
    m_SwapData[m_RenderSwapDataIndex].m_CameraSwapData.reset();
}

void RenderSwapContext::ResetEmitterTickSwapData()
{
    m_SwapData[m_RenderSwapDataIndex].m_EmitterTickRequest.reset();
}

void RenderSwapContext::ResetEmitterTransformSwapData()
{
    m_SwapData[m_RenderSwapDataIndex].m_EmitterTransformRequest.reset();
}

void RenderSwapContext::swap()
{
    ResetLevelRsourceSwapData();
    ResetGameObjectResourceSwapData();
    ResetGameObjectToDelete();
    ResetCameraSwapData();
    ResetEmitterTickSwapData();
    ResetEmitterTransformSwapData();
    ResetPartilceBatchSwapData();
    std::swap(m_LogicSwapDataIndex, m_RenderSwapDataIndex);
}

void RenderSwapData::AddDirtyGameObject(GameObjectDesc&& desc)
{
    if (m_GameObjectResourceDesc.has_value())
    {
        m_GameObjectResourceDesc->Add(desc);
    }
    else
    {
        GameObjectResourceDesc go_descs;
        go_descs.Add(desc);
        m_GameObjectResourceDesc = go_descs;
    }
}

void RenderSwapData::AddDeleteGameObject(GameObjectDesc&& desc)
{
    if (m_GameObjectToDelete.has_value())
    {
        m_GameObjectToDelete->Add(desc);
    }
    else
    {
        GameObjectResourceDesc go_descs;
        go_descs.Add(desc);
        m_GameObjectToDelete = go_descs;
    }
}

void RenderSwapData::AddNewParticleEmitter(ParticleEmitterDesc& desc)
{
    if (m_ParticleSubmitRequest.has_value())
    {
        m_ParticleSubmitRequest->Add(desc);
    }
    else
    {
        ParticleSubmitRequest request;
        request.Add(desc);
        m_ParticleSubmitRequest = request;
    }
}

void RenderSwapData::AddTickParticleEmitter(ParticleEmitterID id)
{
    if (m_EmitterTickRequest.has_value())
    {
        m_EmitterTickRequest->m_EmitterIndices.push_back(id);
    }
    else
    {
        EmitterTickRequest request;
        request.m_EmitterIndices.push_back(id);
        m_EmitterTickRequest = request;
    }
}

void RenderSwapData::UpdateParticleTransform(ParticleEmitterTransformDesc& desc)
{
    if (m_EmitterTransformRequest.has_value())
    {
        m_EmitterTransformRequest->Add(desc);
    }
    else
    {
        EmitterTransformRequest request;
        request.Add(desc);
        m_EmitterTransformRequest = request;
    }
}