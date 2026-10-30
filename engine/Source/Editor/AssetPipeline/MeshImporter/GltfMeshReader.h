#pragma once

// glTF 2.0（.gltf / .glb）几何读取。
//
// 为什么单独成一个文件
// --------------------
// 1. **可测**：MeshImporter.cpp 在 Editor 目标里，它的 Import() 要
//    GET_SYSTEM(ObjectManager) / AssetManager —— 拉不起这些系统就没法测。
//    这里只做"文件字节 → MeshData 几何"，不碰任何系统，于是冒烟测试可以
//    单独编这一个 .cpp + 链 ZRuntime 就跑（同 FbxAnimation.h 的拆法）。
// 2. **依赖隔离**：cgltf 是 C99，只在这个 TU（和 cgltf_impl.c）里出现。
//
// 支持范围（刻意收窄，见 .cpp 的 Unsupported 处理）
// ------------------------------------------------
//   ✅ 三角形图元、POSITION / NORMAL / TEXCOORD_0、索引（含无索引的隐式顺序）
//   ✅ .gltf（外置 .bin 或 data: URI）与 .glb
//   ✅ 节点层级的世界变换（含非均匀缩放下的法线逆转置）
//   ❌ 蒙皮 / 材质 / 贴图 / 动画 / Draco —— **一律显式报错**，不静默产出错误几何
//
// 最后一条是这个文件最重要的设计约束：glTF 是个大格式，"部分支持"最容易变成
// "导入成功但模型是错的"。宁可让导入失败并说清原因。

#include "Runtime/Resource/ResType/Data/MeshData.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zgltf
{
    /// 读出 source_path 里所有三角形网格的几何，拼进 out_mesh。
    ///
    /// @param scale      单位缩放，只作用于位置（与 OBJ 导入一致）
    /// @param out_error  失败原因。失败时 out_mesh 保持未定义，调用方不要用
    /// @param out_warnings 不致命但会改变结果的降级说明（比如忽略了 morph target）
    ///
    /// 返回的 index_buffer 是**追加**到 out_mesh 现有内容之后的（顶点下标已按
    /// 追加前的 size 偏移），所以调用方可以多次调用把多个网格拼起来。
    bool ReadGltfMeshGeometry(const std::filesystem::path& source_path,
                              float scale,
                              MeshData& out_mesh,
                              std::string& out_error,
                              std::vector<std::string>& out_warnings);
}
