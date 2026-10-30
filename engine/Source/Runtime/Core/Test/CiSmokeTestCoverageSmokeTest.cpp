// =============================================================================
// CI 覆盖率门禁：一个 smoke test 只有在某个 workflow 里出现
// `-D<NAME>=ON` 才算真的在跑。
//
//   cmake -B build -DZENGINE_BUILD_CI_COVERAGE_SMOKE_TEST=ON
//   cmake --build build --target CiSmokeTestCoverageSmokeTest
//   ctest --test-dir build -C Debug -R CiSmokeTestCoverageSmokeTest
//
// 为什么需要它
// ------------
// 2026-10-30 实测：仓库里有 **91 个** `option(ZENGINE_BUILD_*SMOKE_TEST|_TESTS)`，
// 当时只有 **38 个** 被任何 workflow 以 `-D...=ON` 启用，另外 53 个从来没有在 CI 上
// 跑过一次 —— 它们能编译、本地有人跑过、但换台机器没人知道。
//
// 最典型的样本是 Lumen 那七组：smoke_tests.yml 用 `ZENGINE_USE_VULKAN=OFF`，
// 该分支把 Function/Render/Lumen/*.cpp 从 ZRuntime 里剔除，所以那边跑不了；
// 文件里的注释写着"它们由 Apple / Linux / Windows+Vulkan 宿主负责"——
// 而 Linux / macOS 宿主当时**一个测试都没接**（build_*.yml 只 build 不 ctest）。
// 于是"注释里有、实际没有"，静默了很久。
//
// 补上 smoke_tests_linux.yml 与 smoke_tests_macos.yml 后：启用 41、欠账 50。
// macOS 那份额外收掉了两个此前没人跑的 Apple 专属测试
// （APPLE_SCENE_BLIT / APPLE_SSAO）。再把剩下 50 个按平台归属分到两个宿主后
// 欠账归零：**91 / 91**。
//
// ⚠️ "启用 91" 的准确含义是 **91 个开关都被某个 workflow 传给了 CMake**，
// 不是"91 个测试都通过"。Linux / macOS 两个 job 目前是**测量**不是门禁
// （单目标编译与 ctest 走 continue-on-error），其中有多少真能在这两个宿主上
// 编过、跑过，要等首跑产物。把"有没有被尝试"和"有没有通过"混为一谈，
// 正是这一整类问题的来源。
//
// 判据必须严格
// ------------
// 只判断"名字在 workflow 文件里出现过"是不够的：上面那七组在 smoke_tests.yml
// 里**被注释提到**（"Not enabled: ZENGINE_BUILD_LUMEN_..."），按出现次数算会
// 误判成已启用。所以这里要求非注释行里出现字面量 `-D<NAME>=ON`。
//
// 而"传了 -D"也还不够 —— 见下面的 C 组：一个开关可能压根没被 option() 声明到。
//
// 双向棘轮
// --------
//   B2 已启用数 >= 91  —— 不许有人把已跑的测试从 CI 里删掉
//   B3 未启用数 <= 0   —— 不许新增一个"写了但没人跑"的测试
// 两个方向都要守：只守一个，另一个方向就能静默退化。欠账已压到 0，所以 B3
// 现在是"新增测试开关必须同时接进 CI"，不再是"慢慢还债"。
// =============================================================================

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-4s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s\n", id, what);
        }
    }

    std::string ReadFileText(const std::filesystem::path& in_path)
    {
        std::ifstream in(in_path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool IsIdentifierChar(char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    /// Build directories and 3rdparty are not ours: scanning them produces
    /// hundreds of unrelated options and would drown the real signal.
    bool IsScanCandidate(const std::filesystem::path& in_path)
    {
        const std::string generic = in_path.generic_string();
        if (generic.find("/3rdparty/") != std::string::npos)
        {
            return false;
        }
        for (const auto& part : in_path)
        {
            const std::string p = part.generic_string();
            if (p == "build" || p.compare(0, 6, "build-") == 0)
            {
                return false;
            }
        }
        return true;
    }

    /// `option(ZENGINE_BUILD_FOO_SMOKE_TEST` -> ZENGINE_BUILD_FOO_SMOKE_TEST
    std::set<std::string> CollectTestOptions(const std::filesystem::path& in_engine_root)
    {
        std::set<std::string> found;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(in_engine_root, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (!it->is_regular_file(ec) || it->path().filename() != "CMakeLists.txt")
            {
                continue;
            }
            if (!IsScanCandidate(it->path()))
            {
                it.disable_recursion_pending();
                continue;
            }

            const std::string text = ReadFileText(it->path());
            size_t at = 0;
            while ((at = text.find("option(", at)) != std::string::npos)
            {
                size_t name_begin = at + 7;
                while (name_begin < text.size() && std::isspace(static_cast<unsigned char>(text[name_begin])))
                {
                    ++name_begin;
                }
                size_t name_end = name_begin;
                while (name_end < text.size() && IsIdentifierChar(text[name_end]))
                {
                    ++name_end;
                }
                const std::string name = text.substr(name_begin, name_end - name_begin);
                at = name_end;

                const bool is_test = name.size() > 14 &&
                                     (name.compare(name.size() - 10, 10, "SMOKE_TEST") == 0 ||
                                      name.compare(name.size() - 6, 6, "_TESTS") == 0);
                if (is_test && name.compare(0, 14, "ZENGINE_BUILD_") == 0)
                {
                    found.insert(name);
                }
            }
        }
        return found;
    }

    /// Flags really passed to CMake: `-D<NAME>=ON` on a non-comment line.
    std::set<std::string> CollectEnabledFlags(const std::filesystem::path& in_workflow_dir)
    {
        std::set<std::string> enabled;
        std::error_code ec;
        if (!std::filesystem::is_directory(in_workflow_dir, ec))
        {
            return enabled;
        }
        for (std::filesystem::directory_iterator it(in_workflow_dir, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            const std::string ext = it->path().extension().generic_string();
            if (ext != ".yml" && ext != ".yaml")
            {
                continue;
            }
            const std::string text = ReadFileText(it->path());
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
            {
                // CRLF: getline leaves the '\r' behind, and comparing "=ON\r"
                // against "=ON" silently fails -- a whole workflow then reads as
                // zero flags and the ratchet loses its lower bound. Found by
                // mutation: rewriting a .yml with Python (CRLF on Windows)
                // dropped the Linux job's 37 flags to 0.
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                const size_t first = line.find_first_not_of(" \t\r");
                if (first == std::string::npos || line[first] == '#')
                {
                    continue;
                }
                size_t at = 0;
                while ((at = line.find("-DZENGINE_BUILD_", at)) != std::string::npos)
                {
                    const size_t name_begin = at + 2;
                    size_t name_end = name_begin;
                    while (name_end < line.size() && IsIdentifierChar(line[name_end]))
                    {
                        ++name_end;
                    }
                    if (name_end < line.size() && line.compare(name_end, 4, "=ON") == 0)
                    {
                        enabled.insert(line.substr(name_begin, name_end - name_begin));
                    }
                    at = name_end;
                }
            }
        }
        return enabled;
    }

    /// File-scope `if(NOT <VAR>)\n    return()\nendif()` makes every *later*
    /// option in the same file unreachable unless <VAR> is on.
    ///
    /// This is not a style nit. Found 2026-10-30 while clearing the never-run
    /// backlog: Render/Test/CMakeLists.txt declares SPOTLIGHT at :337 and
    /// returns at :342, so the option() for SHADER_REGISTRY at :392 never runs.
    /// Every workflow passes -DZENGINE_BUILD_SHADER_REGISTRY_SMOKE_TEST=ON and
    /// CMake answers "Manually-specified variables were not used by the
    /// project" -- the target is never created and the suite is quietly one
    /// test short. Same shape in Gas, Lumen and UMG: 16 options total.
    ///
    /// Returns option name -> set of guard variables that must also be on.
    /// Operates on text so the self-check below can feed it a fixture with a
    /// known answer instead of depending on the tree happening to contain one.
    void ParseShadowsFromText(const std::string& text,
                              std::map<std::string, std::set<std::string>>& out_shadowed)
    {
        std::istringstream lines(text);
        std::string line;
        std::string pending;
        std::set<std::string> guards;  // grows monotonically down the file
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#')
            {
                continue;
            }
            const std::string s = line.substr(first);
            const bool at_file_scope = (first == 0);

            // `return()` is indented inside the if-block, so indentation cannot
            // be used to skip lines here -- doing that is exactly how the first
            // version of this parser reported 0 shadows in a tree that had 16.
            if (at_file_scope && s.compare(0, 7, "if(NOT ") == 0 && s.back() == ')')
            {
                const std::string var = s.substr(7, s.size() - 8);
                bool simple = !var.empty();
                for (const char c : var)
                {
                    if (!IsIdentifierChar(c))
                    {
                        simple = false;
                    }
                }
                pending = simple ? var : std::string();
                continue;
            }
            // The guard only counts if return() is the very next statement;
            // any other line closes the window (a real if-block, endif, ...).
            if (s == "return()" && !pending.empty())
            {
                guards.insert(pending);
            }
            pending.clear();

            if (at_file_scope && s.compare(0, 7, "option(") == 0 && !guards.empty())
            {
                size_t name_begin = 7;
                while (name_begin < s.size() && std::isspace(static_cast<unsigned char>(s[name_begin])))
                {
                    ++name_begin;
                }
                size_t name_end = name_begin;
                while (name_end < s.size() && IsIdentifierChar(s[name_end]))
                {
                    ++name_end;
                }
                if (name_end > name_begin)
                {
                    out_shadowed[s.substr(name_begin, name_end - name_begin)].insert(guards.begin(), guards.end());
                }
            }
        }
    }

    std::map<std::string, std::set<std::string>> CollectShadowedOptions(
        const std::filesystem::path& in_engine_root, size_t& out_files_scanned)
    {
        std::map<std::string, std::set<std::string>> shadowed;
        out_files_scanned = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(in_engine_root, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (!it->is_regular_file(ec) || it->path().filename() != "CMakeLists.txt")
            {
                continue;
            }
            if (!IsScanCandidate(it->path()))
            {
                it.disable_recursion_pending();
                continue;
            }
            ++out_files_scanned;
            ParseShadowsFromText(ReadFileText(it->path()), shadowed);
        }
        return shadowed;
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("\n=== CiSmokeTestCoverage smoke test ===\n\n");

    const std::filesystem::path repo_root = ZENGINE_REPO_ROOT_DIR;
    const std::filesystem::path engine_root = ZENGINE_ENGINE_ROOT_DIR;
    const std::filesystem::path workflow_dir = repo_root / ".github" / "workflows";

    // ---- A. 扫描覆盖率：扫不到东西的话后面全是空跑 ----
    std::printf("-- A. scan coverage --\n");
    std::error_code ec;
    Check(std::filesystem::is_directory(engine_root, ec), "A1", "工程根存在");
    Check(std::filesystem::is_directory(workflow_dir, ec), "A2", ".github/workflows 存在");

    const std::set<std::string> options = CollectTestOptions(engine_root);
    Check(options.size() >= 50u, "A3", "扫到的测试开关数量（防空扫描）");

    const std::set<std::string> enabled = CollectEnabledFlags(workflow_dir);
    Check(!enabled.empty(), "A4", "扫到至少一条 -D...=ON");

    size_t cmakelists_scanned = 0;
    const auto shadowed = CollectShadowedOptions(engine_root, cmakelists_scanned);
    // 同一个"空扫描"陷阱：结构分析扫不到文件的话 C1 会安静地永远绿。
    Check(cmakelists_scanned >= 50u, "A5", "结构分析扫到的 CMakeLists 数量（防空扫描）");

    // 正向对照：喂一段**已知答案**的 CMake。树里将来可能被清理干净（那时
    // shadowed 为空是正确结果），所以不能拿"树里有几处遮蔽"当健康检查 ——
    // 夹具才是。第一版解析器就是因为缩进的 return() 被跳过而对 16 处遮蔽
    // 报告 0，且当时所有其它检查都是绿的。
    {
        const std::string fixture =
            "# comment: if(NOT ZENGINE_BUILD_FAKE_SMOKE_TEST)\n"
            "option(ZENGINE_BUILD_A_SMOKE_TEST \"a\" OFF)\n"
            "if(NOT ZENGINE_BUILD_A_SMOKE_TEST)\n"
            "    return()\n"
            "endif()\n"
            "option(ZENGINE_BUILD_B_SMOKE_TEST \"b\" OFF)\n"
            "if(NOT ZENGINE_BUILD_B_SMOKE_TEST)\n"
            "    return()\n"
            "endif()\n"
            "option(ZENGINE_BUILD_C_SMOKE_TEST \"c\" OFF)\n";
        std::map<std::string, std::set<std::string>> got;
        ParseShadowsFromText(fixture, got);
        // B sits behind A's return; C behind both. A is before any return.
        const bool a_ok = got.count("ZENGINE_BUILD_A_SMOKE_TEST") == 0;
        const bool b_ok = got.count("ZENGINE_BUILD_B_SMOKE_TEST") == 1 &&
                          got["ZENGINE_BUILD_B_SMOKE_TEST"].count("ZENGINE_BUILD_A_SMOKE_TEST") == 1;
        const bool c_ok = got.count("ZENGINE_BUILD_C_SMOKE_TEST") == 1 &&
                          got["ZENGINE_BUILD_C_SMOKE_TEST"].size() == 2;
        // A comment mentioning if(NOT ...) must not be mistaken for a guard.
        Check(a_ok && b_ok && c_ok, "A6", "结构解析器自检（自带夹具，与树的内容无关）");
    }
    {
        // 复合条件（Runtime/CMakeLists.txt 里 VT_* 那一类）解析器**故意不建模**：
        // 猜错方向就是误报，C1 不能狼来了。
        const std::string fixture =
            "if(NOT (WIN32 AND NOT ZENGINE_USE_VULKAN))\n"
            "    return()\n"
            "endif()\n"
            "option(ZENGINE_BUILD_D_SMOKE_TEST \"d\" OFF)\n";
        std::map<std::string, std::set<std::string>> got;
        ParseShadowsFromText(fixture, got);
        Check(got.empty(), "A7", "复合条件的 return() 不误报为遮蔽");
    }

    // ---- B. 双向棘轮 ----
    std::printf("\n-- B. ratchet --\n");

    size_t enabled_known = 0;
    std::vector<std::string> phantom;
    for (const auto& name : enabled)
    {
        if (options.count(name) != 0)
        {
            ++enabled_known;
        }
        else
        {
            phantom.push_back(name);
        }
    }

    // 一个拼错的 -D 不会让 CMake 报错，只会让那个测试静默地不跑。
    Check(phantom.empty(), "B1", "workflow 里没有指向不存在开关的 -D...=ON");
    for (const auto& name : phantom)
    {
        std::printf("       phantom flag: %s\n", name.c_str());
    }

    Check(enabled_known >= 91u, "B2", "被 CI 真正启用的测试开关数（只许增）");

    std::vector<std::string> unenabled;
    for (const auto& name : options)
    {
        if (enabled.count(name) == 0)
        {
            unenabled.push_back(name);
        }
    }

    Check(unenabled.size() <= 0u, "B3", "从未在任何 CI 上跑过的测试开关数（只许减）");

    // ---- C. 结构可达性：启用了但被前面的 return() 压住 = 假启用 ----
    std::printf("\n-- C. reachability --\n");

    std::vector<std::string> dead;
    for (const auto& [name, guards] : shadowed)
    {
        // 只关心 workflow 真的开了的那些：没开的本来就没人以为它在跑。
        if (enabled.count(name) == 0)
        {
            continue;
        }
        for (const auto& guard : guards)
        {
            // WIN32 / APPLE 之类的平台变量不在此列，只看开关之间的互相遮蔽。
            if (guard.compare(0, 14, "ZENGINE_BUILD_") != 0)
            {
                continue;
            }
            if (enabled.count(guard) == 0)
            {
                dead.push_back(name + "  (被 " + guard + " 遮蔽，而 " + guard + " 从未启用)");
            }
        }
    }

    Check(dead.empty(), "C1", "被 workflow 启用的开关没有被前面的 return() 压死");
    for (const auto& line : dead)
    {
        std::printf("       dead: %s\n", line.c_str());
    }

    std::printf("       结构性遮蔽 %zu 处（一个文件里只开后面的选项 = 静默不生效）：\n", shadowed.size());
    for (const auto& [name, guards] : shadowed)
    {
        if (name.compare(0, 14, "ZENGINE_BUILD_") != 0)
        {
            continue;
        }
        for (const auto& guard : guards)
        {
            std::printf("       %-56s <- %s\n", name.c_str(), guard.c_str());
        }
    }

    std::printf("\n   CI 启用 %zu / 仓库共 %zu；欠账 %zu：\n", enabled_known, options.size(), unenabled.size());
    for (const auto& name : unenabled)
    {
        std::printf("       %s\n", name.c_str());
    }

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
