#pragma once

// =============================================================================
// 工具套件（Tool Suite）—— 一个 ZEditor 进程承载多套工具
// -----------------------------------------------------------------------------
// 背景：`engine/tools/` 下每个 GUI 工具都是一个独立 exe（TexPreview、
// LuaMemPreview、DataTableCook…），各自带一份 Win32 宿主、消息泵、DPI 处理、
// app icon。功能重复是小事，真正的代价是**每个 exe 都要单独配置 / 单独构建 /
// 单独分发**，而且谁也不知道机器上装的是哪一版。
//
// 这里的做法（Unity Hub 那一套）：ZEditor 是唯一的可执行文件，启动时由
// Launcher 决定本次要跑哪个套件；`-batchmode` 则不弹窗、不启动引擎，
// 直接跑套件的命令行分支 —— 给 CI / 构建管线用。
//
// ⚠️ `-batchmode` 的分流点必须在 RegisterRuntime() **之前**（见 Main.cpp）。
// 编辑器那套 START_SYSTEM() 会拉起 RHI、世界、资源系统，跑一个"读文件 →
// 算 → 写文件"的烘焙器根本不需要它们，让它们初始化只是白白拖慢 CI。
//
// 哪些东西**不该**做成套件：`ZEnvironmentCollector` 是 /MT 静态链接，
// 全树里链 ZSlate 会 LNK2038（engine/CMakeLists.txt 里写了）；
// `DataTableCook` / `ZHeaderTool` 是 build-time 的，被 POST_BUILD 直接调用，
// 做成 GUI 套件反而绕远。
// =============================================================================

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ZEditor
{
    struct ToolSuiteDesc
    {
        std::string id;             // 命令行用的短名，如 "SpineCook"
        std::string display_name;   // Launcher 里显示的名字
        std::string description;    // 一句话说明
        // 需要 .zproject 才能跑。目前只有关卡编辑器需要；导入/烘焙类工具
        // 输入输出都是命令行给的文件，跟项目无关。
        bool needs_project = false;
        // 支持 `-batchmode`。不支持的套件在 batch 下会被明确拒绝并报出原因，
        // 而不是"静默开了个窗口"。
        bool supports_batch = false;
    };

    class IToolSuite
    {
    public:
        virtual ~IToolSuite() = default;

        virtual const ToolSuiteDesc& Desc() const = 0;

        // `-batchmode` 分支：无窗口、无引擎启动、无 RHI。
        // `args` 是命令行里 `-batchmode` 之后的部分（不含程序名）。返回退出码。
        virtual int RunBatch(const std::vector<std::string>& args) = 0;

        // 交互分支：建 Slate 窗口并跑消息泵。返回退出码。
        // `args` 是命令行里 `-suite=<id>` 之后的部分（`-suite=` 自身不在里面）；
        // 没有 -batchmode 时它就是"带参数打开 GUI"的那条路（例如直接给一个
        // 要打开的文件）。
        virtual int RunInteractive(const std::filesystem::path& project_file,
                                   const std::vector<std::string>& args) = 0;
    };

    // ---- 注册表 ----
    // 套件用静态对象自注册（见 RegisterToolSuite），Main.cpp 不需要知道
    // 具体有哪些套件 —— 加一个新套件只要多一个 .cpp，不用改启动代码。
    void RegisterToolSuite(std::unique_ptr<IToolSuite> suite);

    // 按注册顺序返回。Launcher 与 `-suite` 的报错信息都遍历它。
    std::vector<IToolSuite*> AllToolSuites();

    // 找不到返回 nullptr。
    IToolSuite* FindToolSuite(const std::string& id);

    // ---- 路径 ↔ UTF-8 ----
    // 套件的命令行参数一律是 UTF-8（`ZAppShell::CommandLine::Utf8()`），因为
    // CRT 的 __argv 在 ANSI 代码页上会把非 ASCII 路径 mangled —— 而路径正是
    // 烘焙器类套件唯一的输入。
    //
    // ⚠️ 不能直接 `std::filesystem::path(utf8_string)`：窄字符 path 构造用哪个
    // 代码页是实现定义的东西，赌它等于赌 CI 上那台机器。显式转换，两端都对，
    // 且不依赖 locale。
    std::filesystem::path PathFromUtf8(const std::string& utf8);
    std::string           PathToUtf8(const std::filesystem::path& path);

    // 内建套件：关卡编辑器就是 ZEditor 本体，不走 IToolSuite —— 它要
    // RegisterRuntime + RegisterEditor + START_SYSTEM() 那一整套，跟其它
    // 套件"不启动引擎"的前提相反。这里只给它一个 desc，好让 Launcher
    // 把它和其它套件列在一起。
    const ToolSuiteDesc& LevelEditorSuiteDesc();
}   // namespace ZEditor
