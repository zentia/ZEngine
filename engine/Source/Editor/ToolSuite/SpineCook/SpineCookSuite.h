#pragma once

#include "Editor/ToolSuite/ToolSuite.h"

namespace ZEditor
{
    // Spine → GPU-Spine 容器（.zspg）的导出套件。
    //
    // 为什么**只有** C++ 这一端产出容器：规格（doc/GPU_SPINE_INTERCHANGE_FORMAT.md）
    // 要的是"两端逐字节相同"，而"两端各写一遍导入器"正是漂移的温床 —— 两套解析、
    // 两套边界判断、两套报错措辞，任何一处不一致都是静默错。所以定成：
    // 容器由 C++ 侧唯一地产出，Unity(C#) 侧只**读**（CSharp/ 下那套读端）。
    // 这不是"偷懒不做 C# 导入器"，是把"同一个字节"从"两份实现各自保证"
    // 变成"一份实现 + 跨语言读回验证"。
    class SpineCookSuite final : public IToolSuite
    {
    public:
        const ToolSuiteDesc& Desc() const override;
        int RunBatch(const std::vector<std::string>& args) override;
        int RunInteractive(const std::filesystem::path& project_file,
                           const std::vector<std::string>& args) override;
    };
}   // namespace ZEditor
