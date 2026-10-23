#pragma once

// =====================================================================
// GPU-Spine：骨架文件 → 容器 / 资产（**唯一**一条加载路径）
// ---------------------------------------------------------------------
// 渲染器（加载实例）、编辑器自动 cook（Assets 下的骨架源 → 同名 .zspg）、SpineCook 批处理、
// Inspector 的动画 / 皮肤下拉框都走这里，避免"cook 出来的和运行时现烘的不是同一份字节"。
//
// 认的文件：
//   .zspg                    已烘焙容器，直接读（图集页按页名相对容器目录找）
//   <名>.skel.bytes / .skel  二进制骨架（Unity 的 spine-unity 命名是前者；OSGame 782 个全是）
//   <名>.json                json 骨架 —— **只有同名图集存在时**才当骨架（工程里大量 json 不是骨架）
// 图集：同名 `<名>.atlas` 或 `<名>.atlas.txt`；图集页 png 相对图集所在目录。
// =====================================================================

#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// 骨架源文件？（`.skel.bytes` / `.skel` / 带同名图集的 `.json`；大小写不敏感）
bool SpineGpuIsSkeletonSourcePath(const std::filesystem::path& path);
bool SpineGpuIsContainerPath(const std::filesystem::path& path);   // .zspg

// `dir/X.skel.bytes` → `dir/X`（去掉**整个**骨架后缀；replace_extension 会得到 `X.skel.zspg`）
std::filesystem::path SpineGpuStripSkeletonSuffix(const std::filesystem::path& skeleton_path);
// 同名图集（先 `.atlas` 后 `.atlas.txt`）；找不到 ⇒ false
bool SpineGpuFindAtlas(const std::filesystem::path& skeleton_path, std::filesystem::path& out_atlas);
// 骨架源 cook 出来的容器放哪：同目录同名 `.zspg`
std::filesystem::path SpineGpuCookedPathFor(const std::filesystem::path& skeleton_path);

bool SpineGpuReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
std::string SpineGpuPathToUtf8(const std::filesystem::path& path);
std::filesystem::path SpineGpuPathFromUtf8(const std::string& utf8);

// 骨架源 + 图集 → 解析 → 烘焙 → 写容器字节。`asset_out` 非空时再把字节**读回**进它（与运行时读 .zspg
// 是同一个读端 ⇒ 两条路径的运行时数据逐字节同源）。`page_names_out` 非空时给出图集页名（cook 要拷页）。
SpineImportResult SpineGpuCookSkeleton(const std::filesystem::path& skeleton_path,
                                       const std::filesystem::path& atlas_path, std::vector<uint8_t>& container_out,
                                       SpineGpuAsset* asset_out = nullptr);

// 任意一种骨架文件 → 运行时资产；`page_dir_out` = 图集页所在目录。
SpineImportResult SpineGpuLoadAssetFile(const std::filesystem::path& path, SpineGpuAsset& out,
                                        std::filesystem::path& page_dir_out);

// ---- 轻量元数据（编辑器 UI / 组件的按名查询用）----
// 按 (路径, 修改时间, 大小) 缓存；文件变了下一次查询自动重读。线程安全。
struct SpineGpuAssetInfo
{
    std::vector<std::string> animations;
    std::vector<float> durations;   // 与 animations 一一对应（秒）
    std::vector<std::string> skins; // 容器里的全部皮肤名（含 "default"，若有）
    uint32_t bone_count = 0;
    uint32_t slot_count = 0;
    std::string error;               // 非空 = 读失败（其余字段为空）
};
std::shared_ptr<const SpineGpuAssetInfo> SpineGpuGetAssetInfo(const std::filesystem::path& path);
