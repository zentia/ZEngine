#pragma once

// =====================================================================
// GPU 粒子渲染侧：容器 / 发射器 → 屏幕
// 规格：doc/GPU_PARTICLE_DESIGN.md
// ---------------------------------------------------------------------
// 与 SpineGpuRenderer 同一套分工：**核心（engine/Libraries/GPUParticle）算数据，
// 这里只负责把它搬上 GPU 并画出来**。
//   * 出生记录每条 64 B，整个环形缓冲一张 SSBO，每帧上传；
//   * 顶点一个都不在 CPU 上算：quad 的四个角来自**共享静态 VB**（角序取核心的
//     ParticleGpuQuadCorner），第 i 个 quad 的第 c 个角恒在 vertexID = i*4 + c；
//   * 位置 / 年龄 / 尺寸 / 颜色全在顶点着色器里按公式层（particle_sim.h）算。
//
// 第一版的边界（明确不做，不是忘了）：
//   * 后端：先只接 **DX12**（运行时编 HLSL，shader/hlsl/rp2/particle_gpu.*.hlsl）。
//     Vulkan / Metal 要等 SPIR-V 那一份（着色器 + 构建侧编译清单，下一步）；
//     WebGL2 不画（没有 SSBO），与 GPU-Spine 的边界一致。
//   * 上传：每帧整条环形缓冲重写（capacity × 64 B）。只传"新增那一段"的优化
//     已经把分段逻辑放进核心（ParticleGpuSpawnRuns），等有了"本帧写了哪几槽"
//     的记账再接。
//   * 不接 pass：这一版只提供 Draw()，由调用方在合适的 subpass 里调（下一步
//     接到透明 / UI subpass，与 Spine 的两个 Draw 层同款）。
//   * 排序 / 深度：不排序、不写深度（粒子互相叠加靠绘制顺序，与 Spine 的
//     透明层同款）。深度测试由调用方给的 subpass 决定。
// =====================================================================

#include "GPUParticle/ParticleGpuFormat.h"

#include <cstdint>
#include <string>
#include <vector>

// 加一个发射器要给的**全部**数据：desc 只有 96 B，曲线键 / 渐变键是独立数组
// （ParticleGpuEmulateInput 里就是分开的指针），只给 desc 的话着色器查不到
// sizeCurve / colorGradient 指向的表。
// 这一包就是烘焙产物里"一个发射器"那一份，序列化 / 加载链路将来向 Unity 靠拢时
// 直接由容器加载器填出来。
struct ParticleGpuEmitterSource
{
    ParticleGpuEmitterDesc desc {};
    std::vector<ParticleGpuCurveRange> curve_ranges;
    std::vector<ParticleGpuCurveKey> curve_keys;
    std::vector<ParticleGpuGradientRange> gradient_ranges;
    std::vector<ParticleGpuGradientColorKey> gradient_color_keys;
    std::vector<ParticleGpuGradientAlphaKey> gradient_alpha_keys;
};

class RHI;
class RHIRenderPass;
struct RHIViewport;
enum class ViewportType : uint8_t;   // RenderType.h

class ParticleGpuRenderer
{
public:
    static ParticleGpuRenderer& Get();

    // ---- 游戏线程 ----
    // 加一个发射器；返回 id（≥ 1）。source 就是烘焙产物里这一份的完整数据
    // （desc + 曲线 / 渐变）。
    // capacity 不合法（见 ParticleGpuEmitterCheckCapacity）⇒ 返回 0。
    uint32_t RequestAddEmitter(const ParticleGpuEmitterSource& source);
    // 摆放到世界：local_to_world = 16 float（列主序、列向量约定，与
    // ParticleGpuEmulateInput::model 同款）。dt 累加到下一次 Draw 才消费。
    bool RequestSetTransform(uint32_t id, const float local_to_world[16], float dt);
    bool RequestSetVisible(uint32_t id, bool visible);
    bool RequestRemoveEmitter(uint32_t id);
    void RequestClear();

    // ---- 渲染线程 ----
    // 在已 begin 的 `render_pass` 的 `subpass` 里画全部发射器。
    // view_proj / cam_right / cam_up 由调用方（pass）给：本渲染器自己不挑相机，
    // 这样同一帧能被多个视口各调一次而不用维护多份常量缓冲。
    // 每帧每个视口至多调一次。
    void Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
              ViewportType viewport_type, const float view_proj[16], const float cam_right[3],
              const float cam_up[3]);

    // ---- 诊断 ----
    uint32_t LastDrawCount() const;   // 上一帧画的 quad 数（全部发射器之和）
    std::string Status() const;

private:
    ParticleGpuRenderer();   // Impl 在构造时就建好：游戏线程与渲染线程都只读这根指针
    struct Impl;
    Impl* m_Impl {nullptr};
    Impl& impl();
};
