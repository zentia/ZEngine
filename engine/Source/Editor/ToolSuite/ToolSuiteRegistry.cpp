#include "Editor/ToolSuite/ToolSuite.h"

#include <algorithm>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace ZEditor
{
namespace
{
    // 函数内静态：套件的自注册发生在 RegisterToolSuite() 里，而那通常是一个
    // 全局对象的构造函数。用函数内静态容器而不是全局对象，是为了不受
    // "全局对象构造顺序"影响 —— 第一次访问时才构造，一定晚于任何注册。
    std::vector<std::unique_ptr<IToolSuite>>& Suites()
    {
        static std::vector<std::unique_ptr<IToolSuite>> suites;
        return suites;
    }
}   // namespace

const ToolSuiteDesc& LevelEditorSuiteDesc()
{
    static const ToolSuiteDesc kDesc {
        /* id           = */ "LevelEditor",
        /* display_name = */ "关卡编辑器",
        /* description  = */ "完整的 ZEditor：场景、资源、材质、蓝图",
        /* needs_project = */ true,
        /* supports_batch = */ false,
    };
    return kDesc;
}

void RegisterToolSuite(std::unique_ptr<IToolSuite> suite)
{
    if (suite == nullptr)
    {
        return;
    }
    // 重复 id 会让 `-suite=` 变得有歧义（先注册的赢，后注册的那个永远选不到），
    // 而且这种 bug 只在"有人复制粘贴了一个套件忘了改 id"时出现。直接换掉旧的并
    // 记一条日志，比静默忽略好 —— 忽略的话那个人会盯着 Launcher 里少了一项发呆。
    const std::string id = suite->Desc().id;
    auto it = std::find_if(Suites().begin(), Suites().end(), [&id](const std::unique_ptr<IToolSuite>& s)
    {
        return s->Desc().id == id;
    });
    if (it != Suites().end())
    {
        std::fprintf(stderr, "ZEditor: 工具套件 id '%s' 重复，后者覆盖前者。\n", id.c_str());
        *it = std::move(suite);
        return;
    }
    Suites().push_back(std::move(suite));
}

std::vector<IToolSuite*> AllToolSuites()
{
    std::vector<IToolSuite*> out;
    out.reserve(Suites().size());
    for (const auto& s : Suites())
    {
        out.push_back(s.get());
    }
    return out;
}

IToolSuite* FindToolSuite(const std::string& id)
{
    for (const auto& s : Suites())
    {
        if (s->Desc().id == id)
        {
            return s.get();
        }
    }
    return nullptr;
}

std::filesystem::path PathFromUtf8(const std::string& utf8)
{
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string PathToUtf8(const std::filesystem::path& path)
{
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
#else
    return path.string();
#endif
}
}   // namespace ZEditor
