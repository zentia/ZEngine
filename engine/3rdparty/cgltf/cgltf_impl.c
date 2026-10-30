// cgltf 的实现编译单元。
//
// cgltf.h 是单头文件库（同 ufbx / stb 的套路）：struct 与函数声明在头文件里，
// 实现必须**恰好在一个**编译单元里通过 CGLTF_IMPLEMENTATION 展开。放在独立
// 的 .c 里（而不是塞进某个 C++ 文件）有两个理由：
//
//   1. cgltf 是 C99。以 C 编译，免掉一堆 C++ 的隐式转换 / 枚举 / 指派初始化
//      告警（ZEditor 开 /W4）。
//   2. 只有这一个 TU 需要重编，改 MeshImporter 不会连带重编 7000 行解析器。
//
// 不要把这个文件加到别的目标里再编一遍 —— 会出现重复符号。
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
