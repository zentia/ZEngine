#pragma once

// =====================================================================
// 曲线 / 渐变求值
// ---------------------------------------------------------------------
// 语义对齐 Unity 的 ParticleSystemCurves / ParticleSystemGradients：
//   - 曲线模式 = 常量 / 曲线 / 两常量之间随机 / 两曲线之间随机
//   - 取随机的那个 [0,1) 由**粒子自己的 seed** 决定，不是每帧重新摇：同一个
//     粒子在整个生命周期里取到的是同一个值，否则粒子会逐帧闪烁。
//   - 渐变按 Unity 的 Gradient：颜色键与 alpha 键各自独立插值，两端夹紧
//     （Unity 的 GradientMode 默认 Blend，超出范围取端点，不循环、不乒乓）。
//
// 这里只吃标准库，C# 侧（Runtime/CSharp）逐行镜像，跨语言对拍要求逐位相同。
// =====================================================================

#include "ParticleGpuFormat.h"

// 分段线性求值，两端夹紧。keys 必须按 time 升序且 time ∈ [0,1]。
// 空曲线（keyCount == 0）按常量 1 处理，避免除零与 NaN 传播到 GPU。
float ParticleGpuEvaluateCurve(const ParticleGpuCurveKey* keys, uint32_t keyCount, float t);

// 按 CurveRange 的模式求值。`random01` 是"这个粒子"的随机器（整个生命周期固定）。
float ParticleGpuEvaluateCurveRange(const ParticleGpuCurveRange& range,
                                    const ParticleGpuCurveKey* all_keys,
                                    float t,
                                    float random01);

// 渐变求值，输出 rgba（线性空间，顺序与 Unity 的 Gradient.Evaluate 一致）。
void ParticleGpuEvaluateGradient(const ParticleGpuGradientRange& range,
                                 const ParticleGpuGradientColorKey* color_keys,
                                 const ParticleGpuGradientAlphaKey* alpha_keys,
                                 float t,
                                 float out_rgba[4]);

// ---------------------------------------------------------------------
// 确定性随机：CPU / GPU / C# 三端必须给出同一个序列
// ---------------------------------------------------------------------
// lowbias32（Chris Wellons）—— 只要 uint32 的溢出是 wrap 的，任何语言都能复现。
// C# 侧必须放在 unchecked 上下文里，否则 OverflowException。
uint32_t ParticleGpuHashU32(uint32_t x);

// 原地推进 state 并返回一个 [0,1) 的 float。
float ParticleGpuRandom01(uint32_t& state);

// 由 (发射器种子, 粒子序号) 直接得到该粒子的随机种子：与发射顺序无关，
// 于是"跳过前 N 个不发射"也不会改变后面粒子的随机值。
uint32_t ParticleGpuSpawnSeed(uint32_t emitter_seed, uint64_t particle_index);
