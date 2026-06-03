#pragma once

// Nanite系统主头文件
// 提供统一的Nanite系统接口

#include "NaniteCluster.h"
#include "NaniteCulling.h"
#include "NaniteRenderer.h"
#include "NaniteTypes.h"

// Nanite系统管理器
class NaniteSystem
{
public:
    static NaniteSystem& GetInstance()
    {
        static NaniteSystem instance;
        return instance;
    }

    // 初始化
    bool Initialize(std::shared_ptr<RHI> rhi, std::shared_ptr<RenderResourceBase> render_resource);

    // 清理
    void Shutdown();

    // 获取渲染器
    std::shared_ptr<NaniteRenderer> getRenderer() const { return m_Renderer; }

    // 获取配置
    const NaniteConfig& getConfig() const { return m_Config; }
    void SetConfig(const NaniteConfig& config) { m_Config = config; }

private:
    NaniteSystem() = default;
    ~NaniteSystem() = default;
    NaniteSystem(const NaniteSystem&) = delete;
    NaniteSystem& operator=(const NaniteSystem&) = delete;

    std::shared_ptr<NaniteRenderer> m_Renderer;
    NaniteConfig m_Config;
};