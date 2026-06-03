#pragma once

#include "Runtime/Function/Particle/EmitterIdAllocator.h"
#include "Runtime/Function/Particle/ParticleDesc.h"
#include "Runtime/Function/Render/Interface/RHIStruct.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderEntity.h"
#include "Runtime/Function/Render/RenderObject.h"
#include "Runtime/Resource/ResType/Global/GlobalParticle.h"
#include "Runtime/Resource/ResType/Global/GlobalRendering.h"

#include <cstdint>
#include <deque>
#include <optional>

struct LevelIBLResourceDesc
{
    SkyBoxIrradianceMap m_SkyboxIrradianceMap;
    SkyBoxSpecularMap m_SkyboxSpecularMap;
    eastl::string m_BrdfMap;
};

struct LevelColorGradingResourceDesc
{
    eastl::string m_ColorGradingMap;
};

struct LevelResourceDesc
{
    LevelIBLResourceDesc m_IblResourceDesc;
    LevelColorGradingResourceDesc m_ColorGradingResourceDesc;
};

struct CameraSwapData
{
    std::optional<float> m_FovX;
    std::optional<RenderCameraType> m_CameraType;
    std::optional<Matrix4x4> m_ViewMatrix;
};

struct GameObjectResourceDesc
{
    std::deque<GameObjectDesc> m_GameObjectDescs;

    void Add(GameObjectDesc& desc);
    void pop();

    bool IsEmpty() const;

    GameObjectDesc& GetNextProcessObject();
};

struct ParticleSubmitRequest
{
    std::vector<ParticleEmitterDesc> m_EmitterDescs;

    void Add(ParticleEmitterDesc& desc);

    unsigned int GetEmitterCount() const;

    const ParticleEmitterDesc& GetEmitterDesc(unsigned int index);
};

struct EmitterTickRequest
{
    std::vector<ParticleEmitterID> m_EmitterIndices;
};

struct EmitterTransformRequest
{
    std::vector<ParticleEmitterTransformDesc> m_TransformDescs;

    void Add(ParticleEmitterTransformDesc& desc);

    void clear();

    unsigned int GetEmitterCount() const;

    const ParticleEmitterTransformDesc& GetNextEmitterTransformDesc(unsigned int index);
};

struct ViewportSwapEntry
{
    RHIViewport viewport {};
    RHIRect2D scissor {};
};

struct RenderSwapData
{
    std::optional<LevelResourceDesc> m_LevelResourceDesc;
    std::optional<GameObjectResourceDesc> m_GameObjectResourceDesc;
    std::optional<GameObjectResourceDesc> m_GameObjectToDelete;
    std::optional<CameraSwapData> m_CameraSwapData;
    std::optional<ParticleSubmitRequest> m_ParticleSubmitRequest;
    std::optional<EmitterTickRequest> m_EmitterTickRequest;
    std::optional<EmitterTransformRequest> m_EmitterTransformRequest;
    bool m_VisibleAxisUpdatePending {false};
    std::optional<RenderEntity> m_VisibleAxis;
    bool m_SceneViewportUpdatePending {false};
    std::optional<ViewportSwapEntry> m_SceneViewportUpdate;

    void AddDirtyGameObject(GameObjectDesc&& desc);
    void AddDeleteGameObject(GameObjectDesc&& desc);

    void AddNewParticleEmitter(ParticleEmitterDesc& desc);
    void AddTickParticleEmitter(ParticleEmitterID id);
    void UpdateParticleTransform(ParticleEmitterTransformDesc& desc);
};

enum SwapDataType : uint8_t
{
    LogicSwapDataType = 0,
    RenderSwapDataType,
    SwapDataTypeCount
};

class RenderSwapContext
{
public:
    RenderSwapData& GetLogicSwapData();
    RenderSwapData& GetRenderSwapData();
    void SwapLogicRenderData();
    void ResetLevelRsourceSwapData();
    void ResetGameObjectResourceSwapData();
    void ResetGameObjectToDelete();
    void ResetCameraSwapData();
    void ResetPartilceBatchSwapData();
    void ResetEmitterTickSwapData();
    void ResetEmitterTransformSwapData();
    void ResetVisibleAxisSwapData();
    void ResetSceneViewportSwapData();

private:
    uint8_t m_LogicSwapDataIndex {LogicSwapDataType};
    uint8_t m_RenderSwapDataIndex {RenderSwapDataType};
    RenderSwapData m_SwapData[SwapDataTypeCount];

    bool IsReadyToSwap() const;
    void swap();
};