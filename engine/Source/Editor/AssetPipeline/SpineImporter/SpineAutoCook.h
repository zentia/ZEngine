#pragma once

// =====================================================================
// GPU-Spine 自动 cook：content 根（Assets/）下的骨架源 → 同目录同名 `.zspg`
// ---------------------------------------------------------------------
// 骨架源 = `<名>.skel.bytes` / `<名>.skel` / 带同名图集的 `<名>.json`；图集 = `<名>.atlas` / `<名>.atlas.txt`
// （与 SpineCook 批处理、渲染器现烘焙是同一个 SpineGpuCookSkeleton）。产物与源同目录 ⇒ 图集页不用拷。
//
//   * 启动：CookProject() 扫一遍，产物缺失或比骨架 / 图集旧才 cook（暖启动只 stat）；
//   * 运行：EditorAssetManager 的 m_SpineSourceWatcher 把源 / 图集的改动交给 Queue()，Flush() 在 300 ms 静默后
//     才 cook（编辑器 / 导出工具常是"写临时文件 + 改名"，一次保存会来两三个事件）；
//   * 写盘：先写 `<名>.zspg.tmp` 再改名 —— 主 watcher 只认 .zspg，看不到写了一半的文件；
//   * 写完：SpineRenderer::NotifyAssetChanged ⇒ 用它的组件下一帧重建实例（AssetRegistry 由主 watcher 刷新）。
//
// 不做：删源时删产物（留着 .zspg，日志说一声）；图集页 png 改了不触发（页在加载时才上传，重开场景即生效）。
// =====================================================================

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace SpineAutoCook
{
    struct Stats
    {
        size_t skeletons = 0;   // 找到的骨架源
        size_t cooked = 0;
        size_t up_to_date = 0;
        size_t failed = 0;
    };

    Stats CookProject(const std::filesystem::path& content_root);

    // 产物缺失或比骨架 / 图集旧？
    bool IsStale(const std::filesystem::path& skeleton_path);
    // cook 一个骨架源到同名 .zspg（原子替换）。失败 ⇒ false + error。
    bool CookOne(const std::filesystem::path& skeleton_path, std::string& error);

    // watcher 事件（创建 / 修改）：骨架源或图集
    void Queue(const std::filesystem::path& changed_path);
    void Flush(std::chrono::steady_clock::time_point now);
}  // namespace SpineAutoCook
