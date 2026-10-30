// ---------------------------------------------------------------------------
// 转发头：**唯一权威在子模块里**
//   engine/Libraries/GPUSpine/shader/spine_skinning.h
// ---------------------------------------------------------------------------
// 为什么是转发而不是"留一份拷贝"：
//   公式层必须与 Unity 侧（`External/GPUSpine` 是同一个子模块）是**同一份文件**，
//   拷贝就会漂；但直接改 shader 的 include 搜索路径要动 CMake / DXC 两侧配置，
//   风险比这一行大得多。相对路径包含对 glslang（GL_GOOGLE_include_directive）
//   与 DXC 都成立，两者都按"当前文件所在目录"解析。
//
// ⚠️ 别在这里加任何公式。要改公式改子模块里那份。
// ⚠️ 依赖：cmake/ShaderCompile.cmake 会把 engine/Libraries/*/shader/ 也纳入
//    SPIR-V 的依赖 glob —— 否则改了公式不会重编（那个坑见该文件顶部的注释）。
// ---------------------------------------------------------------------------

#include "../../Libraries/GPUSpine/shader/spine_skinning.h"
