#version 310 es

#extension GL_GOOGLE_include_directive : enable

// 延迟光照的主体在 include/deferred_lighting_main.inl —— 和 deferred_lighting_ssao.frag
// 共用同一份，避免两份延迟光照各自漂移。本文件不定义 Z_DEFERRED_SCREEN_SPACE_AO，
// 编出来的 SPIR-V 与拆分前逐字节一致（Vulkan 的 MainCameraPass / 非 Metal 后端用它）。
#include "deferred_lighting_main.inl"
