#include "Runtime/Function/Render/Texture/RenderTexture.h"

namespace
{
    // 夹到"不高于 v 的最近合法值"。与 ScriptQuality.cpp 里那份同形：MSAA
    // 采样数这类离散合法集，向上取会悄悄超出用户选的档位。
    int SnapDownTo(int v, const int* legal, int count)
    {
        int result = legal[0];
        for (int i = 0; i < count; ++i)
        {
            if (legal[i] <= v)
            {
                result = legal[i];
            }
        }
        return result;
    }
}  // namespace

RenderTexture* RenderTexture::s_Active {nullptr};

void RenderTexture::SetAntiAliasing(int samples)
{
    static const int kLegal[] = {1, 2, 4, 8};
    m_Desc.antiAliasing = SnapDownTo(samples, kLegal, 4);
}

void RenderTexture::SetDepth(int depth)
{
    static const int kLegal[] = {0, 16, 24};
    m_Desc.depth = SnapDownTo(depth, kLegal, 3);
}

void RenderTexture::SetUseMipMap(bool use_mip_map)
{
    m_Desc.useMipMap = use_mip_map;
    if (!use_mip_map)
    {
        // 没有 mip 链还自动生 mip 是自相矛盾的状态。
        m_Desc.autoGenerateMips = false;
    }
}

void RenderTexture::SetAutoGenerateMips(bool auto_generate)
{
    m_Desc.autoGenerateMips = auto_generate;
    if (auto_generate)
    {
        m_Desc.useMipMap = true;
    }
}

void RenderTexture::Create()
{
    if (m_IsCreated)
    {
        return;
    }
    if (m_Desc.width <= 0 || m_Desc.height <= 0)
    {
        // 不创建、也不置位：让 IsCreated() 保持 false，脚本能看到"这一步没成"。
        return;
    }
    // GPU 通路接进来之后，真正的 RHI 分配写在这里。
    m_IsCreated = true;
}

void RenderTexture::Release()
{
    m_IsCreated = false;
    if (s_Active == this)
    {
        // 释放掉的 RT 不能继续当活动目标 —— 留着它，之后的渲染会往一张已经
        // 没有资源的 RT 上画。Unity 也是这个行为（active 自动回落）。
        s_Active = nullptr;
    }
}

void RenderTexture::DiscardContents()
{
    // 见头文件注释：目前没有 GPU 内容可丢。
}

RenderTexture* RenderTexture::GetActive()
{
    return s_Active;
}

void RenderTexture::SetActive(RenderTexture* render_texture)
{
    s_Active = render_texture;
}
