#pragma once

#include "RenderTextureDesc.h"
#include "Texture.h"
#include "RenderTexture.generated.h"

// Not serialized yet (m_Desc is runtime-only); registered so that
// PPtr<RenderTexture> (RenderCamera::targetTexture) resolves to a real Type.
//
// 脚本门面（Unity 的 `UnityEngine.RenderTexture`）
// ------------------------------------------------
// Unity 的 RenderTexture 是"一张可以当渲染目标的贴图"，核心是
// **`Create()` 之后才有 GPU 资源**，`Release()` 之后就没了，`IsCreated()` 问的
// 是这件事。
//
// ⚠️ 本引擎目前**没有** RenderTexture 的 GPU 通路：RenderCamera::targetTexture
// 只存了个指针，渲染那边还没读它。所以这里能做的是
//   * 描述（width / height / depth / format / mip …）—— 权威值，就存在这里
//   * 生命周期状态（isCreated）—— 由 Create / Release 维护
// 而 `Create()` 之后**并没有真的建出一张 GPU 贴图**。
//
// 这与 `Screen.SetFullScreen`（不导出）的处理不同，理由同 `QualitySettings`：
// 描述与生命周期是**上层契约**，"我要一张 1280x720 带深度的 RT"这个意图得有
// 地方表达；GPU 那一半什么时候接是实现进度。等接进来时，`Create()` 里加一句
// 真正的分配即可，脚本侧一个字都不用改。
ZCLASS()
class RenderTexture : public Texture
{
    ZGENERATED_BODY(RenderTexture)

public:
    // ---- 描述（可读可写）----

    /// 宽（像素）。Unity 的 `RenderTexture.width`。
    ZFUNCTION()
    int GetWidth() const { return m_Desc.width; }
    ZSCRIPTPROPERTY(Name = "width", Getter = "GetWidth", Setter = "SetWidth")

    /// 设置宽。<= 0 夹到 1（0 宽的资源在 RHI 上是非法尺寸，而在脚本这一侧
    /// 只能表现为"创建失败"，那已经太晚了）。
    ZFUNCTION()
    void SetWidth(int width) { m_Desc.width = width > 0 ? width : 1; }

    /// 高（像素）。Unity 的 `RenderTexture.height`。
    ZFUNCTION()
    int GetHeight() const { return m_Desc.height; }
    ZSCRIPTPROPERTY(Name = "height", Getter = "GetHeight", Setter = "SetHeight")

    /// 设置高。<= 0 夹到 1。
    ZFUNCTION()
    void SetHeight(int height) { m_Desc.height = height > 0 ? height : 1; }

    /// MSAA 采样数（1 = 不开）。Unity 的 `RenderTexture.antiAliasing`。
    ZFUNCTION()
    int GetAntiAliasing() const { return m_Desc.antiAliasing; }
    ZSCRIPTPROPERTY(Name = "antiAliasing", Getter = "GetAntiAliasing", Setter = "SetAntiAliasing")

    /// 设置 MSAA 采样数。只接受 1/2/4/8，其余夹到不高于它的合法值 —— 与
    /// `QualitySettings.antiAliasing` 同一条理由（采样数是管线里的常量）。
    ZFUNCTION()
    void SetAntiAliasing(int samples);

    /// 深度缓冲位数（0 / 16 / 24）。Unity 的 `RenderTexture.depth`。
    ZFUNCTION()
    int GetDepth() const { return m_Desc.depth; }
    ZSCRIPTPROPERTY(Name = "depth", Getter = "GetDepth", Setter = "SetDepth")

    /// 设置深度缓冲位数。只接受 0/16/24，其余夹到不高于它的合法值。
    ZFUNCTION()
    void SetDepth(int depth);

    /// 像素格式的 RHIFormat 序号。
    ZFUNCTION()
    int GetFormat() const { return static_cast<int>(m_Desc.format); }
    ZSCRIPTPROPERTY(Name = "format", Getter = "GetFormat", Setter = "SetFormat")

    /// 设置像素格式序号。负值夹到 0（"未指定"）。
    ZFUNCTION()
    void SetFormat(int format) { m_Desc.format = format > 0 ? static_cast<uint32_t>(format) : 0u; }

    /// 是否带 mip 链。Unity 的 `RenderTexture.useMipMap`。
    ZFUNCTION()
    bool GetUseMipMap() const { return m_Desc.useMipMap; }
    ZSCRIPTPROPERTY(Name = "useMipMap", Getter = "GetUseMipMap", Setter = "SetUseMipMap")

    /// 设置是否带 mip 链。
    ///
    /// 关掉时**连带关掉** autoGenerateMips —— 没有 mip 链却自动生 mip 是个自相
    /// 矛盾的状态，Unity 也是这么处理的。
    ZFUNCTION()
    void SetUseMipMap(bool use_mip_map);

    /// 是否自动生 mip。
    ZFUNCTION()
    bool GetAutoGenerateMips() const { return m_Desc.autoGenerateMips; }
    ZSCRIPTPROPERTY(Name = "autoGenerateMips", Getter = "GetAutoGenerateMips",
                    Setter = "SetAutoGenerateMips")

    /// 设置是否自动生 mip。为真时**连带打开** useMipMap。
    ZFUNCTION()
    void SetAutoGenerateMips(bool auto_generate);

    // ---- 生命周期 ----

    /// GPU 资源是否已建立。Unity 的 `RenderTexture.IsCreated()`。
    ///
    /// 注意它问的是"有没有"，不是"还有效吗"：Unity 在设备丢失（device lost）
    /// 后会变回 false，这里不会（没有那套通知）。
    ZFUNCTION()
    bool IsCreated() const { return m_IsCreated; }
    ZSCRIPTPROPERTY(Name = "isCreated", Getter = "IsCreated")

    /// 建立资源。Unity 的 `RenderTexture.Create()`。
    ///
    /// 尺寸非法（宽或高 <= 0）时**不创建**并保持 `IsCreated() == false` —— 见
    /// SetWidth 的注释：宁可在这一步失败，也不要造出一个 RHI 拒绝的资源。
    /// 重复调用是幂等的（已经 created 就直接返回）。
    ZFUNCTION()
    void Create();

    /// 释放资源。Unity 的 `RenderTexture.Release()`。未创建时是空操作。
    ZFUNCTION()
    void Release();

    /// 丢弃内容（内容变未定义，但资源还在）。Unity 的
    /// `RenderTexture.DiscardContents()`。
    ///
    /// ⚠️ 目前**是空操作**：没有 GPU 内容可丢。留着它是因为"我要复用这张 RT，
    /// 上一帧的内容我不要了"是脚本侧合法的意图表达 —— 等 GPU 通路接进来，这
    /// 里就是一句 `DiscardContents` 而已。
    ZFUNCTION()
    void DiscardContents();

    // ---- 全局活动 RT（Unity 的 `RenderTexture.active`）--------------------

    /// 当前的全局活动 RT。没有时返回 nullptr。
    ZFUNCTION()
    static RenderTexture* GetActive();
    ZSCRIPTPROPERTY(Name = "active", Getter = "GetActive", Setter = "SetActive")

    /// 设置全局活动 RT。传 nullptr 表示"渲染回屏幕"。
    ZFUNCTION()
    static void SetActive(RenderTexture* render_texture);

private:
    RenderTextureDesc m_Desc;
    bool              m_IsCreated {false};

    /// 进程级单例（同 `Gizmos.color` / `TimeState`）：`RenderTexture.active` 是
    /// 全局概念，不该挂在某张 RT 上。
    static RenderTexture* s_Active;
};
