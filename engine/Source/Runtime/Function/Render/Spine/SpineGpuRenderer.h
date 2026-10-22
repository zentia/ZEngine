#pragma once

// =====================================================================
// GPU-Spine 渲染侧：容器 + 解算器 → 屏幕
// 规格：doc/GPU_SPINE_INTERCHANGE_FORMAT.md §3 / §5 / §6 / §8
// ---------------------------------------------------------------------
// 每帧 CPU 只做两件事：`SpineGpuSamplePose`（骨骼世界变换 + 槽位状态）与"哪些绘制项可见"；
// **顶点一个都不在 CPU 上算** —— 蒙皮在顶点着色器里做（spine_gpu.vert），静态顶点 /
// 影响流 / UV / 索引每个资产只上传一次、所有实例共用。
//
// 第一版的边界（明确不做，不是忘了）：
//   * 只在 Vulkan 编辑器里画（MainCameraPass 的 RP2 UI subpass，DX12 / Metal 另接）；
//   * 只有一个实例，由控制台 `spine.load <骨架文件> [动画下标]` 指定，画在视口里自动居中；
//   * mesh deform（S6）在 GPU 上做：解算器给每帧生效的 deform 数组，上传到 b7，shader 里
//     rigid 整体替换 bindPos、weighted 逐影响加偏移，共享 VB 不动；
//   * 在裁剪作用域内的绘制项**照未裁剪画**：
//     裁剪只有 CPU 实现（§7.2），未裁剪不是丢失；
//   * tint black（暗色）未接：只用槽位色。
// =====================================================================

#include <cstdint>
#include <string>

class RHI;
class RHIRenderPass;
struct RHIViewport;

class SpineGpuRenderer
{
public:
    static SpineGpuRenderer& Get();

    // 游戏线程：请求加载（实际加载在渲染线程下一次 Draw 时做）。
    // `skeleton_path` 是 .skel.bytes / .skel / .json；图集取同名 .atlas.txt / .atlas，
    // 贴图取图集里写的页名（相对图集所在目录）。
    void RequestLoad(const std::string& skeleton_path, int anim_index);
    void RequestClear();

    // 渲染线程：在 `render_pass` 的 `subpass` 里、`viewport` 范围内画当前实例。
    // 调用方负责已 begin 了该 render pass、并处在该 subpass。
    void Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport);

    // 诊断：上一帧画了几个绘制项（`spine.status` 用）。
    uint32_t LastDrawCount() const;
    std::string Status() const;

private:
    SpineGpuRenderer();   // Impl 在构造时就建好：游戏线程与渲染线程都只读这根指针
    struct Impl;
    Impl* m_Impl {nullptr};
    Impl& impl();
};
