#version 310 es

#extension GL_GOOGLE_include_directive : enable

// 带 SSAO 的延迟光照（Apple/Metal 用，见 MainCameraRp1Pass 的 CreateBuiltinFragShader）。
//
// 和 deferred_lighting.frag 是同一份主体（include/deferred_lighting_main.inl），只多
// 定义这一个宏 ⇒ 多一个 set 1 / binding 4 的 AO 采样，并在 mesh_lighting.inl 末尾把
// 遮蔽乘到环境光 / IBL / 直接光上。AO 图由 AppleSsaoPass 产出（RG8：r = 间接光
// 可见度，g = 直接光乘子）；关掉时绑 1x1 白图，结果与原版逐位一致。
#define Z_DEFERRED_SCREEN_SPACE_AO 1

#include "deferred_lighting_main.inl"
