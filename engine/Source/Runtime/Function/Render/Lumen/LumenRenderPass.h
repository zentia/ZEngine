#pragma once

#include "LumenRenderer.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderPassBase.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <memory>

class RHI;
class RenderResourceBase;

// Lumen渲染Pass
//
// 这是 Lumen 唯一的生产者：每帧在渲染线程上被 RenderPipelineBase::UpdateLumen 调用，
// 跑完把结果写进 RenderScene::m_IndirectLighting，供后续 Pass / per-frame uniform 取用。
class LumenRenderPass : public RenderPassBase
{
public:
    LumenRenderPass();
    ~LumenRenderPass();

    // 初始化
    virtual void Initialize(const RenderPassInitInfo* init_info) override;

    // 准备Pass数据
    virtual void PreparePassData(std::shared_ptr<RenderResourceBase> render_resource) override;

    // 执行渲染
    void Render(std::shared_ptr<RenderScene> render_scene, std::shared_ptr<RenderCamera> camera, float delta_time);

    // 获取Lumen渲染器
    LumenRenderer* getLumenRenderer() { return &m_LumenRenderer; }

    // 最近一次 Render 有没有产出间接光。给调试/编辑器用。
    bool hasPublishedIndirectLighting() const { return m_HasPublished; }

private:
    // 把 r.Lumen.* 的当前值推给渲染器（每帧都做，所以 CVar 是热生效的）
    void ApplyConsoleVariables();

    // 把本帧的 GI 结果写进场景
    void PublishIndirectLighting(RenderScene& render_scene);

    // 把本帧的反射结果写进场景（与 GI 是独立的开关，所以单独发布、单独一个权重）
    void PublishReflection(RenderScene& render_scene);

    // Lumen渲染器
    LumenRenderer m_LumenRenderer;

    // Lumen配置
    LumenConfig m_LumenConfig;

    // 最近一次 Render 是否产出了间接光
    bool m_HasPublished {false};

    // 子系统是否真的初始化过（需要 render_resource，而它在 Initialize 时经常还没有）
    bool m_RendererInitialized {false};
};
