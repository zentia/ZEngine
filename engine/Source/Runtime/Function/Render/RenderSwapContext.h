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

/// 逐字段比较，给"内容没变就别再上传一次"用。
///
/// 上传一次 LevelResourceDesc 要读 12 张 HDR cubemap 面 + BRDF 并在 GPU 上重建
/// 两张 cubemap（Metal 上还要跑一遍 mipgen）。开场景 / 重载关卡时调用的
/// `RenderSystem::RequestGlobalResourceUpload()` 会走到这里，而**绝大多数关卡切换
/// 换的只是 GameObject，全局渲染资源根本没变** —— 每次都重建就是白白的几十毫秒
/// 加一次显存抖动。
inline bool operator==(const LevelIBLResourceDesc& a, const LevelIBLResourceDesc& b)
{
    return a.m_BrdfMap == b.m_BrdfMap &&
           a.m_SkyboxIrradianceMap.m_PositiveXMap == b.m_SkyboxIrradianceMap.m_PositiveXMap &&
           a.m_SkyboxIrradianceMap.m_NegativeXMap == b.m_SkyboxIrradianceMap.m_NegativeXMap &&
           a.m_SkyboxIrradianceMap.m_PositiveYMap == b.m_SkyboxIrradianceMap.m_PositiveYMap &&
           a.m_SkyboxIrradianceMap.m_NegativeYMap == b.m_SkyboxIrradianceMap.m_NegativeYMap &&
           a.m_SkyboxIrradianceMap.m_PositiveZMap == b.m_SkyboxIrradianceMap.m_PositiveZMap &&
           a.m_SkyboxIrradianceMap.m_NegativeZMap == b.m_SkyboxIrradianceMap.m_NegativeZMap &&
           a.m_SkyboxSpecularMap.m_PositiveXMap == b.m_SkyboxSpecularMap.m_PositiveXMap &&
           a.m_SkyboxSpecularMap.m_NegativeXMap == b.m_SkyboxSpecularMap.m_NegativeXMap &&
           a.m_SkyboxSpecularMap.m_PositiveYMap == b.m_SkyboxSpecularMap.m_PositiveYMap &&
           a.m_SkyboxSpecularMap.m_NegativeYMap == b.m_SkyboxSpecularMap.m_NegativeYMap &&
           a.m_SkyboxSpecularMap.m_PositiveZMap == b.m_SkyboxSpecularMap.m_PositiveZMap &&
           a.m_SkyboxSpecularMap.m_NegativeZMap == b.m_SkyboxSpecularMap.m_NegativeZMap;
}

inline bool operator==(const LevelResourceDesc& a, const LevelResourceDesc& b)
{
    return a.m_IblResourceDesc == b.m_IblResourceDesc &&
           a.m_ColorGradingResourceDesc.m_ColorGradingMap == b.m_ColorGradingResourceDesc.m_ColorGradingMap;
}

inline bool operator!=(const LevelResourceDesc& a, const LevelResourceDesc& b) { return !(a == b); }

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

    /// 排一次全局资源上传（IBL 的 12 张 HDR cubemap 面 + BRDF + 颜色分级 LUT）。
    ///
    /// **覆盖**语义，不是队列追加：全局资源是一整份替换，同一帧里排两次只有最后
    /// 一次有意义（GameObject 那边才是队列，所以它是 `Add`）。
    ///
    /// 这个字段以前**全库没有生产者** —— 两个 `ProcessSwapData()` 都读它、
    /// `IsReadyToSwap()` 把它当门闩，但没有任何地方赋值 ⇒ "关卡热重载刷新全局
    /// 资源"在任何平台都不通。唯一的生产者是
    /// `RenderSystem::RequestGlobalResourceUpload()`。
    void SetLevelResourceDesc(const LevelResourceDesc& desc);

    void AddDirtyGameObject(GameObjectDesc&& desc);
    void AddDeleteGameObject(GameObjectDesc&& desc);
    /// Drop a still-queued delete for `go_id` (editor undo restoring an object in the
    /// frame it was deleted). The render-buffer drain runs upserts BEFORE deletes, so
    /// a delete + re-upsert of the same id in one batch would otherwise lose the object.
    void CancelDeleteGameObject(GObjectID go_id);

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
    // 清空 logic buffer 里待处理的 GameObjectDesc。Render 侧的 Reset* 只作用于
    // render buffer，但 ProcessSwapData 会直接读 logic buffer，所以丢弃关卡时
    // 必须单独清这一侧。只应由游戏线程调用。
    void ResetLogicGameObjectSwapData();
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