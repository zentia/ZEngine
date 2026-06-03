#pragma once

#include "LumenRenderer.h"
#include "Runtime/Function/Render/RenderCamera.h"
#include "Runtime/Function/Render/RenderPassBase.h"
#include "Runtime/Function/Render/RenderScene.h"

#include <memory>

class RHI;
class RenderResourceBase;

// Lumen渲染Pass
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

private:
    // 初始化渲染资源
    void InitializeRenderResources();

    // 更新渲染资源
    void UpdateRenderResources(std::shared_ptr<RenderResourceBase> render_resource);

    // Lumen渲染器
    LumenRenderer m_LumenRenderer;

    // Lumen配置
    LumenConfig m_LumenConfig;
};