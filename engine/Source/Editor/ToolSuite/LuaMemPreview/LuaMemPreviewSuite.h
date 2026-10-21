#pragma once

#include "Editor/ToolSuite/ToolSuite.h"

namespace ZEditor
{
    // Lua 内存快照查看 / CSV 导出套件。
    //
    // 原先是 engine/tools/ 下的独立 exe（WinMain + 自己的 app icon + 自己的
    // CMake target）。合成套件之后那一份删掉了：FSlateStandaloneApp 已经把
    // Win32 宿主、消息泵、DPI、字体图集都做完了，再包一层 exe 只是多一份要
    // 单独构建、单独分发的东西 —— 而"机器上装的是哪一版"正是要解决的问题。
    //
    // needs_project = false：输入输出都是命令行给的文件，跟 .zproject 无关。
    class LuaMemPreviewSuite final : public IToolSuite
    {
    public:
        const ToolSuiteDesc& Desc() const override;
        int RunBatch(const std::vector<std::string>& args) override;
        int RunInteractive(const std::filesystem::path& project_file,
                           const std::vector<std::string>& args) override;
    };
}   // namespace ZEditor
